"""Coherent GGAK (Elektro-L5, 1693 MHz) frame decoder core.

Chain: cu8 IQ @1.024 Msps -> decimate to ~24.4 kHz -> residual-carrier PLL -> PM message
-> resample to 16 samples/bit -> per-bit integrate-and-dump against one subcarrier cycle
(subcarrier = bit clock, 4999.90 Hz) -> ASM search -> per-frame timing search with a fixed,
ASM-anchored phase (no tracking loop, so no mid-frame polarity flips) -> 16-bit sum checksum.
"""
import sys, numpy as np
from fractions import Fraction
from scipy.signal import fftconvolve, resample_poly, firwin, upfirdn
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ggak_common as v4
from repair import repair as _repair
REPAIR = os.environ.get("GGAK_REPAIR", "1") == "1"   # gated 1-bit soft repair, see repair.py
FRONT = os.environ.get("GGAK_FRONT", "upfirdn")     # "upfirdn" (fast) or "fft" (original)
CARRIER = os.environ.get("GGAK_CARRIER", "pll")
REPAIR40 = os.environ.get("GGAK_REPAIR40", "1") == "1"    # "block" (vectorised) or "pll" (sample loop, slow)
BLK = int(os.environ.get("GGAK_BLK", "244"))          # carrier block, samples at ~24.4 kHz (244 = 10 ms)

FS0 = 1024000.0
RB = 4999.90                      # bit rate = 1792 bits x 2.790123 Hz frame rate
NS = 16                           # samples per bit after resampling
FB = v4.FBITS                     # 1792 bits per frame
ASMB = v4.ASM_BITS.astype(np.uint8)
ASMC = 1.0 - 2.0 * ASMB.astype(np.float64)
EPOCH_MSK = 946684800 - 10800     # onboard clock: s since 2000-01-01 00:00 Moscow time
DEC = int(FS0 // 24000)
LPF = firwin(129, 0.9 / DEC).astype(np.float32)
_TW = np.exp(-2j * np.pi * np.arange(NS) / NS)


def message(z, pll_bw=0.005):
    """Complex IQ (float32 pairs as complex64) -> unit-variance PM message at NS*RB, plus fc, FSD."""
    if FRONT == "upfirdn":
        z = upfirdn(LPF, z, down=DEC)          # decimating FIR: only the kept samples are computed
    else:
        z = fftconvolve(z, LPF, mode="same")[::DEC]
    fs = FS0 / DEC
    z = z - z.mean()
    n = len(z)
    q = z * z
    Q = np.abs(np.fft.fftshift(np.fft.fft(q * np.hanning(n))))
    fq = np.fft.fftshift(np.fft.fftfreq(n, 1 / fs))
    m = (np.abs(fq) < 800) & (np.abs(fq) > 3)
    fc = fq[m][np.argmax(Q[m])] / 2
    zz = z * np.exp(-2j * np.pi * fc / fs * np.arange(n))
    if CARRIER == "block":
        # Vectorised carrier tracking: the residual carrier is the block mean of the derotated signal
        # (the PM sidebands average out over a block). Phase per BLK-sample block, unwrapped, interpolated.
        nb = n // BLK
        ph = np.unwrap(np.angle(zz[:nb * BLK].reshape(nb, BLK).mean(1)))
        phi = np.interp(np.arange(n), (np.arange(nb) + 0.5) * BLK, ph)
        msg = (zz * np.exp(-1j * phi)).imag.astype(np.float64)
    else:
        pll = v4.residual_pll(zz.astype(np.complex128), pll_bw)
        msg = pll.imag.astype(np.float64)
    msg -= msg.mean()
    fr = Fraction(NS * RB / fs).limit_denominator(2000)
    m2 = resample_poly(msg, fr.numerator, fr.denominator)
    return m2 / (np.std(m2) + 1e-12), fc, fs * fr.numerator / fr.denominator


def bits_at(m2, sps, start, nbits):
    k = np.arange(nbits)[:, None]
    j = np.arange(NS)[None, :]
    pos = start + k * sps + j * (sps / NS)
    i0 = np.floor(pos).astype(np.int64)
    f = pos - i0
    ok = (i0 >= 0) & (i0 + 1 < len(m2))
    i0 = np.clip(i0, 0, len(m2) - 2)
    x = (m2[i0] * (1 - f) + m2[i0 + 1] * f) * ok
    return (x * _TW[None, :]).sum(1) / NS


def checksum_ok(by):
    return (int(by[:222].astype(np.uint32).sum()) & 0xFFFF) == ((int(by[222]) << 8) | int(by[223]))


def _frame_at(m2, sps, start):
    yy = bits_at(m2, sps, start, FB)
    a = np.sum(yy[:32] * ASMC)
    r = (yy * np.exp(-1j * np.angle(a))).real
    return np.mean(np.abs(r)) / np.sqrt(np.mean(np.abs(yy) ** 2) + 1e-12), r


def decode(z, ncorr_min=0.6):
    """Decode all frames whose start lies in z. Returns (frames, fc). Each frame dict has
    start_s (offset of frame start into z, seconds), ok, asm_err, eye, snr_db, bytes (np.uint8[224])."""
    m2, fc, fsd = message(z)
    sps = fsd / RB
    nb = int((len(m2) - 2 * sps) / sps)
    y = bits_at(m2, sps, 0.0, nb)
    c = np.abs(np.correlate(y, ASMC.astype(np.complex128), mode="valid"))
    norm = np.convolve(np.abs(y), np.ones(32), mode="valid")[:len(c)] + 1e-12
    nc = c / norm
    peaks = []
    for p in np.argsort(nc)[::-1]:
        if nc[p] < ncorr_min:
            break
        if all(abs(p - q) > FB // 2 for q in peaks):
            peaks.append(int(p))
    out = []
    for p in sorted(peaks):
        if p + FB + 2 > nb:
            continue
        best = None
        for o in np.linspace(-sps, sps, 17):
            e, r = _frame_at(m2, sps, p * sps + o)
            if best is None or e > best[0]:
                best = (e, r, o)
        o0 = best[2]
        for o in np.linspace(o0 - sps / 8, o0 + sps / 8, 5):
            e, r = _frame_at(m2, sps, p * sps + o)
            if e > best[0]:
                best = (e, r, o)
        e, r, o = best
        bits = (r < 0).astype(np.uint8)
        by = np.packbits(bits)
        blk = np.abs(r[:FB - FB % 128]).reshape(-1, 128)
        snr = float(np.median(20 * np.log10(blk.mean(1) / (blk.std(1) + 1e-12))))
        ok = checksum_ok(by); nrep = 0
        if not ok and REPAIR:
            rb, nrep = _repair(r, ASMB, T=0.25, w=1)
            if rb is not None:
                by = np.packbits(rb); ok = True
            elif REPAIR40 and by[4] == 0x40:
                # GALS frames only: a wider repair (up to 2 flips of bits below 0.35 x median confidence,
                # unique solution) is accepted only if the repaired frame also passes the GALS structure check.
                rb, n2 = _repair(r, ASMB, T=0.35, w=2)
                if rb is not None and gals_ok(np.packbits(rb)):
                    by = np.packbits(rb); ok = True; nrep = 10 + n2
        out.append(dict(start_s=(p * sps + o) / fsd, ok=ok, rep=nrep, asm_err=int((bits[:32] != ASMB).sum()),
                        eye=float(e), snr_db=snr, bytes=by))
    return out, fc


def gals_ok(by):
    """Type-0x40 structure: 11 readings x 19 B from byte 13, each 0xA0/0xA1, BE16 sequence +2 per reading,
    and nested counters ch1 >= ch2 >= ch3."""
    if by[4] != 0x40:
        return False
    seq = []
    for i in range(11):
        x = by[13 + 19 * i: 32 + 19 * i]
        if x[0] not in (0xA0, 0xA1):
            return False
        seq.append((int(x[1]) << 8) | int(x[2]))
        c = [(int(x[3 + 2 * k]) << 8) | int(x[4 + 2 * k]) for k in range(3)]
        if not (c[0] >= c[1] >= c[2]):
            return False
    return all((b - a) % 65536 == 2 for a, b in zip(seq, seq[1:]))


def parse(by):
    """Header fields that are structurally known (L5): marker nibble, onboard clock."""
    marker = int(by[4] >> 4)
    clock = int.from_bytes(bytes(by[9:13]), "big") if marker != 7 else None
    return marker, clock

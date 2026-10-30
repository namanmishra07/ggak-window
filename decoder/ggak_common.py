"""Frame constants and the residual-carrier PLL shared by the decoder.

Vendored from our earlier clean-room demodulator (approach v4), which followed SatDump's
elektro_ggak pipeline: 224-byte frames, ASM 0x1ACFFC1D, residual-carrier PM.
"""
import math
import numpy as np

ASM_WORD = 0x1ACFFC1D
FLEN = 224
FBITS = FLEN * 8                       # 1792
ASM_BITS = np.array([int(b) for b in format(ASM_WORD, "032b")], np.int8)


def residual_pll(z, loop_bw=0.02, zeta=0.707):
    """2nd-order residual-carrier PLL, atan2 phase detector. Gains exactly as in
    SatDump's PLLCarrierTrackingBlock. Derotates the pilot onto REAL."""
    theta = loop_bw
    denom = 1.0 + 2.0 * zeta * theta + theta * theta
    alpha = (4.0 * zeta * theta) / denom     # proportional
    beta = (4.0 * theta * theta) / denom      # integral
    zr = z.real.tolist()                      # python floats: fast scalar trig
    zi = z.imag.tolist()
    N = len(z)
    out_r = [0.0] * N
    out_i = [0.0] * N
    cos = math.cos
    sin = math.sin
    atan2 = math.atan2
    phase = 0.0
    freq = 0.0
    for i in range(N):
        c = cos(phase)
        s = sin(phase)
        zri = zr[i]
        zii = zi[i]
        sr = zri * c + zii * s               # derotate by -phase
        si = -zri * s + zii * c
        out_r[i] = sr
        out_i[i] = si
        err = atan2(si, sr)
        freq += beta * err
        phase += alpha * err + freq
    return np.array(out_r) + 1j * np.array(out_i)

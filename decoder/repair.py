"""Soft-decision single/double-bit repair for GGAK frames, gated for honesty.

The 16-bit additive checksum is weak, so a repair is accepted only if: the 32 ASM bits are forced
to their known value; at most `w` bits flip; every flipped bit is a low-confidence decision
(|soft| < T x median); and exactly ONE such flip set satisfies the checksum. Measured on known
fill frames + a permuted-content null test before use (see analysis/repair_validation.txt)."""
import itertools, numpy as np

_W = np.tile(1 << np.arange(7, -1, -1), 224).astype(np.int64)
_C = _W.copy(); _C[222 * 8:223 * 8] = -(_W[222 * 8:223 * 8] << 8); _C[223 * 8:] = -_W[223 * 8:]


def residual(bits):
    by = np.packbits(bits)
    return (int(by[:222].astype(np.int64).sum()) - ((int(by[222]) << 8) | int(by[223]))) % 65536


def repair(soft, asm_bits, T=0.25, w=2, L=24):
    """soft: real per-bit values (negative = 1). Returns (bits, nflips) or (None, 0)."""
    bits = (soft < 0).astype(np.uint8)
    bits[:32] = asm_bits
    d0 = residual(bits)
    if d0 == 0:
        return bits, 0
    rel = np.abs(soft) / (np.median(np.abs(soft)) + 1e-12)
    rel[:32] = np.inf
    cand = [int(j) for j in np.argsort(rel)[:L] if rel[j] < T]
    dl = np.where(bits == 1, -1, 1) * _C
    sols = [cb for k in range(1, w + 1) for cb in itertools.combinations(cand, k)
            if (d0 + int(dl[list(cb)].sum())) % 65536 == 0]
    if len(sols) != 1:
        return None, 0
    bits = bits.copy(); bits[list(sols[0])] ^= 1
    return bits, len(sols[0])

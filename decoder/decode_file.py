#!/usr/bin/env python3
"""Decode GGAK frames from an RTL-SDR recording (cu8, 1.024 Msps, tuned to 1693 MHz).

    python3 decoder/decode_file.py samples/ggak_l5_20261030_1229utc_1024ksps.cu8

Prints one line per checksum-clean frame: offset, frame type, the satellite's onboard clock,
and for particle-counter frames (type 0x40) the eleven one-second readings (raw values are
doubled counts: see analysis/poisson_check.py).
"""
import sys, datetime
import numpy as np
import ggak_core as g
from ggak_pub import gals_readings, EPOCH_MSK


def main(path):
    raw = np.fromfile(path, np.uint8)
    z = (raw[0::2].astype(np.float32) - 127.5) + 1j * (raw[1::2].astype(np.float32) - 127.5)
    print(f"{path}: {len(z) / g.FS0:.1f} s of IQ")
    frames, fc = g.decode(z)
    seen = set()
    clean = 0
    for f in frames:
        if not f["ok"]:
            continue
        by = bytes(f["bytes"])
        if by in seen:
            continue
        seen.add(by)
        clean += 1
        if by[4] >> 4 == 7:                     # marker 7 frames carry no clock
            t = "(none in this frame type)"
        else:
            clk = int.from_bytes(by[9:13], "big") + g.EPOCH_MSK
            t = datetime.datetime.fromtimestamp(clk, datetime.timezone.utc).strftime("%H:%M:%S UTC")
        line = f"{f['start_s']:6.2f} s  type 0x{by[4]:02x}  onboard clock {t}"
        if by[4] == 0x40:
            rd = gals_readings(by)
            if rd:
                line += "  p600 " + " ".join(str(v[0]) for v in rd)
        print(line + ("  (repaired)" if f.get("rep") else ""))
    print(f"{clean} unique checksum-clean frames")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])

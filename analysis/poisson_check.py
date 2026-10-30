#!/usr/bin/env python3
"""Are the L5 counter values real particle counts?

For Poisson counts, variance = mean. If every value is reported doubled, variance = 2 x mean
and no value is odd. Uses clean (unrepaired) seconds in 10-minute blocks.

    python3 analysis/poisson_check.py data/ggak-l5-*.jsonl.gz
"""
import sys, gzip, json, collections, statistics as st

blocks = collections.defaultdict(list)
for path in sys.argv[1:]:
    for line in gzip.open(path, "rt"):
        try:
            x = json.loads(line)
        except ValueError:                # a few lines were cut by a power loss
            continue
        if x.get("src") != "live" or x.get("p600") is None or x.get("rep"):
            continue
        blocks[int(x["rx"] // 600)].append((x["p600"], x["p800"], x["p1100"]))

odd = sum(1 for v in blocks.values() for s in v for c in s if c % 2)
for i, name in enumerate(["p600", "p800", "p1100"]):
    r = [st.pvariance([s[i] for s in v]) / st.mean([s[i] for s in v])
         for v in blocks.values() if len(v) >= 150 and st.mean([s[i] for s in v]) > 0]
    print(f"{name}: variance/mean = {st.median(r):.2f} (median of {len(r)} ten-minute blocks)")
print(f"odd values: {odd}")

#!/usr/bin/env python3
"""Does the L5 'protons > 600 MeV' channel track protons > 500 MeV seen by GOES?

Hourly means of clean L5 seconds vs NOAA GOES-18 integral protons >= 500 MeV and
GOES-19 electrons >= 2 MeV (SWPC 7-day JSON snapshots in data/, public domain).
Prints each series' range as % of its own mean, and correlations at time shifts
(GOES-19 sits ~150 deg west of Elektro-L5, about 10 h apart in local time).

    python3 analysis/goes_check.py data/ggak-l5-*.jsonl.gz
"""
import sys, gzip, json, math, collections, datetime as dt

def goes_hourly(path, energy=None):
    h = collections.defaultdict(list)
    for x in json.load(gzip.open(path, "rt")):
        if x["flux"] is None or (energy and x["energy"] != energy):
            continue
        t = dt.datetime.fromisoformat(x["time_tag"].replace("Z", "+00:00")).timestamp()
        h[int(t // 3600) * 3600].append(x["flux"])
    return {k: sum(v) / len(v) for k, v in h.items()}

l5 = collections.defaultdict(list)
for path in sys.argv[1:]:
    for line in gzip.open(path, "rt"):
        try:
            x = json.loads(line)
        except ValueError:                # a few lines were cut by a power loss
            continue
        if x.get("src") == "live" and x.get("p600") is not None and not x.get("rep"):
            l5[int(x["rx"] // 3600) * 3600].append(x["p600"])
l5 = {h: sum(v) / len(v) for h, v in l5.items() if len(v) >= 300}

prot = goes_hourly("data/goes-integral-protons-7-day-20261030.json.gz", ">=500 MeV")
elec = goes_hourly("data/goes-integral-electrons-7-day-20261030.json.gz")
hours = sorted(h for h in l5 if h in prot)

def pct_range(xs):
    m = sum(xs) / len(xs)
    return min(xs) / m * 100, max(xs) / m * 100

def corr(a, b):
    ma, mb = sum(a) / len(a), sum(b) / len(b)
    return sum((x - ma) * (y - mb) for x, y in zip(a, b)) / math.sqrt(sum((x - ma) ** 2 for x in a) * sum((y - mb) ** 2 for y in b))

print(f"{len(hours)} hours with >= 300 clean L5 seconds")
print("L5 p600 range: %.0f-%.0f %% of mean" % pct_range([l5[h] for h in hours]))
print("GOES >=500 MeV protons range: %.0f-%.0f %% of mean" % pct_range([prot[h] for h in hours]))
print("r(L5, GOES protons) = %+.2f" % corr([l5[h] for h in hours], [prot[h] for h in hours]))
for s in (0, 6, 10, -9):
    hs = [h for h in hours if h + s * 3600 in elec]
    print("r(log L5, log GOES-19 e>2MeV shifted %+d h) = %+.2f (n=%d)" % (
        s, corr([math.log(l5[h]) for h in hs], [math.log(elec[h + s * 3600]) for h in hs]), len(hs)))

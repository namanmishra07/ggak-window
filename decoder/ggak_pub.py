#!/usr/bin/env python3
"""GGAK publisher: tails data/frames-*.jsonl from ggak_live.py and publishes one message per second
to ggak/l5/sec (plus retained ggak/l5/status every 10 s), and archives every message to
archive/ggak-l5-YYYYMMDD.jsonl. Each message covers one received second (field "rx"), published
DELAY seconds later so the decoder's 5 s windows have landed. Runs in dry-run (archive only) when
mqtt.json is absent.

Proton counts: p600/p800/p1100/mip come from GALS readings in checksum-clean type-0x40 frames
(11 readings x 19 B from byte 13: 0xA0/0xA1, BE16 sequence, 8 x BE16 = ch1..ch3, Cg-1..Cg-4, MIP), layout
per SatDump PR #1143 and confirmed on our own frames, but the channel meaning is NOT validated
(no official L5 data exists): status carries map="pr1143-unvalidated". Values are RAW (all even;
true counts may be raw/2). Reading i is assigned to UTC second (frame clock - 10 + i). Seconds with
no clean 0x40 coverage publish null, never 0. See analysis/gals_mapping.md."""
import os, json, time, glob, ssl, datetime, statistics, collections
HERE = os.path.dirname(os.path.abspath(__file__))
DATA, ARCH = os.path.join(HERE, "data"), os.path.join(HERE, "archive")
CFG = os.path.join(HERE, "mqtt.json")
TOPIC_SEC, TOPIC_STATUS = "ggak/l5/sec", "ggak/l5/status"
TOPIC_REPLAY = "ggak/l5/replay"   # retained: last 24 h of particles/s in 10-s slots, for receivers' fallback
DELAY = 20               # a 0x40 frame lands ~12 s after its first reading, plus the 5 s decode hop
SW = "ggak_pub 0.7"
MAP = "pr1143-unvalidated"
EPOCH_MSK = 946684800 - 10800
os.makedirs(ARCH, exist_ok=True)


def log(m):
    print(time.strftime("%H:%M:%S"), m, flush=True)


def utc_day(t):
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%Y%m%d")


class Tail:
    """Follows data/frames-<UTC day>.jsonl across day rollovers."""
    def __init__(self):
        self.day = None; self.fh = None
    def lines(self):
        day = utc_day(time.time())
        if day != self.day:
            if self.fh:
                for l in self.fh:          # drain yesterday's tail
                    yield l
                self.fh.close(); self.fh = None
            self.day = day
        if self.fh is None:
            p = os.path.join(DATA, f"frames-{day}.jsonl")
            if not os.path.exists(p):
                return
            self.fh = open(p); self.fh.seek(0, 2)
        for l in self.fh:
            yield l


# SKL frames (type 0x3c): 3 x 69-byte records, each starting 0x9c, then a 16-bit record number.
# Candidate proton-telescope fields (2026-10-30, 18 h of records): bytes 43 and 47 fire together
# (27 of 28), byte 37 fires only with them, byte 15 fires alone; each ~1 count per 7-23 min.
# Treated as CANDIDATES: one particle = max(b43, b47) + b15. Unvalidated until a proton event.
PC_COINC = (43, 47)
PC_SINGLE = 15


def skl_records(by):
    """[(record number, candidate count)] from a type-0x3c frame; records with an impossible value are skipped."""
    out = []
    for i in range(3):
        r = by[13 + 69 * i: 13 + 69 * (i + 1)]
        if len(r) < 69 or r[0] != 0x9C:
            continue
        if any(r[o] >= 8 for o in (*PC_COINC, PC_SINGLE, 37)):
            continue                      # stray bit error (weak checksum), not a count
        out.append((int.from_bytes(r[1:3], "big"), max(r[PC_COINC[0]], r[PC_COINC[1]]) + r[PC_SINGLE]))
    return out


def gals_readings(by):
    """11 GALS readings from a type-0x40 frame, or None if the structure is not exactly as expected:
    every reading starts 0xA0 or 0xA1 (both seen; PR #1143 lists both), sequence numbers step by +2, and ch1 >= ch2 >= ch3 (nested integral channels)."""
    out, seq = [], []
    for i in range(11):
        x = by[13 + 19 * i: 32 + 19 * i]
        if x[0] not in (0xA0, 0xA1):
            return None
        seq.append(int.from_bytes(x[1:3], "big"))
        v = [int.from_bytes(x[3 + 2 * k: 5 + 2 * k], "big") for k in range(8)]
        if not (v[0] >= v[1] >= v[2]):
            return None
        out.append(v)
    if any((b - a) % 65536 != 2 for a, b in zip(seq, seq[1:])):
        return None
    return out


class ReplayTable:
    """Rolling day of clean particle rates: slot = (UTC second of day) // 10, value = mean true particles/s
    (raw p600 / 2, counts are doubled Poisson). Each slot keeps its most recent reading within 72 h, so
    gaps (receiver off, downlink drops) are filled from earlier days. Seeded once from the archive."""
    N = 8640

    def __init__(self):
        self.acc = {}                     # slot -> [day, sum_raw, n]
        now = time.time()
        for path in sorted(glob.glob(os.path.join(ARCH, "ggak-l5-*.jsonl")))[-4:]:
            try:
                fh = open(path, "rb")
            except OSError:
                continue
            for line in fh:
                try:
                    m = json.loads(line)
                except ValueError:
                    continue
                if m.get("p600") is not None and not m.get("rep") and now - m["rx"] < 72 * 3600:
                    self.add(m["rx"], m["p600"])

    def add(self, rx, p600):
        day, slot = int(rx // 86400), int(rx % 86400) // 10
        a = self.acc.get(slot)
        if a is None or a[0] < day:
            self.acc[slot] = a = [day, 0, 0]
        if a[0] == day:
            a[1] += p600; a[2] += 1

    def payload(self, now):
        cutoff = int(now // 86400) - 3
        b = bytearray([255]) * self.N     # 255 = no data for that slot
        for slot, (day, sm, n) in self.acc.items():
            if day > cutoff and n:
                b[slot] = min(254, round(sm / n / 2))
        filled = sum(1 for x in b if x != 255)
        return json.dumps({"t": int(now), "src": "live-archive", "slot_s": 10, "unit": "particles/s",
                           "filled": filled, "b64": __import__("base64").b64encode(bytes(b)).decode()},
                          separators=(",", ":"))


def mqtt_client():
    if not os.path.exists(CFG):
        log("no mqtt.json: dry run, archive only"); return None
    import paho.mqtt.client as mqtt
    c = json.load(open(CFG))
    cl = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=c.get("client_id", "ggak-l5-pi"))
    cl.username_pw_set(c["user"], c["pass"])
    cl.tls_set(cert_reqs=ssl.CERT_REQUIRED)
    cl.reconnect_delay_set(1, 60)
    cl.on_connect = lambda cl_, u, f, rc, p=None: log(f"mqtt connected rc={rc}")
    cl.on_disconnect = lambda cl_, u, f, rc, p=None: log(f"mqtt disconnected rc={rc}")
    cl.connect_async(c["host"], int(c.get("port", 8883)), keepalive=30)
    cl.loop_start()
    return cl


def main():
    cl = mqtt_client(); tail = Tail()
    bins = collections.defaultdict(lambda: {"ok": 0, "fail": 0, "snr": [], "clock": None})
    last_hb = 0.0; last_det = 0.0; last_clean = None
    counts = {}                       # UTC second -> (p600, p800, p1100, mip), raw
    pc_new = collections.defaultdict(lambda: [0, 0])   # publish second -> [candidate protons, new records]
    pc_seen = collections.deque(maxlen=200)            # recent SKL record numbers (frames are sent twice)
    hist = collections.deque(maxlen=10)
    replay = ReplayTable()
    log(f"replay table seeded: {sum(1 for a in replay.acc.values() if a[2])} slots")
    # Repaired frames: ~0.1% carry wildly wrong counts (2026-10-30 audit). Counts are doubled Poisson
    # (var/mean = 2.00), so reject a repaired frame if any p600 reading is > 6 sigma from the recent clean median.
    clean_ref = collections.deque(maxlen=660)          # last ~60 clean frames' readings
    def implausible(rd):
        if len(clean_ref) < 33:
            return False
        med = statistics.median(clean_ref); sig = (2 * med) ** 0.5
        return any(abs(v[0] - med) > 6 * sig for v in rd)
    S = int(time.time()) + 1
    while True:
        time.sleep(max(0, S - time.time()))
        for l in tail.lines():
            try:
                r = json.loads(l)
            except ValueError:
                continue
            if "hb" in r:
                if r.get("iq"):
                    last_hb = r["hb"]
                continue
            b = bins[int(r["rx"])]
            last_det = max(last_det, r["rx"])
            b["snr"].append(r["snr"])
            if r["ok"]:
                b["ok"] += 1
                by = bytes.fromhex(r["hex"])
                if by[4] == 0x40:
                    rd = gals_readings(by)
                    if rd is None:
                        log(f"0x40 frame at rx {r['rx']} failed structure check, counts dropped (rep={r.get('rep', 0)})")
                    elif r.get("rep") and implausible(rd):
                        log(f"0x40 frame at rx {r['rx']} repaired but counts implausible vs recent clean median, dropped (rep={r.get('rep')})")
                    else:
                        clk = int.from_bytes(by[9:13], "big") + EPOCH_MSK
                        if not r.get("rep"):
                            clean_ref.extend(v[0] for v in rd)
                        for i, v in enumerate(rd):
                            counts[clk - 10 + i] = (v[0], v[1], v[2], v[7], bool(r.get("rep")))
                elif by[4] == 0x3C and not r.get("rep"):
                    for seq, n in skl_records(by):
                        if seq in pc_seen:
                            continue
                        pc_seen.append(seq)
                        a = pc_new[int(r["rx"]) + DELAY]       # surfaces in the second this frame is published
                        a[0] += n; a[1] += 1
                if r.get("clock") is not None:
                    b["clock"] = r["clock"]
                last_clean = max(last_clean or 0, r["rx"])
            else:
                b["fail"] += 1
        rx = S - DELAY
        b = bins.pop(rx, None) or {"ok": 0, "fail": 0, "snr": [], "clock": None}
        for k in [k for k in bins if k < rx - 60]:
            bins.pop(k)
        c = counts.pop(rx, None)
        for k in [k for k in counts if k < rx - 60]:
            counts.pop(k)
        lock = (S - last_hb) < 12 and (rx - last_det) < 15
        snr = round(statistics.median(b["snr"]), 1) if b["snr"] else None
        msg = {"t": S, "rx": rx, "clock": b["clock"], "src": "live",
               "p600": c[0] if c else None, "p800": c[1] if c else None, "p1100": c[2] if c else None, "mip": c[3] if c else None,
               "frames": b["ok"], "csum_fail": b["fail"], "snr": snr, "lock": lock}
        if c and not c[4]:
            replay.add(rx, c[0])
        if c and c[4]:
            msg["rep"] = True            # counts came from a soft-repaired frame (see repair.py)
        pc = pc_new.pop(S, None)
        for k in [k for k in pc_new if k < S - 60]:
            pc_new.pop(k)
        msg["pc"] = pc[0] if pc else None    # candidate proton counts in SKL records that arrived this second
        msg["pc_rec"] = pc[1] if pc else 0
        s = json.dumps(msg, separators=(",", ":"))
        assert len(s) < 512
        with open(os.path.join(ARCH, f"ggak-l5-{utc_day(rx)}.jsonl"), "a") as fh:
            fh.write(s + "\n")
        if cl:
            cl.publish(TOPIC_SEC, s, qos=0)
        hist.append(msg)
        if S % 600 == 0 and cl:
            cl.publish(TOPIC_REPLAY, replay.payload(S), qos=1, retain=True)
        if S % 10 == 0:
            snrs = [m["snr"] for m in hist if m["snr"] is not None]
            st = {"t": S, "src": "live", "lock": lock, "snr": round(statistics.median(snrs), 1) if snrs else None,
                  "frames_10s": sum(m["frames"] for m in hist), "csum_fail_10s": sum(m["csum_fail"] for m in hist),
                  "last_frame_t": round(last_clean, 3) if last_clean else None, "sw": SW, "map": MAP, "units": "raw"}
            if cl:
                cl.publish(TOPIC_STATUS, json.dumps(st, separators=(",", ":")), qos=1, retain=True)
            log(f"status {st}")
        S += 1


if __name__ == "__main__":
    main()

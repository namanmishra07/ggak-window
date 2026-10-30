#!/usr/bin/env python3
"""Synthetic GGAK feed for developing the window without live signal.
Publishes to ggak/test/sec + ggak/test/status with src="synthetic"; refuses any ggak/l5 topic.
Counts are Poisson per 1 s bin from integral rates (p600 >= p800 >= p1100 by construction).
Usage: ggak_fake.py [--storm] [--rates 3.0,1.6,0.7] [--cfg mqtt.json] [--print]
  --storm  ramps every rate to 6x over 2 min, holds 3 min, decays over 5 min, repeats."""
import argparse, json, math, os, random, ssl, time
ap = argparse.ArgumentParser()
ap.add_argument("--rates", default="3.0,1.6,0.7", help="mean counts/s for >600, >800, >1100 MeV (synthetic)")
ap.add_argument("--storm", action="store_true")
ap.add_argument("--topic", default="ggak/test")
ap.add_argument("--cfg", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "mqtt.json"))
ap.add_argument("--print", action="store_true", help="print only, no MQTT")
a = ap.parse_args()
if a.topic.startswith("ggak/l5"):
    raise SystemExit("refusing: synthetic data must never be published on ggak/l5")
r600, r800, r1100 = map(float, a.rates.split(","))
assert r600 >= r800 >= r1100 >= 0


def poisson(lam):
    if lam <= 0:
        return 0
    L, k, p = math.exp(-lam), 0, 1.0
    while True:
        p *= random.random()
        if p <= L:
            return k
        k += 1


def storm_factor(t):
    ph = t % 600
    if ph < 120: return 1 + 5 * ph / 120
    if ph < 300: return 6
    return 1 + 5 * math.exp(-(ph - 300) / 100)


cl = None
if not a.print:
    import paho.mqtt.client as mqtt
    c = json.load(open(a.cfg))
    cl = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="ggak-fake-" + str(os.getpid()))
    cl.username_pw_set(c["user"], c["pass"]); cl.tls_set(cert_reqs=ssl.CERT_REQUIRED)
    cl.connect(c["host"], int(c.get("port", 8883)), keepalive=30); cl.loop_start()
S = int(time.time()) + 1; tot = 0
while True:
    time.sleep(max(0, S - time.time()))
    f = storm_factor(S) if a.storm else 1.0
    n1100 = poisson(r1100 * f); n800 = n1100 + poisson((r800 - r1100) * f); n600 = n800 + poisson((r600 - r800) * f)
    msg = {"t": S, "rx": S, "clock": S - 946684800 + 10800, "src": "synthetic", "p600": n600, "p800": n800, "p1100": n1100,
           "mip": None, "frames": 3, "csum_fail": 0, "snr": 9.0, "lock": True}
    s = json.dumps(msg, separators=(",", ":")); tot += 1
    if cl:
        cl.publish(a.topic + "/sec", s)
        if S % 10 == 0:
            cl.publish(a.topic + "/status", json.dumps({"t": S, "src": "synthetic", "lock": True, "snr": 9.0, "frames_10s": 30,
                       "csum_fail_10s": 0, "last_frame_t": S, "sw": "ggak_fake 0.1"}), qos=1, retain=True)
    if a.print or S % 10 == 0:
        print(s, flush=True)
    S += 1

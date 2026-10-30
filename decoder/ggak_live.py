#!/usr/bin/env python3
"""Live GGAK decoder: streams IQ from rtl_tcp, decodes 6 s windows every 5 s, writes one JSON
line per detected frame (clean or not) plus a heartbeat per window to data/frames-YYYYMMDD.jsonl (UTC)."""
import os, sys, json, time, socket, struct, threading, collections, datetime, subprocess, multiprocessing as mp
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ggak_core as g

HOST = os.environ.get("RTLTCP_HOST", "127.0.0.1"); PORT = int(os.environ.get("RTLTCP_PORT", "1234"))
FREQ = int(os.environ.get("GGAK_FREQ", "1693000000")); GAIN = int(os.environ.get("GGAK_GAIN_TENTHS", "250"))
WIN, HOP = 6.0, 5.0
DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
os.makedirs(DATA, exist_ok=True)
FS = int(g.FS0); BPS = 2 * FS                  # cu8 bytes per second

buf = collections.deque(); buf_bytes = 0; last_rx = [0.0]; lock = threading.Lock()


SPAWN = os.environ.get("SPAWN_RTLTCP", "1") == "1"
child = [None]


def rtl_tcp_up():
    """Own the rtl_tcp process: (re)start it if missing. A V4 re-enumeration leaves rtl_tcp holding a
    dead handle, so the watchdog in main() kills it when IQ stalls and this brings it back."""
    if not SPAWN:
        return
    if child[0] is None or child[0].poll() is not None:
        child[0] = subprocess.Popen(["/usr/bin/rtl_tcp", "-a", HOST, "-p", str(PORT), "-T"],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        log(f"started rtl_tcp pid {child[0].pid}")
        time.sleep(3)


def rtl_tcp_kill():
    if SPAWN and child[0] is not None and child[0].poll() is None:
        child[0].kill(); child[0].wait()
        log("killed stalled rtl_tcp")


def reader():
    global buf_bytes
    while True:
        try:
            rtl_tcp_up()
            s = socket.create_connection((HOST, PORT), timeout=10)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16 << 20)
            s.recv(12)
            cmd = lambda c, v: s.sendall(struct.pack(">BI", c, v))
            cmd(14, 1); cmd(1, FREQ); cmd(2, FS); cmd(3, 1); cmd(4, GAIN); cmd(8, 0)
            log(f"connected to rtl_tcp {HOST}:{PORT}, {FREQ} Hz, gain {GAIN/10} dB, bias-T on")
            pending = b""
            s.settimeout(5)
            while True:
                d = s.recv(1 << 18)
                if not d:
                    raise ConnectionError("rtl_tcp closed")
                d = pending + d
                cut = len(d) - (len(d) % 2); pending = d[cut:]
                with lock:
                    buf.append((time.time(), d[:cut])); buf_bytes += cut
                    while buf_bytes > (QUIET_MAX + 15) * BPS:
                        _, old = buf.popleft(); buf_bytes -= len(old)
                    last_rx[0] = time.time()
        except Exception as e:
            log(f"rtl_tcp link error: {e}; retry in 5 s")
            with lock:
                buf.clear(); buf_bytes = 0
            time.sleep(5)


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def out_path(t):
    return os.path.join(DATA, "frames-" + datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%Y%m%d") + ".jsonl")


def _decode(data):
    raw = np.frombuffer(data, np.uint8)
    z = ((raw[0::2].astype(np.float32) - 127.5) + 1j * (raw[1::2].astype(np.float32) - 127.5)).astype(np.complex64)
    frames, fc = g.decode(z)
    for f in frames:
        f["bytes"] = f["bytes"].tobytes()
    return frames, fc


QUIET_MIN, QUIET_MAX = 3.0, 10.0   # s of IQ per decode window, recorded while the CPU is idle
P40, F40 = 11.0, 0.36                # type-0x40 (GALS) frames: one every 11 s, 0.36 s long


def main():
    """Record quietly, then decode. The Pi's own interference rises with CPU load (measured 2026-10-27:
    41 % clean frames idle vs 4 % loaded), so each window only holds IQ received while no decode was
    running, and once a 0x40 frame has been seen, windows are cut just after the next predicted one."""
    global started
    started = time.time()
    threading.Thread(target=reader, daemon=True).start()
    pool = mp.get_context("spawn").Pool(1)   # decode off the reader's interpreter (see _decode)
    emitted = collections.deque(maxlen=64)
    t_quiet = time.time() + 1.0              # IQ received after this instant was recorded with the CPU quiet
    t40 = None                               # rx time of the last clean 0x40 frame
    while True:
        if t40 is not None and t_quiet - t40 < 600:
            nxt = t40 + P40 * np.ceil((t_quiet + 0.2 - t40) / P40)
            cut = nxt + F40 + 0.4
            if cut - t_quiet < QUIET_MIN:
                cut += P40
            cut = min(cut, t_quiet + QUIET_MAX)
        else:
            cut = t_quiet + 8.0
        time.sleep(max(0, cut - time.time()))
        deadline = time.time() + 3
        while time.time() < deadline:
            with lock:
                if buf and buf[-1][0] >= cut:
                    break
            time.sleep(0.05)
        with lock:
            chunks = [(t, c) for t, c in buf if t > t_quiet + 0.15]
            stalled = time.time() - last_rx[0] > 3
        if stalled or not chunks or sum(len(c) for _, c in chunks) < QUIET_MIN * BPS:
            write({"hb": time.time(), "iq": False})
            if time.time() - max(last_rx[0], started) > 20:
                rtl_tcp_kill(); last_rx[0] = time.time()
            t_quiet = time.time() + 0.2
            continue
        data = b"".join(c for _, c in chunks)
        t_end = chunks[-1][0]
        t0 = t_end - len(data) / BPS              # wall time of the window's first sample (+-50 ms)
        rate = len(data) / 2 / max(1e-3, t_end - chunks[0][0])
        flag = os.path.join(os.path.dirname(DATA), "dump.flag")
        if os.path.exists(flag):          # debug: touch dump.flag to save the next windows' raw IQ
            n = int(open(flag).read() or "3")
            with open(f"/home/pi/elektro/livewin_{int(t0)}.cu8", "wb") as fh:
                fh.write(data)
            if n <= 1: os.remove(flag)
            else: open(flag, "w").write(str(n - 1))
        tp = time.time()
        frames, fc = pool.apply(_decode, (data,))
        nnew = nok = 0
        for f in frames:
            rx = t0 + f["start_s"]
            if any(abs(rx - e) < 0.15 for e in emitted):
                continue
            emitted.append(rx); nnew += 1; nok += f["ok"]
            rec = {"rx": round(rx, 3), "ok": bool(f["ok"]), "rep": f["rep"], "asm": f["asm_err"], "eye": round(f["eye"], 3), "snr": round(f["snr_db"], 1)}
            if f["ok"]:
                by = np.frombuffer(f["bytes"], np.uint8)
                mk, clk = g.parse(by)
                rec.update(marker=mk, clock=clk, hex=f["bytes"].hex())
                if by[4] == 0x40:
                    t40 = rx
            write(rec)
        write({"hb": round(time.time(), 3), "iq": True, "fc": round(float(fc), 1), "new": nnew, "ok": nok,
               "cpu": round(time.time() - tp, 2), "sps": int(rate), "win": round(len(data) / BPS, 1), "t40": t40 is not None})
        t_quiet = time.time() + 0.2


def write(rec):
    t = rec.get("rx", rec.get("hb"))
    with open(out_path(t), "a") as fh:
        fh.write(json.dumps(rec, separators=(",", ":")) + "\n")


if __name__ == "__main__":
    mp.set_start_method("spawn", force=True)
    main()

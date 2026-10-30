# Hear a Russian weather satellite count particles

Elektro-L5 is a Russian weather satellite parked 36,000 km above the Indian Ocean at 76° East.
Besides its cameras it carries a radiation monitor (GGAK) that counts charged particles and
sends the counts down, all day, on 1693 MHz. This repo receives that signal with a wire antenna
and an RTL-SDR, decodes it live on a Raspberry Pi, publishes one message a second to a public
feed, and drives a wall of LEDs (and a web page that clicks like a Geiger counter) from it.

- **Live page, with sound:** https://gs.nl06.com/frontier/window
- **The write-up:** https://gs.nl06.com/frontier/particle-window

Three things this data shows (scripts and data are here, so you can check them):

1. **The values are real particle counts, reported doubled.** Variance / mean = 2.00 over 96
   ten-minute blocks, and not one odd value. About 40 particles a second.
2. **The channel labelled "protons above 600 MeV" is not tracking protons above 500 MeV.** Over three
   days it ranged 89-117 % of its mean while GOES-18's >500 MeV protons held 96-105 % (r = -0.12).
3. **It may be radiation-belt electrons.** It correlates with GOES-19's >2 MeV electrons (r ≈ +0.6 to
   +0.8) once the two are shifted by 6-10 h, roughly their local-time difference. 28 hours of data:
   suggestive, not proven.

## 1. Listen to the feed (no hardware, 1 minute)

The feed is public and read-only. Anyone can subscribe; only the station can publish.

```
mosquitto_sub -h z5166a1e.ala.asia-southeast1.emqxsl.com -p 8883 --capath /etc/ssl/certs \
  -u ggak-public -P M27enNeGa2DH8w9 -t ggak/l5/sec
```

(On macOS use `--cafile /etc/ssl/cert.pem` instead of `--capath`. WebSockets: port 8084, path `/mqtt`.)

One message a second:

```
{"t":1793098256,"rx":1793098236,"src":"live","p600":82,"p800":40,"p1100":12,
 "frames":3,"csum_fail":1,"snr":10.4,"lock":true}
```

`rx` is the second the counts describe (Unix time, ~20 s before `t`). Counts are raw (doubled), or
`null` when no checksum-verified reading covers that second. `"rep":true` marks counts from a
soft-repaired frame.

## 2. Decode a real recording (5 minutes)

`samples/` holds 9.7 s of raw IQ from the station (RTL-SDR, 1.024 Msps, tuned to 1693 MHz,
30 Oct 2026 12:29 UTC).

```
pip install numpy scipy
python3 decoder/decode_file.py samples/ggak_l5_20261030_1229utc_1024ksps.cu8
```

You should get 8 checksum-clean frames. The satellite's own clock, read out of the bytes, says
12:29:36 UTC, which is when it was recorded:

```
  0.27 s  type 0x3c  onboard clock 12:29:36 UTC
  ...
  9.23 s  type 0x40  onboard clock 12:29:45 UTC  p600 80 76 86 72 76 72 78 70 78 80 80
8 unique checksum-clean frames
```

## 3. Check the findings (1 minute)

```
python3 analysis/poisson_check.py data/ggak-l5-*.jsonl.gz
python3 analysis/goes_check.py   data/ggak-l5-*.jsonl.gz
```

`data/` has four days of the published feed (27-30 Oct 2026) and the NOAA SWPC 7-day GOES
snapshots they are compared against.

## 4. Build the receiver

| Part | Notes |
|---|---|
| RTL-SDR Blog V4 | any RTL-SDR that reaches 1.7 GHz |
| Nooelec SAWbird+ GOES | filtered LNA at the antenna, powered by the dongle's bias-T |
| Antenna | a home-built 1.7 m wire Yagi here; a 60 cm dish also works |
| Raspberry Pi 5 | keep it away from the antenna: a busy Pi 5 jams 1693 MHz (see below) |

Point at 76° E (visible from Africa, the Middle East, Asia, Australia, and low from eastern Europe).
Then:

```
pip install numpy scipy paho-mqtt
sudo cp decoder/ggak-live.service decoder/ggak-pub.service /etc/systemd/system/   # edit paths first
```

`ggak_live.py` runs `rtl_tcp`, records, decodes and writes frames; `ggak_pub.py` turns them into
the per-second feed (it needs your own broker in `mqtt.json`).

Things that cost days:

- **Zero clean frames is not always weak signal.** Ours were whole blocks of inverted bits: the
  tracking loop slipped half a subcarrier cycle. The decoder now integrates each bit against one
  subcarrier cycle with a fixed, sync-marker-anchored phase. No loop, no slips.
- **The Pi jams the antenna.** 41 % clean frames with the Pi idle, 4 % with it busy, same signal.
  So the decoder records quietly, then decodes, and times its quiet windows to the particle frames
  (one every 11 s).
- **The satellite goes quiet every 3 hours.** While the image downlinks run (15 min from :12 and
  :42 past every third hour, UTC) the beacon drops ~8 dB.
- **Repaired frames occasionally lie.** About 0.1 % of soft-repaired frames carry wildly wrong
  counts; `ggak_pub.py` rejects any that sit > 6 σ from the recent clean median.

## 5. Build the window

`firmware/ggak_window/` is an Arduino sketch for an ESP32 DevKit, two 60-LED WS2812B strips behind
two 25 cm box frames, an SSD1306 SPI screen and a 5 V 3 A supply. One count lights one LED, full
on then fading; the fade is quick in a storm and slow when quiet. If the live feed gives no count
for 5 minutes it plays the previous day's Elektro-L2 protons from SINP (labelled REPLAY), then
random test data (labelled TEST). Fill in your Wi-Fi at the top. Board "ESP32 Dev Module",
partition scheme "Minimal SPIFFS (1.9MB APP with OTA)".

## What is not known

- **What the channels count.** The byte layout comes from SatDump pull request #1143 (closed
  unmerged) and matches every frame here. Its labels, protons above 600 / 800 / 1100 MeV, are
  what finding 2 calls into question. No official L5 data is published to settle it.
- **Calibration.** Counts are raw. Turning them into flux needs the instrument's geometry factor.

## Credits

SatDump PR #1143 (the counter layout). SINP, Moscow State University (Elektro-L2 data). NOAA SWPC
(GOES data). Our earlier Elektro-L2 decode and the first L5 catch are written up at
https://gs.nl06.com/frontier.

MIT licence.

# Hear a Russian weather satellite count particles

Elektro-L5 is a Russian weather satellite parked 36,000 km above the Indian Ocean at 76° East.
Besides its cameras it carries a radiation monitor (GGAK) that counts charged particles and
sends the counts down, all day, on 1693 MHz. This repo receives that signal with a wire antenna
and an RTL-SDR, decodes it live on a Raspberry Pi, publishes one message a second to a public
feed, and drives a wall of LEDs (and a web page that clicks like a Geiger counter) from it.

- **The write-up, with the live feed and sound at the top:** https://gs.nl06.com/frontier/particle-window
- **Just the live window:** https://gs.nl06.com/frontier/window-live.html

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

The `ggak-public` login is public on purpose. It can only subscribe to `ggak/l5/sec`, `ggak/l5/status`
and `ggak/l5/replay`: publishing and wildcard subscriptions are refused by the broker.

(On macOS use `--cafile /etc/ssl/cert.pem` instead of `--capath`. WebSockets: port 8084, path `/mqtt`.)

One message a second:

```
{"t":1793098256,"rx":1793098236,"src":"live","p600":82,"p800":40,"p1100":12,
 "frames":3,"csum_fail":1,"snr":10.4,"lock":true,"pc":0,"pc_rec":3}
```

`rx` is the second the counts describe (Unix time, ~20 s before `t`). `p600`, `p800` and `p1100` are
raw counts (doubled, so `p600 / 2` is particles that second), or `null` when no checksum-verified
reading covers that second. `"rep":true` marks counts from a soft-repaired frame. The names are the
labels from SatDump PR #1143; finding 2 is why we no longer call them protons.

`pc` is a separate, much rarer stream: candidate proton hits from the SKL spectrometer records
(`pc_rec` = records seen that second). About one hit every 7 to 23 minutes, unvalidated until a solar
proton event shows up in it.

Two more topics:

- `ggak/l5/status` (retained, every 10 s): receiver state, SNR, software version.
- `ggak/l5/replay` (retained, every 10 min): a day of particles per second in 8,640 ten-second slots,
  built from the station's own recordings (each slot keeps its latest reading from the last 72 h).
  Base64 bytes, 255 = no reading. The window plays it when the live feed is quiet.

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
the per-second feed and the replay table (it needs your own broker in `mqtt.json`). The Pi only makes
outgoing connections: nothing reaches it from the internet.

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
two 25 cm box frames, an SSD1306 SPI screen and a 5 V 3 A supply. One particle lights one LED, in
one colour, full on then fading; the fade is quick in a storm and slow when quiet. If the live feed
gives no count for 5 minutes it plays the station's own recorded counts for the same time of day
from `ggak/l5/replay` (labelled REPLAY), and if it has none, random test data (labelled TEST).

It joins Wi-Fi first and serves a control page at `http://ggak-window.local` to anyone on the same
network: brightness, flash rate, fade times, display on/off (always back on after a power cut) and
the data source. Fill in your Wi-Fi and an upload password at the top. Board "ESP32 Dev Module",
partition scheme "Minimal SPIFFS (1.9MB APP with OTA)".

## What is not known

- **What the channels count.** The byte layout comes from SatDump pull request #1143 (closed
  unmerged) and matches every frame here. Its labels, protons above 600 / 800 / 1100 MeV, are
  what finding 2 calls into question. No official L5 data is published to settle it.
- **Calibration.** Counts are raw. Turning them into flux needs the instrument's geometry factor.

## Credits

SatDump PR #1143 by petrvosahlocom (May 2026): the GGAK frame and instrument layout, including the
channel labels questioned above. Aweeri: the Elektro-L2 recording behind our first decode.
SINP, Moscow State University: the published Elektro-L2 data we validated that decode against.
NOAA SWPC: GOES data. Our Elektro-L2 decode (June 2026) and first Elektro-L5 catch (July 2026) are
written up at https://gs.nl06.com/frontier.

MIT licence.

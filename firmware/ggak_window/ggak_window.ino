// ggak_window.ino : Elektro-L5 GGAK "window to space"
// ESP32 DevKit + 2 x WS2812B panes (60 LEDs each) + 0.96" SSD1306 OLED (SPI)
//
// Behaviour matches the browser reference: https://gs.nl06.com/frontier/window-live (CONFIG block)
// Feed spec: ESP32_FEED.md
//
// Rules this sketch follows (do not "improve" them away):
//   * p600/p800/p1100 == null  -> draw nothing for that second. Never treated as 0.
//   * one flash = one LED, one colour, full on then fade to off; max N per second (panel setting),
//     counts beyond that are not drawn; no background glow
//   * link state from message ARRIVAL time on this device (stale 10 s, lost 60 s), not from rx
//   * only src == "live" messages are drawn from the broker
//   * TEST mode is local synthetic data and is always labelled TEST on the OLED
//
// Libraries (Library Manager): FastLED, PubSubClient (Nick O'Leary), ArduinoJson (Benoit Blanchon, v7), U8g2 (oliver)
// Board: ESP32 Dev Module (esp32 by Espressif 3.x)
//
// Serial Monitor (115200), send:
//   t  toggle TEST mode (same as a short press of BOOT)
//   m  map mode: lights each strip LED in turn and prints its index (to fill in COLS below)
//   c  column check: lights each configured column in turn
//   x  exit map/column mode
//
// Wi-Fi updates (OTA): after one USB upload of this sketch, Tools > Port shows "ggak-window at <ip>".
// Pick it, Upload, enter OTA_PASS. Mac and ESP32 must be on the same Wi-Fi.
// Partition Scheme must be "Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)".
//
// Control page: http://ggak-window.local (or http://<ip>, shown on the OLED bottom line). No login:
// anyone on the same Wi-Fi can use it. Shows LIVE/STALE/LOST/TEST, rate, frame age, SNR; TEST on/off;
// display on/off (off lasts only until the next boot); sliders for brightness, flashes per second,
// quiet fade and storm fade (saved in flash, survive reboots); restart button. Firmware uploads still need OTA_PASS.
//
// Data source (Auto by default, can be forced from the page; the choice resets to Auto on every boot):
//   1. LIVE   : our own L5 decode via the broker, while it has delivered a non-null count in the last 5 min
//   2. REPLAY : else this station's own recorded L5 particle rates at the same UTC time of day (retained
//               table on ggak/l5/replay, refreshed by the Pi every 10 min). Labelled REPLAY.
// What the light is: the GGAK counter channel the community decoder calls "protons > 600 MeV". Our data
// says those are real particle counts (doubled Poisson, ~40/s) but probably NOT >600 MeV protons (they
// don't follow GOES >500 MeV protons). So everything here says "particles", never "protons".
//   3. TEST   : else local random data, labelled TEST.
//
// Boot order: Wi-Fi is joined first (up to 20 s, OLED says so), then the web page + OTA start, then LEDs/feed.
// If Wi-Fi drops it keeps retrying and re-announces ggak-window.local on reconnect.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <U8g2lib.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <mbedtls/base64.h>
#include <time.h>
#include <math.h>

// =============================== FILL IN ===============================
#define WIFI_SSID "your-wifi-name"
#define WIFI_PASS "your-wifi-password"
#define OTA_HOST  "ggak-window"
#define OTA_PASS  "choose-an-upload-password"   // Wi-Fi firmware upload password (the web page has no login)
// ======================================================================

// ---------- feed: public, subscribe-only account ----------
static const char* MQTT_HOST    = "z5166a1e.ala.asia-southeast1.emqxsl.com";
static const uint16_t MQTT_PORT = 8883;
static const char* MQTT_USER    = "ggak-public";
static const char* MQTT_PASS    = "M27enNeGa2DH8w9";
static const char* TOPIC_SEC    = "ggak/l5/sec";
static const char* TOPIC_STATUS = "ggak/l5/status";
static const char* TOPIC_REPLAY = "ggak/l5/replay";

// DigiCert Global Root G2 (valid to 2038-01-15)
// SHA-256 fingerprint CB:3C:CB:B7:60:31:E5:E0:13:8F:8D:D3:9A:23:F9:DE:47:FF:C3:5E:43:C1:14:4C:EA:27:D4:6A:5A:B1:CB:5F
static const char CA_CERT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH
MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI
2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx
1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ
q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz
tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ
vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP
BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV
5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY
1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4
NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG
Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91
8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe
pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----
)EOF";

// ---------- hardware ----------
#define PIN_TOP        16      // top pane strip data (via 330 ohm)
#define PIN_BOTTOM     17      // bottom pane strip data (via 330 ohm)
#define LEDS_PER_PANE  60
#define NUM_LEDS       (2 * LEDS_PER_PANE)
#define PIN_BOOT       0       // on-board BOOT button: short press toggles TEST
#define OLED_CS        5       // OLED (7-pin SPI module): D0=SCK->18, D1=MOSI->23, RES->21, DC->19, CS->5
#define OLED_DC        19
#define OLED_RST       21
#define MAX_MILLIAMPS  2500    // stays under the 3 A adapter

// LED layout. For each pane, 3 columns (left, middle, right) as positions along THAT pane's strip.
// dir = +1 : strip index increases going DOWN the column; -1 : increases going UP.
// LEDs not inside any column (folds) are never lit.
// Default guess: 20 LEDs per column, serpentine. Run map mode ('m') and edit these numbers.
struct Col { uint8_t start; uint8_t len; int8_t dir; };
Col COLS[2][3] = {
  { {0, 20, +1}, {20, 20, -1}, {40, 20, +1} },   // pane 0 = top    (GPIO 16)
  { {0, 20, +1}, {20, 20, -1}, {40, 20, +1} },   // pane 1 = bottom (GPIO 17)
};

// ---------- look: adjustable from the control page, saved in flash (Preferences "ggak") ----------
// LEDs are off, or flashing in one colour. No background glow (it held every column LED dimly lit on
// the live feed, ~80 counts/s), no multi-LED blooms, no colour shift.
struct Look { int bright; int maxFlashes; float fadeQuietS, fadeStormS; };
const Look LOOK_DEFAULT = {110, 3, 1.2f, 0.3f};
Look look = LOOK_DEFAULT;
Preferences prefs;
bool displayOn = true;           // RAM only: every boot (power cut, restart, update) starts with the display on

void clampLook(Look& l) {
  l.bright     = constrain(l.bright, 1, 255);
  l.maxFlashes = constrain(l.maxFlashes, 0, 20);
  l.fadeQuietS = constrain(l.fadeQuietS, 0.05f, 5.0f);   // time from full to off
  l.fadeStormS = constrain(l.fadeStormS, 0.05f, 5.0f);
}
void loadLook() {
  prefs.begin("ggak", true);
  look.bright     = prefs.getInt("bright", LOOK_DEFAULT.bright);
  look.maxFlashes = prefs.getInt("flashes", LOOK_DEFAULT.maxFlashes);
  look.fadeQuietS = prefs.getFloat("fadeq", LOOK_DEFAULT.fadeQuietS);
  look.fadeStormS = prefs.getFloat("fades", LOOK_DEFAULT.fadeStormS);
  prefs.end();
  clampLook(look);
}
void saveLook() {
  prefs.begin("ggak", false);
  prefs.putInt("bright", look.bright);       prefs.putInt("flashes", look.maxFlashes);
  prefs.putFloat("fadeq", look.fadeQuietS);  prefs.putFloat("fades", look.fadeStormS);
  prefs.end();
}

// ---------- behaviour ----------
const float  STALE_AFTER_S      = 10.0f;
const float  LOST_AFTER_S       = 60.0f;
const float  STORM_FULL_AT      = 5.0f;
const int    BASELINE_MIN_MINUTES = 30;
const CRGB   AMBER(255, 110, 0);

// ---------- state ----------
CRGB leds[NUM_LEDS];
U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI oled(U8G2_R0, OLED_CS, OLED_DC, OLED_RST);
WiFiClientSecure net;
PubSubClient mqtt(net);

struct Flash { float t0, tau; uint8_t led; bool used; };
const int MAX_ACTIVE = 128;      // 20 flashes/s x 5 s at the slowest fade
Flash flashes[MAX_ACTIVE];

uint32_t lastMsgMs = 0;  bool haveMsg = false;
bool testMode = false;            // true while the active source is TEST (set by selectSource)
enum Source { SRC_LIVE, SRC_REPLAY, SRC_TEST };
enum Choice { CH_AUTO, CH_LIVE, CH_REPLAY, CH_TEST };
Source active = SRC_LIVE;  Choice choice = CH_AUTO;  // choice is RAM only: every boot starts in Auto
const char* SRC_NAME[] = {"live", "replay", "test"};
const char* CH_NAME[]  = {"auto", "live", "replay", "test"};
const float FALLBACK_AFTER_S = 300;  // Auto leaves LIVE after this long without a non-null live count
uint32_t lastLiveCountMs = 0;  bool haveLiveCount = false;
enum Mode { RUN, MAPPING, COLCHECK } mode = RUN;

// readout
long   lastFrameUnix = 0;          // unix s of the last decoded frame
float  lastSnr = NAN;  bool lastLock = false;  bool lastRep = false;
const char* statusMap = nullptr;  char statusMapBuf[32] = "";

// recent counts (last 60 non-null seconds) and 24 h baseline in minute buckets
int recentN[60]; int recentLen = 0, recentHead = 0;
struct Bucket { int32_t minute; uint32_t sum; uint16_t n; };
Bucket baseline[1440];

static inline float nowS() { return millis() / 1000.0f; }

// ---------- helpers ----------
uint32_t poisson(float lambda) {                 // Knuth; fine for small lambda
  float L = expf(-lambda), p = 1.0f; uint32_t k = 0;
  do { k++; p *= (esp_random() / 4294967296.0f); } while (p > L);
  return k - 1;
}
float frand() { return esp_random() / 4294967296.0f; }

int ledIndex(int pane, int col, int k) {         // k = 0 at the top of the column
  const Col& c = COLS[pane][col];
  int i = c.dir > 0 ? c.start + k : c.start + c.len - 1 - k;
  return pane * LEDS_PER_PANE + i;
}

void baselineAdd(long tSec, int n) {
  int32_t m = tSec / 60; Bucket& b = baseline[m % 1440];
  if (b.minute != m) { b.minute = m; b.sum = 0; b.n = 0; }
  b.sum += n; b.n++;
}
float baselineRate(long nowUnix) {               // mean counts/s over the last 24 h, or -1 if < 30 min of history
  int32_t cutoff = nowUnix / 60 - 1440; uint64_t s = 0; uint32_t n = 0; int mins = 0;
  for (int i = 0; i < 1440; i++) if (baseline[i].n && baseline[i].minute > cutoff) { s += baseline[i].sum; n += baseline[i].n; mins++; }
  return (mins >= BASELINE_MIN_MINUTES && n) ? (float)s / n : -1;
}
float recentRate() {
  if (!recentLen) return -1; long s = 0; for (int i = 0; i < recentLen; i++) s += recentN[i]; return (float)s / recentLen;
}
float replayStorm = 0;
float stormLevel() {
  if (testMode) return 0;
  if (active == SRC_REPLAY) return replayStorm;
  time_t nowUnix = time(nullptr); if (nowUnix < 1700000000) return 0;
  float base = baselineRate(nowUnix), r = recentRate();
  if (base <= 0 || r <= 0) return 0;
  float v = logf(r / base) / logf(STORM_FULL_AT);
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

// n counts spread over the next windowS seconds, at most look.maxFlashes per second; the rest are not drawn
void spawnFlashes(int n, float windowS) {
  // fade: slow when quiet, quick in a storm (storm level 0..1 vs the source's own baseline)
  float fadeS = look.fadeQuietS + (look.fadeStormS - look.fadeQuietS) * stormLevel();
  float tau = fadeS / 4.60517f;                            // ln(1/0.01): fadeS = time to fully off
  n = min(n, (int)(look.maxFlashes * windowS)); float base = nowS();
  for (int k = 0; k < n; k++) {
    for (int s = 0; s < MAX_ACTIVE; s++) if (!flashes[s].used) {
      int p = esp_random() % 2, c = esp_random() % 3;
      flashes[s] = { base + windowS * frand(), tau, (uint8_t)ledIndex(p, c, esp_random() % COLS[p][c].len), true };
      break;
    }
  }
}

// ---------- intake: every source funnels through here (mirrors onSec in the browser) ----------
// counts < 0 means null (no verified reading covers that second)
void onSecond(int p600, int p800, int p1100, int frames, long rx, bool live) {
  lastMsgMs = millis(); haveMsg = true;
  if (frames > 0 && rx > 0) lastFrameUnix = rx;
  if (p600 < 0 || p800 < 0 || p1100 < 0) return;          // null: nothing to show, nothing invented
  int t2 = p1100, t1 = p800 - p1100, t0 = p600 - p800;
  if (t0 < 0 || t1 < 0 || t2 < 0) return;                  // not nested: malformed, drop
  int particles = p600 / 2;                                // raw values are doubled counts (var/mean = 2.00)
  recentN[recentHead] = particles; recentHead = (recentHead + 1) % 60; if (recentLen < 60) recentLen++;
  if (live && rx > 0) baselineAdd(rx, particles);
  spawnFlashes(particles, 1.0f);
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  if (!strcmp(topic, TOPIC_REPLAY)) { onReplayTable(payload, len); return; }   // big: parse once, on its own
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;
  if (!strcmp(topic, TOPIC_STATUS)) {
    if (doc["last_frame_t"].is<long>()) lastFrameUnix = doc["last_frame_t"].as<long>();
    const char* m = doc["map"] | "";
    strlcpy(statusMapBuf, m, sizeof statusMapBuf); statusMap = statusMapBuf;
    return;
  }
  if (strcmp(topic, TOPIC_SEC)) return;
  const char* src = doc["src"] | "";
  if (strcmp(src, "live")) return;                         // only live data from the broker
  if (!doc["snr"].isNull()) lastSnr = doc["snr"].as<float>();
  lastLock = doc["lock"] | false;
  lastRep  = doc["rep"] | false;
  auto cnt = [&](const char* k) -> int { return doc[k].is<int>() ? doc[k].as<int>() : -1; };
  if (cnt("p600") >= 0 && cnt("p800") >= 0 && cnt("p1100") >= 0) { lastLiveCountMs = millis(); haveLiveCount = true; }
  if (active != SRC_LIVE) { lastMsgMs = millis(); haveMsg = true; return; }   // keep link state honest, draw the active source only
  long rx = doc["rx"].is<long>() ? doc["rx"].as<long>() : (doc["t"] | 0L);
  onSecond(cnt("p600"), cnt("p800"), cnt("p1100"), doc["frames"] | 0, rx, true);
}

// ---------- synthetic TEST data (same shape as the browser's synthetic source) ----------
uint32_t testStartMs = 0, testNextMs = 0;
void testTick() {
  if (!testMode || millis() < testNextMs) return;
  testNextMs = millis() + 1000;
  float ph = fmodf((millis() - testStartMs) / 1000.0f, 600.0f);
  float f = ph < 120 ? 1 + 5 * ph / 120 : ph < 300 ? 6 : 1 + 5 * expf(-(ph - 300) / 100);  // storm swell every 10 min
  int a = poisson(0.7f * f), b = a + poisson(0.9f * f), c = b + poisson(1.4f * f);
  onSecond(c, b, a, 1, 0, false);
}

// ---------- REPLAY: this station's own recorded L5 particle rates, same UTC time of day ----------
// The Pi publishes a retained table on ggak/l5/replay every 10 min: 8640 x 10-s slots, value = mean true
// particles/s (raw / 2), 255 = no data; each slot holds its most recent reading within 72 h. Empty slots
// are filled here from the nearest recorded slot. Arrives over the same MQTT link as the live feed.
const int  ROWS_PER_DAY = 8640;
uint8_t    replayBuf[2][ROWS_PER_DAY];
volatile int replayCur = -1;               // which buffer is live, -1 = none yet
char       replayDay[24] = "";             // when the Pi built the table (UTC)
volatile int replayRows = 0;               // slots that held real data before gap filling
float      replayMean = 0;                 // mean particles/s over the table, for the storm level
volatile uint32_t replayFetchedMs = 0;
char       replayErr[48] = "waiting for table";

void onReplayTable(byte* payload, unsigned int len) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, payload, len);
  if (e) { snprintf(replayErr, sizeof replayErr, "table: %s (%u B, heap %u)", e.c_str(), len, (unsigned)ESP.getMaxAllocHeap()); return; }
  const char* b64 = doc["b64"] | "";
  int nb = replayCur == 0 ? 1 : 0; uint8_t* b = replayBuf[nb];
  size_t olen = 0;
  if (mbedtls_base64_decode(b, ROWS_PER_DAY, &olen, (const unsigned char*)b64, strlen(b64)) || olen != ROWS_PER_DAY) {
    strlcpy(replayErr, "table: bad size", sizeof replayErr); return;
  }
  int filled = 0; uint32_t sum = 0;
  for (int i = 0; i < ROWS_PER_DAY; i++) if (b[i] != 255) { filled++; sum += b[i]; }
  if (!filled) { strlcpy(replayErr, "table: empty", sizeof replayErr); return; }
  // fill each empty slot from the nearest recorded one (circular, two passes)
  static int16_t dist[ROWS_PER_DAY]; static uint8_t val[ROWS_PER_DAY];
  for (int i = 0; i < ROWS_PER_DAY; i++) { dist[i] = b[i] != 255 ? 0 : 32767; val[i] = b[i]; }
  for (int pass = 0; pass < 2; pass++)
    for (int k = 0; k < 2 * ROWS_PER_DAY; k++) {
      int i = pass == 0 ? k % ROWS_PER_DAY : (2 * ROWS_PER_DAY - 1 - k) % ROWS_PER_DAY;
      int j = pass == 0 ? (i + ROWS_PER_DAY - 1) % ROWS_PER_DAY : (i + 1) % ROWS_PER_DAY;
      if (dist[j] < 32767 && dist[j] + 1 < dist[i]) { dist[i] = dist[j] + 1; val[i] = val[j]; }
    }
  memcpy(b, val, ROWS_PER_DAY);
  time_t built = doc["t"] | 0L; struct tm tm; gmtime_r(&built, &tm);
  replayCur = nb; replayRows = filled; replayMean = (float)sum / filled;
  strftime(replayDay, sizeof replayDay, "%d %b %H:%M UTC", &tm);
  replayFetchedMs = millis(); replayErr[0] = 0;
  Serial.printf("replay table: %d of %d slots recorded, mean %.1f particles/s\n", filled, ROWS_PER_DAY, replayMean);
}

int lastReplayIdx = -1;
void replayTick() {
  if (active != SRC_REPLAY || replayCur < 0) { lastReplayIdx = -1; return; }
  time_t now = time(nullptr); if (now < 1700000000) return;
  int idx = (now % 86400) / 10;
  if (idx == lastReplayIdx) return;
  lastReplayIdx = idx;
  const uint8_t* b = replayBuf[replayCur];
  int n = b[idx];
  lastMsgMs = millis(); haveMsg = true;
  if (n == 255) return;                                         // missing row: nothing shown, nothing invented
  // storm level: last minute (6 slots) vs the table's mean
  int s = 0, k = 0; for (int i = 0; i < 6; i++) { int j = (idx - i + ROWS_PER_DAY) % ROWS_PER_DAY; if (b[j] != 255) { s += b[j]; k++; } }
  float r = k ? (float)s / k : 0;
  replayStorm = (replayMean > 0 && r > 0) ? constrain(logf(r / replayMean) / logf(STORM_FULL_AT), 0.0f, 1.0f) : 0;
  recentN[recentHead] = n; recentHead = (recentHead + 1) % 60; if (recentLen < 60) recentLen++;
  spawnFlashes(n * 10, 10.0f);                                  // n particles/s over this 10-s slot
}

// ---------- source selection ----------
void setTest(bool on);
bool liveFresh() { return haveLiveCount && (millis() - lastLiveCountMs) / 1000.0f < FALLBACK_AFTER_S; }
void selectSource() {
  Source want;
  if (choice == CH_LIVE) want = SRC_LIVE;
  else if (choice == CH_REPLAY) want = SRC_REPLAY;
  else if (choice == CH_TEST) want = SRC_TEST;
  else if (liveFresh() || (!haveLiveCount && millis() < 60000)) want = SRC_LIVE;   // give the feed a minute after boot
  else if (replayCur >= 0 && time(nullptr) > 1700000000) want = SRC_REPLAY;
  else want = SRC_TEST;
  if (want == active) return;
  Serial.printf("source: %s -> %s (%s)\n", SRC_NAME[active], SRC_NAME[want], CH_NAME[choice]);
  active = want;
  setTest(want == SRC_TEST);
}
void setChoice(int c) { choice = (Choice)c; Serial.printf("source choice: %s\n", CH_NAME[c]); selectSource(); }

// ---------- rendering ----------
const char* linkState() {
  if (testMode) return "TEST";
  if (active == SRC_REPLAY) return replayCur >= 0 ? "REPLAY" : "LOST";
  if (!haveMsg) return "LOST";
  float age = (millis() - lastMsgMs) / 1000.0f;
  return age > LOST_AFTER_S ? "LOST" : age > STALE_AFTER_S ? "STALE" : "LIVE";
}

// One flash = one LED, one colour (AMBER), straight to full, then fades back to off.
// No background glow, no neighbours, no colour shift.
void render() {
  float t = nowS();
  const char* st = linkState();
  bool live = !strcmp(st, "LIVE") || !strcmp(st, "TEST") || !strcmp(st, "REPLAY");
  if (!live) for (int s = 0; s < MAX_ACTIVE; s++) if (flashes[s].used && flashes[s].t0 > t) flashes[s].used = false;  // no new flashes once stale

  fill_solid(leds, NUM_LEDS, CRGB::Black);
  if (!displayOn) { FastLED.show(); return; }
  for (int s = 0; s < MAX_ACTIVE; s++) {
    Flash& f = flashes[s]; if (!f.used) continue;
    float a = t - f.t0;
    if (a < 0) continue;
    float env = expf(-a / f.tau);
    if (env < 0.01f) { f.used = false; continue; }
    CRGB col = AMBER; col.nscale8_video((uint8_t)(env * 255.0f));
    if (col.r > leds[f.led].r) leds[f.led] = col;        // same LED twice: keep the brighter, never add
  }
  FastLED.show();
}

// ---------- OLED ----------
uint32_t lastOledMs = 0;
void drawOled() {
  if (millis() - lastOledMs < 250) return; lastOledMs = millis();
  static bool oledOn = true;
  if (displayOn != oledOn) { oledOn = displayOn; oled.setPowerSave(oledOn ? 0 : 1); }
  if (!displayOn) return;
  const char* st = linkState();
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  if (testMode) {                                   // unmistakable TEST banner
    oled.drawBox(0, 0, 128, 14); oled.setDrawColor(0);
    oled.drawStr(4, 11, "TEST  synthetic data"); oled.setDrawColor(1);
  } else if (active == SRC_REPLAY) {                // equally unmistakable REPLAY banner
    oled.drawBox(0, 0, 128, 14); oled.setDrawColor(0);
    oled.drawStr(4, 11, "REPLAY  recorded L5"); oled.setDrawColor(1);
  } else {
    oled.drawStr(0, 10, "ELEKTRO-L5 76E");
    oled.drawStr(128 - oled.getStrWidth(st) , 10, st);
  }
  char line[32];
  float r = recentRate();
  if (r >= 0) snprintf(line, sizeof line, "particles %5.1f/s", r); else snprintf(line, sizeof line, "particles   --");
  oled.drawStr(0, 24, line);
  time_t nowUnix = time(nullptr);
  if (testMode) snprintf(line, sizeof line, "not live: local test");
  else if (active == SRC_REPLAY) snprintf(line, sizeof line, "same time, earlier day");
  else if (!strcmp(st, "LOST")) snprintf(line, sizeof line, "link lost");
  else if (lastFrameUnix > 0 && nowUnix > 1700000000) snprintf(line, sizeof line, "frame %lds ago", (long)(nowUnix - lastFrameUnix));
  else snprintf(line, sizeof line, "no frame yet");
  oled.drawStr(0, 36, line);
  if (!isnan(lastSnr)) snprintf(line, sizeof line, "snr %.1f %s%s", lastSnr, lastLock ? "lock" : "nolock", lastRep ? " rep" : "");
  else snprintf(line, sizeof line, "snr --");
  oled.drawStr(0, 48, line);
  if (WiFi.status() == WL_CONNECTED) snprintf(line, sizeof line, "%s", WiFi.localIP().toString().c_str());
  else snprintf(line, sizeof line, "wifi: connecting");
  oled.drawStr(0, 62, line);                       // IP so anyone can reach the control page
  oled.sendBuffer();
}

// ---------- network ----------

void startNetServices();
void wifiBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(OTA_HOST);                      // shows as ggak-window in the router's client list
  WiFi.setSleep(false);                            // modem sleep makes the page and .local lookups flaky
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}
// first thing at boot: join Wi-Fi before anything else, up to timeoutMs
void connectWifiFirst(uint32_t timeoutMs) {
  oled.clearBuffer(); oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 12, "GGAK window"); oled.drawStr(0, 30, "joining wifi:"); oled.drawStr(0, 44, WIFI_SSID);
  oled.sendBuffer();
  wifiBegin();
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(100);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("wifi up, ip %s\n", WiFi.localIP().toString().c_str());
    oled.drawStr(0, 58, WiFi.localIP().toString().c_str());
  } else {
    Serial.println("wifi not up yet, will keep retrying");
    oled.drawStr(0, 58, "not yet, retrying");
  }
  oled.sendBuffer();
}
void ensureWifi() {
  static uint32_t last = 0; static bool wasUp = false;
  wl_status_t s = WiFi.status();
  if (s == WL_CONNECTED) {
    if (!wasUp) { wasUp = true; startNetServices(); }   // first connect, or back after a drop
    return;
  }
  if (wasUp) { wasUp = false; last = millis(); Serial.println("wifi lost"); }
  if (millis() - last < 20000) return;
  last = millis();
  Serial.printf("wifi status %d, retrying\n", s);
  WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS);
}
void ensureMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  static uint32_t last = 0, backoff = 2000;
  if (millis() - last < backoff && last) return; last = millis();
  String id = "ggak-esp32-" + WiFi.macAddress(); id.replace(":", "");
  if (mqtt.connect(id.c_str(), MQTT_USER, MQTT_PASS)) {
    mqtt.subscribe(TOPIC_SEC, 0); mqtt.subscribe(TOPIC_STATUS, 0); mqtt.subscribe(TOPIC_REPLAY, 1);
    Serial.println("mqtt connected"); backoff = 2000;
  } else {
    Serial.printf("mqtt connect failed, state %d\n", mqtt.state());
    backoff = min<uint32_t>(backoff * 2, 60000);
  }
}

// ---------- map / column check modes ----------
uint32_t mapNextMs = 0; int mapIdx = 0;
void mapTick() {
  if (millis() < mapNextMs) return;
  if (mode == MAPPING) {
    mapNextMs = millis() + 700;
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    leds[mapIdx] = (mapIdx % 10 == 0) ? CRGB(0, 80, 0) : CRGB(60, 60, 60);
    FastLED.show();
    Serial.printf("pane %d (%s)  strip index %d\n", mapIdx / LEDS_PER_PANE, mapIdx < LEDS_PER_PANE ? "top" : "bottom", mapIdx % LEDS_PER_PANE);
    mapIdx = (mapIdx + 1) % NUM_LEDS;
  } else if (mode == COLCHECK) {
    mapNextMs = millis() + 1500;
    int p = mapIdx / 3, c = mapIdx % 3;
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    for (int k = 0; k < COLS[p][c].len; k++) leds[ledIndex(p, c, k)] = k == 0 ? CRGB(0, 80, 0) : CRGB(80, 35, 0);
    FastLED.show();
    Serial.printf("pane %d column %d (green = top end)\n", p, c);
    mapIdx = (mapIdx + 1) % 6;
  }
}
void setTest(bool on) {
  testMode = on; testStartMs = millis(); testNextMs = 0;
  for (int s = 0; s < MAX_ACTIVE; s++) flashes[s].used = false;
  recentLen = 0;
  Serial.println(on ? "TEST mode ON (synthetic)" : "TEST mode OFF");
}
void handleSerial() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == 't') setChoice(choice == CH_TEST ? CH_AUTO : CH_TEST);
    else if (ch == 'm') { mode = MAPPING;  mapIdx = 0; mapNextMs = 0; Serial.println("map mode: 'x' to exit"); }
    else if (ch == 'c') { mode = COLCHECK; mapIdx = 0; mapNextMs = 0; Serial.println("column check: 'x' to exit"); }
    else if (ch == 'x') { mode = RUN; Serial.println("back to run"); }
  }
}
void handleBoot() {
  static bool was = false; static uint32_t down = 0;
  bool now = digitalRead(PIN_BOOT) == LOW;
  if (now && !was) down = millis();
  if (!now && was && millis() - down > 50 && millis() - down < 1500) setChoice(choice == CH_TEST ? CH_AUTO : CH_TEST);
  was = now;
}


// ---------- control web page (no login: anyone on the same Wi-Fi) ----------
WebServer web(80);
static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>GGAK window</title>
<style>body{background:#0b0b0d;color:#e8e2d8;font:16px -apple-system,system-ui,sans-serif;margin:0;padding:20px;max-width:420px}
h1{font-size:18px;font-weight:600;margin:0 0 16px}h2{font-size:15px;font-weight:600;margin:22px 0 4px}#st{font-size:34px;font-weight:700;margin:4px 0 14px}
.LIVE{color:#ff8a1e}.STALE{color:#c9a227}.LOST{color:#888}.TEST{color:#6ab0ff}
table{border-collapse:collapse;width:100%;margin-bottom:18px}td{padding:6px 0;border-bottom:1px solid #222}td:last-child{text-align:right;font-variant-numeric:tabular-nums}
button{font:inherit;padding:12px 18px;border-radius:10px;border:1px solid #444;background:#1a1a1e;color:#e8e2d8;margin:0 8px 8px 0}
button.on{background:#6ab0ff;color:#000;border-color:#6ab0ff}small{color:#777}
label{display:flex;justify-content:space-between;margin-top:14px}label b{font-weight:400;font-variant-numeric:tabular-nums;color:#ff8a1e}
input[type=range]{width:100%;accent-color:#ff8a1e}</style></head><body>
<h1>Elektro-L5 GGAK window</h1><div id="st">…</div>
<table><tr><td>particle rate</td><td id="r">–</td></tr><tr><td>last frame</td><td id="f">–</td></tr>
<tr><td>SNR</td><td id="s">–</td></tr><tr><td>IP</td><td id="ip">–</td></tr><tr><td>uptime</td><td id="u">–</td></tr></table>
<h2>Source</h2><div id="src" style="margin-bottom:10px">–</div>
<button id="c_auto" onclick="c('auto')">Auto</button><button id="c_live" onclick="c('live')">Live</button><button id="c_replay" onclick="c('replay')">Replay</button><button id="c_test" onclick="c('test')">Test</button>
<table><tr><td>last live count</td><td id="lc">–</td></tr><tr><td>replay table built</td><td id="rd">–</td></tr><tr><td>replay table</td><td id="re">–</td></tr></table>
<p><small>The light: real particle counts from Elektro-L5's radiation monitor, about 40 a second, shown at most a few flashes a second. Which particles is not confirmed (the "protons above 600 MeV" label looks wrong; probably radiation-belt electrons). Auto: live feed while it gives counts; after 5 min without, this station's own recorded counts from the same time of day (labelled REPLAY); if that is missing too, random TEST data. A forced choice lasts until the next restart.</small></p>
<h2>Display</h2><button id="don" onclick="d(1)">On</button><button id="doff" onclick="d(0)">Off</button>
<p><small>Off turns the LEDs and the small screen dark. After a power cut or restart it always comes back on.</small></p>
<h2>Look</h2><div id="sl"></div>
<p><button onclick="get('/set?reset=1')">Reset look to defaults</button></p>
<p><small>Changes apply immediately and are kept after a restart.</small></p>
<h2>Device</h2><button onclick="if(confirm('Restart the window?'))fetch('/restart')">Restart</button>
<script>
const S=[['bright','Brightness',1,255,1,v=>Math.round(v/2.55)+' %'],
['flashes','Max flashes per second',0,20,1,v=>v],
['fadeq','Fade time, quiet',0.05,5,0.05,v=>(+v).toFixed(2)+' s'],
['fades','Fade time, storm',0.05,5,0.05,v=>(+v).toFixed(2)+' s']];
let busy=0;
sl.innerHTML=S.map(([k,n,a,b,st])=>`<label>${n}<b id="v_${k}"></b></label><input type="range" id="i_${k}" min="${a}" max="${b}" step="${st}">`).join('');
S.forEach(([k,,,,,fmt])=>{const i=document.getElementById('i_'+k),v=document.getElementById('v_'+k);
i.oninput=()=>{busy=Date.now();v.textContent=fmt(i.value)};
i.onchange=()=>{busy=Date.now();get('/set?'+k+'='+i.value)}});
function show(j){st.textContent=j.state;st.className=j.state;
r.textContent=j.rate==null?'–':j.rate.toFixed(1)+' particles/s';
f.textContent=j.frame_age==null?(j.state=='LOST'?'link lost':'none yet'):j.frame_age+' s ago';
s.textContent=j.snr==null?'–':j.snr.toFixed(1)+' dB '+(j.lock?'lock':'no lock')+(j.rep?' · replay':'');
ip.textContent=j.ip;u.textContent=Math.floor(j.uptime/60)+' min';
['auto','live','replay','test'].forEach(k=>document.getElementById('c_'+k).className=j.choice==k?'on':'');
src.innerHTML='Showing <b class="'+j.state+'">'+({live:'LIVE feed',replay:'REPLAY (recorded)',test:'TEST (random)'})[j.source]+'</b>'+(j.choice=='auto'?' (auto)':' (forced)');
lc.textContent=j.live_count_age==null?'none since boot':(j.live_count_age<120?j.live_count_age+' s ago':Math.round(j.live_count_age/60)+' min ago');
rd.textContent=j.replay.day?j.replay.day+' ('+Math.round(j.replay.rows/86.4)+' % of the day recorded)':'none';
re.textContent=j.replay.err?j.replay.err:(j.replay.fetched_min==null?'–':'received '+j.replay.fetched_min+' min ago');
don.className=j.display?'on':'';doff.className=j.display?'':'on';
if(Date.now()-busy>1500)S.forEach(([k,,,,,fmt])=>{document.getElementById('i_'+k).value=j.look[k];document.getElementById('v_'+k).textContent=fmt(j.look[k])});}
function get(u){fetch(u).then(x=>x.json()).then(show).catch(()=>{st.textContent='no reply';st.className='LOST'})}
function t(on){get('/test?on='+on)}
function c(k){get('/source?c='+k)}
function d(on){get('/display?on='+on)}
get('/status');setInterval(()=>get('/status'),1000);
</script></body></html>)HTML";

void sendStatus() {
  JsonDocument j;
  j["state"] = linkState(); j["test"] = testMode;
  j["source"] = SRC_NAME[active]; j["choice"] = CH_NAME[choice];
  if (haveLiveCount) j["live_count_age"] = (millis() - lastLiveCountMs) / 1000; else j["live_count_age"] = nullptr;
  JsonObject rp = j["replay"].to<JsonObject>();
  rp["day"] = replayDay; rp["rows"] = (int)replayRows; rp["err"] = replayErr;
  if (replayFetchedMs) rp["fetched_min"] = (millis() - replayFetchedMs) / 60000; else rp["fetched_min"] = nullptr;
  float r = recentRate(); if (r >= 0) j["rate"] = r; else j["rate"] = nullptr;
  time_t nowUnix = time(nullptr);
  if (!testMode && lastFrameUnix > 0 && nowUnix > 1700000000) j["frame_age"] = (long)(nowUnix - lastFrameUnix); else j["frame_age"] = nullptr;
  if (!isnan(lastSnr)) j["snr"] = lastSnr; else j["snr"] = nullptr;
  j["lock"] = lastLock; j["rep"] = lastRep;
  j["ip"] = WiFi.localIP().toString(); j["uptime"] = millis() / 1000;
  JsonObject l = j["look"].to<JsonObject>();
  l["bright"] = look.bright; l["flashes"] = look.maxFlashes;
  l["fadeq"] = look.fadeQuietS; l["fades"] = look.fadeStormS;
  j["display"] = displayOn;
  String out; serializeJson(j, out);
  web.send(200, "application/json", out);
}
void handleSet() {
  if (web.hasArg("reset")) look = LOOK_DEFAULT;
  if (web.hasArg("bright"))  look.bright     = web.arg("bright").toInt();
  if (web.hasArg("flashes")) look.maxFlashes = web.arg("flashes").toInt();
  if (web.hasArg("fadeq"))   look.fadeQuietS = web.arg("fadeq").toFloat();
  if (web.hasArg("fades"))   look.fadeStormS = web.arg("fades").toFloat();
  clampLook(look);
  FastLED.setBrightness(look.bright);
  saveLook();
  Serial.printf("look: bright %d flashes %d fade quiet %.2f storm %.2f\n",
                look.bright, look.maxFlashes, look.fadeQuietS, look.fadeStormS);
  sendStatus();
}
bool webStarted = false;
void startWeb() {
  web.on("/", []() { web.send_P(200, "text/html", PAGE); });
  web.on("/status", sendStatus);
  web.on("/test", []() {
    if (web.hasArg("on")) setChoice(web.arg("on") == "1" ? CH_TEST : CH_AUTO);
    sendStatus();
  });
  web.on("/set", handleSet);
  web.on("/source", []() {
    String k = web.arg("c");
    for (int i = 0; i < 4; i++) if (k == CH_NAME[i]) setChoice((Choice)i);
    sendStatus();
  });
  web.on("/display", []() {
    if (web.hasArg("on")) { displayOn = web.arg("on") == "1"; Serial.println(displayOn ? "display ON" : "display OFF"); }
    sendStatus();
  });
  web.on("/restart", []() { web.send(200, "text/plain", "restarting"); delay(300); ESP.restart(); });
  web.begin();
  webStarted = true;
}

// ---------- OTA (Wi-Fi uploads) ----------
bool otaBusy = false;
// called on every (re)connect: OTA + web start once; mDNS is re-announced each time
void startNetServices() {
  static bool once = false;
  if (!once) {
    ArduinoOTA.setHostname(OTA_HOST);
    ArduinoOTA.setPassword(OTA_PASS);
    ArduinoOTA.onStart([]() {
      otaBusy = true; mqtt.disconnect(); FastLED.clear(true);
      oled.clearBuffer(); oled.setFont(u8g2_font_6x10_tf); oled.drawStr(0, 30, "OTA update..."); oled.sendBuffer();
    });
    ArduinoOTA.onEnd([]() { otaBusy = false; });
    ArduinoOTA.onError([](ota_error_t e) { otaBusy = false; Serial.printf("OTA error %u\n", e); });
    ArduinoOTA.begin();                            // also starts mDNS with the _arduino service
    startWeb();
    once = true;
  } else {
    MDNS.end();
    MDNS.begin(OTA_HOST);
    MDNS.enableArduino(3232, true);
  }
  MDNS.addService("http", "tcp", 80);
  Serial.printf("wifi up, ip %s, web + OTA at http://%s.local\n", WiFi.localIP().toString().c_str(), OTA_HOST);
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  oled.begin();
  connectWifiFirst(20000);                         // Wi-Fi before anything else
  loadLook();
  FastLED.addLeds<WS2812B, PIN_TOP, GRB>(leds, 0, LEDS_PER_PANE);
  FastLED.addLeds<WS2812B, PIN_BOTTOM, GRB>(leds, LEDS_PER_PANE, LEDS_PER_PANE);
  FastLED.setBrightness(look.bright);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
  FastLED.clear(true);
  for (int i = 0; i < 1440; i++) baseline[i] = {-1, 0, 0};
  net.setCACert(CA_CERT);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(16384);                       // the retained replay table is ~11.6 KB
  mqtt.setKeepAlive(30);
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  ensureWifi();                                    // starts web + OTA + mDNS if already connected
  Serial.println("ggak_window ready. Serial: t=test m=map c=columns x=exit");
}

uint32_t lastFrameMs = 0;
void loop() {
  ensureWifi(); ArduinoOTA.handle(); if (webStarted) web.handleClient();
  if (otaBusy) return;
  ensureMqtt(); mqtt.loop();
  handleSerial(); handleBoot();
  if (mode != RUN) { mapTick(); drawOled(); return; }
  selectSource(); testTick(); replayTick();
  if (millis() - lastFrameMs >= 12) { lastFrameMs = millis(); render(); }   // ~80 fps
  drawOled();
}

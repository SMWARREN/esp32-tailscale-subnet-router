// Hash fleet touchscreen monitor for the Hiwonder ESP32-S3 board.
// READ-ONLY: only HTTP GET /api/system/info. Never POST/PATCH to miners.
// Wi-Fi is entered by the user through a captive portal (WiFiManager);
// credentials are stored on the board only.
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include "Audio.h"   // ESP32-audioI2S: Google TTS streaming to I2S
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
extern "C" {
#include "microlink.h"
#include "microlink_internal.h"
#include "lwip/lwip_napt.h"
#include "lwip/tcpip.h"
}
#include "secrets.h"   // TS_AUTH_KEY (git-ignored, never committed)
#include <WebServer.h>
#include <Preferences.h>

// ---- Board pins (from the Hiwonder BSP port in milobeans/ESP32-DIV) ----
static const int PIN_MOSI = 47, PIN_SCLK = 21, PIN_CS = 2, PIN_DC = 3;
static const int TOUCH_SDA = 4, TOUCH_SCL = 5;        // FT6336 @0x38 (I2C0)
static const int EXP_SDA = 38, EXP_SCL = 48;          // XL9555 @0x20 (I2C1)
static const uint8_t EXP_ADDR = 0x20, TOUCH_ADDR = 0x38;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 panel;
  lgfx::Bus_SPI bus;
 public:
  LGFX() {
    auto b = bus.config();
    b.spi_host = SPI3_HOST; b.spi_mode = 0;
    b.freq_write = 40000000; b.freq_read = 16000000;
    b.pin_sclk = PIN_SCLK; b.pin_mosi = PIN_MOSI; b.pin_miso = -1; b.pin_dc = PIN_DC;
    b.dma_channel = SPI_DMA_CH_AUTO;
    bus.config(b); panel.setBus(&bus);
    auto p = panel.config();
    p.pin_cs = PIN_CS; p.pin_rst = -1; p.pin_busy = -1;
    p.panel_width = 240; p.panel_height = 320;
    p.invert = true; p.rgb_order = false; p.readable = false;
    panel.config(p);
    setPanel(&panel);
  }
};
LGFX lcd;
LGFX_Sprite tft(&lcd);  // full-frame buffer in PSRAM, pushed once per frame (no flicker)

// ---- Fleet (read-only) ----
struct Miner {
  const char* name; const char* ip; bool twelveV;
  bool online = false; uint32_t lastOk = 0;
  float hr = 0, hr1h = 0, expected = 0, temp = 0, vr = 0, power = 0;
  uint32_t shares = 0, uptime = 0; int fan = 0, freq = 0, mv = 0;
  double sessionBest = -1; int blocks = -1;   // -1 = not yet seen (no announcement on first poll)
  uint32_t lastUptime = 0;
  int fails = 0; bool announcedOffline = false; bool everOnline = false;
  float temp2 = 0, errPct = 0; uint32_t rejected = 0; double allBest = 0;
  int domActive = 0, domTotal = 0;
};
// Edit for your LAN. Each IP is advertised to Tailscale as a /32 route.
Miner miners[] = {
  {"Miner 1", "192.168.1.101", true},
  {"Miner 2", "192.168.1.102", true},
  {"Miner 3", "192.168.1.103", true},
  {"Miner 4", "192.168.1.104", true},
};
const int N = sizeof(miners) / sizeof(miners[0]);
int selected = -1;  // -1 = overview
uint32_t lastPoll = 0;
const uint32_t POLL_MS = 5000;
uint32_t lastFrame = 0;
const float TEMP_WARN = 65, TEMP_HOT = 70;
extern bool soundOn;
extern uint32_t soundToastUntil;
microlink_t* ml = nullptr;
bool awayMode = false;
void announce(Miner& m, double sb, int bf);
void poll(Miner& m);

// ---- XL9555: backlight bit 6 of port 1 (reg 3 output, reg 7 config) ----
bool expRead(uint8_t reg, uint8_t& v) {
  Wire1.beginTransmission(EXP_ADDR); Wire1.write(reg);
  if (Wire1.endTransmission(false)) return false;
  if (Wire1.requestFrom(EXP_ADDR, (uint8_t)1) != 1) return false;
  v = Wire1.read(); return true;
}
bool expUpdate(uint8_t reg, uint8_t mask, uint8_t bits) {
  uint8_t cur; if (!expRead(reg, cur)) return false;
  uint8_t nv = (cur & ~mask) | (bits & mask);
  if (nv == cur) return true;
  Wire1.beginTransmission(EXP_ADDR); Wire1.write(reg); Wire1.write(nv);
  return Wire1.endTransmission() == 0;
}
void backlight(bool on) {
  expUpdate(7, 0x40, 0);              // P1.6 as output
  expUpdate(3, 0x40, on ? 0x40 : 0);  // drive it
}

// ---- FT6336 touch ----
bool readTouch(int& x, int& y) {
  Wire.beginTransmission(TOUCH_ADDR); Wire.write(0x02);
  if (Wire.endTransmission(false)) return false;
  if (Wire.requestFrom(TOUCH_ADDR, (uint8_t)5) != 5) return false;
  uint8_t d[5]; for (auto& b : d) b = Wire.read();
  uint8_t n = d[0] & 0x0F;
  if (n < 1 || n > 2) return false;
  x = ((d[1] & 0x0F) << 8) | d[2];
  y = ((d[3] & 0x0F) << 8) | d[4];
  return x < 240 && y < 320;
}

// ---- Drawing helpers ----
const uint16_t BG = 0x0841, CARD = 0x18E3, MUTED = 0x8C51;
uint16_t tempColor(float t) { return t >= TEMP_HOT ? TFT_RED : t >= TEMP_WARN ? TFT_ORANGE : TFT_GREEN; }
String fmtHr(float gh) { return gh >= 1000 ? String(gh / 1000, 2) + " TH/s" : String(gh, 0) + " GH/s"; }
String fmtUp(uint32_t s) { return s >= 86400 ? String(s / 86400) + "d " + String((s % 86400) / 3600) + "h"
                                             : String(s / 3600) + "h " + String((s % 3600) / 60) + "m"; }

void header(const char* title) {
  tft.fillRect(0, 0, 240, 28, CARD);
  tft.setTextColor(TFT_WHITE, CARD); tft.setTextDatum(middle_left);
  tft.setFont(&fonts::FreeSansBold9pt7b); tft.drawString(title, 8, 14);
  tft.setFont(&fonts::Font0); tft.setTextDatum(middle_right);
  tft.setTextColor(WiFi.isConnected() ? TFT_GREEN : TFT_RED, CARD);
  String st = WiFi.isConnected() ? String((millis() - lastPoll) / 1000) + "s ago" : "no WiFi";
  tft.drawString(st, 232, 14);
  bool tsUp = ml && microlink_is_connected(ml);
  tft.setTextColor(tsUp ? (awayMode ? TFT_CYAN : TFT_GREEN) : MUTED, CARD);
  tft.drawString(awayMode ? "VPN" : "TS", 180, 14);
  tft.setTextColor(soundOn ? TFT_GREEN : MUTED, CARD);
  tft.drawString(soundOn ? "SND" : "MUTE", 148, 14);
}

String fmtDiffShort(double d) {
  const char* u[] = {"", "K", "M", "G", "T", "P"};
  int i = 0; while (d >= 1000 && i < 5) { d /= 1000; i++; }
  return String(d, d < 10 ? 2 : (d < 100 ? 1 : 0)) + u[i];
}
String fmtTemps(const Miner& m) {
  String s = String(m.temp, 1);
  if (m.temp2 > 0) s += "/" + String(m.temp2, 1);
  return s + " VR" + String(m.vr, 0);
}

// Layout mirrors `axe status`: 10m TH/s, chip/VR temps, accepted/rejected, W, plus session best.
void drawOverview() {
  tft.fillScreen(BG); header("Hash Fleet");
  float total = 0, watts = 0; int up = 0; double topSB = 0; int topI = -1;
  for (int i = 0; i < N; i++) if (miners[i].online) {
    total += miners[i].hr; watts += miners[i].power; up++;
    if (miners[i].sessionBest > topSB) { topSB = miners[i].sessionBest; topI = i; }
  }
  tft.setTextDatum(top_center); tft.setTextColor(TFT_WHITE, BG);
  tft.setFont(&fonts::FreeSansBold18pt7b); tft.drawString(fmtHr(total), 120, 32);
  tft.setFont(&fonts::Font2); tft.setTextColor(MUTED, BG);
  tft.drawString(String(up) + "/" + N + " online  " + String(watts, 0) + " W  live", 120, 64);
  tft.setTextColor(TFT_YELLOW, BG);
  tft.drawString(topI >= 0 ? "Top session best " + fmtDiffShort(topSB) + " " + miners[topI].name : "", 120, 80);
  for (int i = 0; i < N; i++) {
    Miner& m = miners[i]; int y = 98 + i * 55;
    uint16_t edge = !m.online ? TFT_RED : tempColor(max(m.temp, m.temp2));
    tft.fillRoundRect(6, y, 228, 52, 6, CARD); tft.fillRoundRect(6, y, 5, 52, 2, edge);
    tft.setFont(&fonts::Font2);
    tft.setTextDatum(top_left); tft.setTextColor(TFT_WHITE, CARD); tft.drawString(m.name, 16, y + 2);
    if (!m.online) {
      tft.setTextColor(TFT_RED, CARD); tft.drawString("OFFLINE", 16, y + 18);
      continue;
    }
    tft.setTextDatum(top_right); tft.setTextColor(TFT_YELLOW, CARD);
    tft.drawString("SB " + fmtDiffShort(m.sessionBest), 228, y + 2);
    tft.setTextDatum(top_left); tft.setTextColor(TFT_CYAN, CARD); tft.drawString(fmtHr(m.hr), 16, y + 18);
    tft.setTextDatum(top_right); tft.setTextColor(m.rejected ? TFT_ORANGE : MUTED, CARD);
    tft.drawString("A " + String(m.shares) + " / R " + String(m.rejected), 228, y + 18);
    tft.setTextDatum(top_left); tft.setTextColor(tempColor(max(m.temp, m.temp2)), CARD);
    tft.drawString(fmtTemps(m), 16, y + 34);
    tft.setTextDatum(top_right); tft.setTextColor(MUTED, CARD);
    String dom = m.domTotal ? String(m.domActive) + "/" + m.domTotal + "  " : "";
    tft.drawString(dom + String(m.power, 1) + " W", 228, y + 34);
  }
}

void row(int y, const char* k, String v, uint16_t c = TFT_WHITE) {
  tft.setFont(&fonts::Font2); tft.setTextDatum(top_left); tft.setTextColor(MUTED, BG); tft.drawString(k, 12, y);
  tft.setTextDatum(top_right); tft.setTextColor(c, BG); tft.drawString(v, 228, y);
}

void drawDetail(int i) {
  Miner& m = miners[i]; tft.fillScreen(BG); header(m.name);
  int y = 38;
  row(y, "Status", m.online ? "online" : "OFFLINE", m.online ? TFT_GREEN : TFT_RED); y += 20;
  row(y, "Address", m.ip); y += 20;
  row(y, "Supply", m.twelveV ? "12 V" : "5 V ONLY", m.twelveV ? TFT_WHITE : TFT_YELLOW); y += 20;
  if (m.lastOk) {
    row(y, "10m / 1h", fmtHr(m.hr) + " / " + fmtHr(m.hr1h), TFT_CYAN); y += 18;
    row(y, "Expected", fmtHr(m.expected)); y += 18;
    row(y, "Session best", fmtDiffShort(m.sessionBest), TFT_YELLOW); y += 18;
    row(y, "All-time best", fmtDiffShort(m.allBest), TFT_YELLOW); y += 18;
    row(y, "Chip temps", fmtTemps(m), tempColor(max(m.temp, m.temp2))); y += 18;
    row(y, "Power", String(m.power, 1) + " W"); y += 18;
    row(y, "Accepted / rej", String(m.shares) + " / " + m.rejected, m.rejected ? TFT_ORANGE : TFT_WHITE); y += 18;
    row(y, "Error rate", String(m.errPct, 2) + " %"); y += 18;
    if (m.domTotal) { row(y, "Domains", String(m.domActive) + "/" + m.domTotal, m.domActive == m.domTotal ? TFT_GREEN : TFT_ORANGE); y += 18; }
    row(y, "Freq / Vcore", String(m.freq) + " MHz / " + m.mv + " mV"); y += 18;
    row(y, "Fan / uptime", String(m.fan) + " rpm / " + fmtUp(m.uptime)); y += 18;
  }
  tft.setTextDatum(bottom_center); tft.setTextColor(MUTED, BG);
  tft.drawString("tap to go back  -  read only", 120, 316);
}

void redraw() {
  if (selected < 0) drawOverview(); else drawDetail(selected);
  if (millis() < soundToastUntil) {
    tft.fillRoundRect(40, 140, 160, 44, 8, CARD);
    tft.setTextDatum(middle_center); tft.setFont(&fonts::FreeSansBold12pt7b);
    tft.setTextColor(soundOn ? TFT_GREEN : TFT_ORANGE, CARD);
    tft.drawString(soundOn ? "Sound on" : "Sound off", 120, 162);
  }
  tft.pushSprite(0, 0); lastFrame = millis();
}

// ---- Speech (ES8311 codec over I2C0 @0x18, I2S out, speaker amp on XL9555 P1.7) ----
static const int I2S_MCLK = 45, I2S_BCLK = 39, I2S_WS = 41, I2S_DOUT = 42;
static const uint8_t ES_ADDR = 0x18;
Audio* audioPtr = nullptr;               // created at runtime in internal RAM (I2S DMA ctx can't live in PSRAM)
#define audio (*audioPtr)
String speechQueue[6]; int qHead = 0, qTail = 0;
bool speaking = false;
// Sound toggles with KEY1 (XL9555 P0.4, active low). Saved in NVS; defaults to OFF.
static const uint16_t KEY1_MASK = 0x0010;
bool soundOn = false;
uint16_t lastInputs = 0xFFFF;
uint32_t soundToastUntil = 0;
void applySound(bool on) {
  soundOn = on;
  expUpdate(3, 0x80, on ? 0x80 : 0);       // speaker amp follows the setting
  if (!on) { audio.stopSong(); speaking = false; qHead = qTail; }
  Preferences p; p.begin("fleet", false); p.putBool("sound", on); p.end();
  Serial.printf("[sound] %s\n", on ? "ON" : "OFF");
}
void readInputs() {
  uint8_t p0, p1;
  static uint32_t fails = 0, lastFailLog = 0;
  if (!expRead(0, p0) || !expRead(1, p1)) {
    fails++;
    if (millis() - lastFailLog > 5000) { lastFailLog = millis(); Serial.printf("[inputs] read FAILED x%lu\n", (unsigned long)fails); }
    return;
  }
  uint16_t v = p0 | (p1 << 8);
  if (v != lastInputs) Serial.printf("[inputs] 0x%04X\n", v);
  bool wasDown = !(lastInputs & KEY1_MASK), isDown = !(v & KEY1_MASK);
  static bool primed = false;
  if (primed && isDown && !wasDown) {   // press edge only
    applySound(!soundOn);
    soundToastUntil = millis() + 1500;
    redraw();
  }
  lastInputs = v; primed = true;
}
void say(const String& s) {
  if (!soundOn) { Serial.println("[say:muted] " + s); return; }
  int next = (qTail + 1) % 6;
  if (next == qHead) return;               // queue full: drop
  speechQueue[qTail] = s; qTail = next;
  Serial.println("[say] " + s);
}
void esWrite(uint8_t r, uint8_t v) { Wire.beginTransmission(ES_ADDR); Wire.write(r); Wire.write(v); Wire.endTransmission(); }
void es8311Init() {
  // Slave mode, MCLK from pin, 16-bit I2S, DAC only. Mirrors espressif esp_codec_dev es8311_open/start.
  esWrite(0x44, 0x08); esWrite(0x44, 0x08);
  esWrite(0x01, 0x30); esWrite(0x02, 0x00); esWrite(0x03, 0x10); esWrite(0x16, 0x24);
  esWrite(0x04, 0x10); esWrite(0x05, 0x00); esWrite(0x0B, 0x00); esWrite(0x0C, 0x00);
  esWrite(0x10, 0x1F); esWrite(0x11, 0x7F); esWrite(0x00, 0x80);
  esWrite(0x01, 0xBF);                     // all clocks on, internal MCLK derived from BCLK (rate independent)
  esWrite(0x02, 0x18);                     // BCLK x8 => 256*fs for 16-bit stereo, any sample rate
  esWrite(0x06, 0x03);                     // BCLK divider (unused in slave)
  esWrite(0x13, 0x10); esWrite(0x1B, 0x0A); esWrite(0x1C, 0x6A); esWrite(0x44, 0x58);
  esWrite(0x09, 0x0C); esWrite(0x0A, 0x0C); // 16-bit I2S in/out
  esWrite(0x17, 0xBF); esWrite(0x0E, 0x02); esWrite(0x12, 0x00); esWrite(0x14, 0x1A);
  esWrite(0x0D, 0x01); esWrite(0x15, 0x40); esWrite(0x37, 0x08); esWrite(0x45, 0x00);
  esWrite(0x31, 0x00);                     // unmute
  esWrite(0x32, 0xBF);                     // DAC volume ~0 dB
  expUpdate(7, 0x80, 0);                   // P1.7 (AUDIO_EN) as output
  expUpdate(3, 0x80, 0);                   // speaker amp off until the sound switch is ON
}
void speechLoop() {
  static uint32_t lastIn = 0;
  if (millis() - lastIn > 200) { lastIn = millis(); readInputs(); }
  if (!soundOn) return;
  audio.loop();
  if (speaking && !audio.isRunning()) speaking = false;
  if (!speaking && qHead != qTail && WiFi.isConnected()) {
    String s = speechQueue[qHead]; qHead = (qHead + 1) % 6;
    speaking = audio.connecttospeech(s.c_str(), "en");
  }
}
String fmtDiff(double d) {
  const char* u[] = {"", " thousand", " million", " billion", " trillion"};
  int i = 0; while (d >= 1000 && i < 4) { d /= 1000; i++; }
  return String(d, d < 10 ? 2 : (d < 100 ? 1 : 0)) + u[i];
}

void checkOnline(Miner& m) {
  if (m.online) {
    if (m.announcedOffline) say(String(m.name) + " is back online.");
    m.fails = 0; m.announcedOffline = false; m.everOnline = true;
  } else if (++m.fails == 3 && m.everOnline && !m.announcedOffline) {  // ~15 s of failures, no blips
    say(String("Warning. ") + m.name + " is offline.");
    m.announcedOffline = true;
  }
}

// ---- Tailscale relay (MicroLink) ----
// A board that can reach the miners on its LAN pushes read-only snapshots to the other
// hashfleet-* board over the tailnet. A board away from home shows those snapshots.
// Nothing received over the tailnet is ever sent to a miner.
static const uint16_t RELAY_PORT = 47800;
microlink_udp_socket_t* relaySock = nullptr;
struct Remote { volatile bool fresh = false; uint32_t at = 0; char buf[400]; } remoteSnap[4];
portMUX_TYPE remoteMux = portMUX_INITIALIZER_UNLOCKED;
int localFailRounds = 0;

bool isFleetPeer(uint32_t ip) {
  int n = microlink_get_peer_count(ml);
  for (int i = 0; i < n; i++) {
    microlink_peer_info_t p;
    if (microlink_get_peer_info(ml, i, &p) == ESP_OK && p.vpn_ip == ip)
      return strncmp(p.hostname, "hashfleet-", 10) == 0;
  }
  return false;
}

void onRelayRx(microlink_udp_socket_t*, uint32_t src, uint16_t, const uint8_t* data, size_t len, void*) {
  if (len == 0 || len >= sizeof(remoteSnap[0].buf) || !isFleetPeer(src)) return;
  JsonDocument d; if (deserializeJson(d, (const char*)data, len)) return;
  int i = d["i"] | -1; if (i < 0 || i >= N) return;
  portENTER_CRITICAL(&remoteMux);
  memcpy(remoteSnap[i].buf, data, len); remoteSnap[i].buf[len] = 0;
  remoteSnap[i].at = millis(); remoteSnap[i].fresh = true;
  portEXIT_CRITICAL(&remoteMux);
}

// ---- Read-only status page (phone over Tailscale, or home Wi-Fi) ----
// Count tailnet packets addressed to the miners (validation of the subnet route).
volatile uint32_t fwdTotal = 0, fwdBy[8] = {0};
uint32_t minerAddr[8] = {0};   // network byte order, filled at boot from miners[]
netif_input_fn origWgInput = nullptr;
static uint32_t lastDiag = 0;
err_t countingInput(struct pbuf* p, struct netif* inp) {
  if (p && p->len >= 20 && (((uint8_t*)p->payload)[0] >> 4) == 4) {
    uint32_t dst; memcpy(&dst, (const uint8_t*)p->payload + 16, 4);
    for (int i = 0; i < N && i < 8; i++) if (dst == minerAddr[i]) { fwdTotal++; fwdBy[i]++; break; }
  }
  return origWgInput(p, inp);
}
// GET only. It serves the display's own cached readings; it never contacts a miner on request.
WebServer web(80);
void handleJson() {
  JsonDocument d;
  d["tailscale"] = ml && microlink_is_connected(ml);
  d["relayedPackets"] = fwdTotal;
  d["pollAgeS"] = (millis() - lastPoll) / 1000;
  for (int i = 0; i < N; i++) {
    Miner& m = miners[i]; JsonObject o = d["miners"].add<JsonObject>();
    o["name"] = m.name; o["online"] = m.online;
    o["ghs"] = m.hr; o["ghs1h"] = m.hr1h; o["tempC"] = m.temp; o["temp2C"] = m.temp2; o["vrC"] = m.vr;
    o["watts"] = m.power; o["accepted"] = m.shares; o["rejected"] = m.rejected;
    o["uptimeS"] = m.uptime; o["fanRpm"] = m.fan; o["mhz"] = m.freq; o["mv"] = m.mv;
    o["sessionBest"] = m.sessionBest; o["allTimeBest"] = m.allBest; o["errPct"] = m.errPct;
  }
  String s; serializeJson(d, s);
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", s);
}
void handleRoot() {
  float total = 0, watts = 0; int up = 0;
  for (auto& m : miners) if (m.online) { total += m.hr; watts += m.power; up++; }
  String h = F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<meta http-equiv=refresh content=10><title>Hash Fleet</title>"
    "<style>body{font:16px -apple-system,sans-serif;background:#111;color:#eee;margin:16px}"
    ".c{background:#222;border-radius:10px;padding:12px;margin:10px 0}.off{color:#f55}.m{color:#999;font-size:14px}"
    "h1{margin:0}</style>");
  h += "<h1>" + fmtHr(total) + "</h1><div class=m>" + up + "/" + N + " online &middot; " + String(watts, 0) +
       " W &middot; updated " + (millis() - lastPoll) / 1000 + "s ago</div>";
  for (auto& m : miners) {
    h += "<div class=c><b>" + String(m.name) + "</b> ";
    if (!m.online) { h += "<span class=off>offline</span></div>"; continue; }
    h += fmtHr(m.hr) + "<div class=m>" + fmtTemps(m) + " &deg;C &middot; " + String(m.power, 1) + " W &middot; " +
         m.shares + "/" + m.rejected + " shares &middot; best " + fmtDiffShort(m.sessionBest) + " &middot; up " + fmtUp(m.uptime) +
         "</div></div>";
  }
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "text/html", h);
}

void tailscaleLoop() {
  web.handleClient();
  if (ml && millis() - lastDiag > 15000) {
    lastDiag = millis();
    microlink_diag_t dg;
    if (microlink_get_diag(ml, &dg) == ESP_OK) {
      char ip[16]; microlink_ip_to_str(dg.vpn_ip, ip);
      Serial.printf("[ts] state=%d conn=%d ip=%s peers=%d/%d derp=%u away=%d heap=%u psram=%u\n",
        dg.state, dg.connected, ip, dg.peer_online, dg.peer_count, dg.derp_home_region, awayMode,
        (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
      Serial.printf("[relay] tailnet->miner packets: %lu (", (unsigned long)fwdTotal);
      for (int i = 0; i < N && i < 8; i++) Serial.printf("%s%s=%lu", i ? " " : "", miners[i].ip, (unsigned long)fwdBy[i]);
      Serial.println(")");
    }
  }
  if (!ml || !microlink_is_connected(ml)) return;
  static void* naptNetif = nullptr;
  void* wg = ml->wg_netif;
  if (wg && wg != naptNetif) {
    struct netif* n = (struct netif*)wg;
    LOCK_TCPIP_CORE();
    if (n->input != countingInput) { origWgInput = n->input; n->input = countingInput; }
    // Translate tailnet traffic to the miners so their replies come back via this board.
    int ok = ip_napt_enable_netif((struct netif*)wg, 1);
    UNLOCK_TCPIP_CORE();
    Serial.printf("[ts] subnet NAT on wg: %s\n", ok ? "on" : "FAILED");
    if (ok) naptNetif = wg;
  }
  if (!relaySock) {
    relaySock = microlink_udp_create(ml, RELAY_PORT);
    if (relaySock) microlink_udp_set_rx_callback(relaySock, onRelayRx, nullptr);
  }
}

void pushSnapshots() {
  if (!relaySock || awayMode) return;
  uint32_t me = microlink_get_vpn_ip(ml);
  int n = microlink_get_peer_count(ml);
  for (int p = 0; p < n; p++) {
    microlink_peer_info_t info;
    if (microlink_get_peer_info(ml, p, &info) != ESP_OK || info.vpn_ip == me || !info.online) continue;
    if (strncmp(info.hostname, "hashfleet-", 10) != 0) continue;
    for (int i = 0; i < N; i++) {
      Miner& m = miners[i];
      if (!m.online) continue;
      char b[400];
      int len = snprintf(b, sizeof b,
        "{\"i\":%d,\"hr\":%.2f,\"h1\":%.2f,\"ex\":%.2f,\"t\":%.1f,\"t2\":%.1f,\"vr\":%.1f,\"w\":%.1f,"
        "\"sa\":%lu,\"sr\":%lu,\"up\":%lu,\"fan\":%d,\"f\":%d,\"mv\":%d,\"sb\":%.0f,\"bd\":%.0f,"
        "\"bf\":%d,\"er\":%.2f,\"da\":%d,\"dt\":%d}",
        i, m.hr, m.hr1h, m.expected, m.temp, m.temp2, m.vr, m.power,
        (unsigned long)m.shares, (unsigned long)m.rejected, (unsigned long)m.uptime, m.fan, m.freq, m.mv,
        m.sessionBest, m.allBest, m.blocks, m.errPct, m.domActive, m.domTotal);
      if (len > 0 && len < (int)sizeof b) microlink_udp_send(relaySock, info.vpn_ip, RELAY_PORT, (const uint8_t*)b, len);
    }
  }
}

// Apply a relayed snapshot, running the same announcements as a local poll.
void applyRemote(Miner& m, int i) {
  char b[400]; uint32_t at;
  portENTER_CRITICAL(&remoteMux);
  bool fresh = remoteSnap[i].fresh; at = remoteSnap[i].at;
  if (fresh) memcpy(b, remoteSnap[i].buf, sizeof b);
  portEXIT_CRITICAL(&remoteMux);
  if (!fresh || millis() - at > 20000) { m.online = false; return; }
  JsonDocument d; if (deserializeJson(d, b)) { m.online = false; return; }
  m.hr = d["hr"] | 0.0f; m.hr1h = d["h1"] | 0.0f; m.expected = d["ex"] | 0.0f;
  m.temp = d["t"] | 0.0f; m.temp2 = d["t2"] | 0.0f; m.vr = d["vr"] | 0.0f; m.power = d["w"] | 0.0f;
  m.shares = d["sa"] | 0; m.rejected = d["sr"] | 0; m.uptime = d["up"] | 0;
  m.fan = d["fan"] | 0; m.freq = d["f"] | 0; m.mv = d["mv"] | 0;
  m.allBest = d["bd"] | 0.0; m.errPct = d["er"] | 0.0f; m.domActive = d["da"] | 0; m.domTotal = d["dt"] | 0;
  m.online = true; m.lastOk = at;
  announce(m, d["sb"] | 0.0, d["bf"] | 0);
}

void announce(Miner& m, double sb, int bf) {
  bool rebooted = m.uptime < m.lastUptime;  // session best resets on reboot: re-baseline silently
  if (m.blocks >= 0 && bf > m.blocks)
    for (int i = 0; i < 3; i++) say(String("Block found! ") + m.name + " just found a Bitcoin block!");
  else if (m.sessionBest >= 0 && !rebooted && sb > m.sessionBest)
    say(String("New session best on ") + m.name + ", " + fmtDiff(sb) + ".");
  m.sessionBest = sb; m.blocks = bf; m.lastUptime = m.uptime;
}

// Local first. After two rounds where no miner answers on the LAN, switch to
// the tailnet relay, retrying the LAN once a minute (e.g. when brought home).
void pollAll() {
  static uint32_t lastLanTry = 0;
  bool tryLan = !awayMode || millis() - lastLanTry > 60000;
  int localUp = 0;
  if (tryLan) {
    lastLanTry = millis();
    for (auto& m : miners) { poll(m); if (m.online) localUp++; }
    if (localUp) { localFailRounds = 0; awayMode = false; }
    else if (++localFailRounds >= 2 && ml) awayMode = true;
  }
  if (awayMode) for (int i = 0; i < N; i++) applyRemote(miners[i], i);
  for (auto& m : miners) checkOnline(m);
  pushSnapshots();
}

// ---- Polling (GET only) ----
void poll(Miner& m) {
  HTTPClient http; http.setTimeout(4000); http.setConnectTimeout(3000);
  String url = String("http://") + m.ip + "/api/system/info";
  if (!http.begin(url)) { m.online = false; return; }
  int code = http.GET();
  if (code == 200) {
    JsonDocument filter;
    for (auto k : {"hashRate", "hashRate_1h", "expectedHashrate", "temp", "vrTemp", "power",
                   "sharesAccepted", "uptimeSeconds", "fanrpm", "frequency", "coreVoltage",
                   "bestSessionDiff", "blockFound", "hashRate_10m", "temp2", "sharesRejected",
                   "bestDiff", "errorPercentage"}) filter[k] = true;
    filter["hashrateMonitor"]["asics"][0]["domains"] = true;
    JsonDocument d;
    if (!deserializeJson(d, http.getStream(), DeserializationOption::Filter(filter))) {
      m.hr = d["hashRate"] | 0.0f; m.hr1h = d["hashRate_1h"] | 0.0f; m.expected = d["expectedHashrate"] | 0.0f;
      m.temp = d["temp"] | 0.0f; m.vr = d["vrTemp"] | 0.0f; m.power = d["power"] | 0.0f;
      m.shares = d["sharesAccepted"] | 0; m.uptime = d["uptimeSeconds"] | 0;
      m.fan = d["fanrpm"] | 0; m.freq = (int)(d["frequency"] | 0.0f); m.mv = d["coreVoltage"] | 0;
      // Live hashrate (same as HashFleet), not the 10-minute average.
      m.temp2 = d["temp2"] | 0.0f; m.rejected = d["sharesRejected"] | 0;
      m.allBest = d["bestDiff"] | 0.0; m.errPct = d["errorPercentage"] | 0.0f;
      m.domActive = m.domTotal = 0;
      for (JsonObject a : d["hashrateMonitor"]["asics"].as<JsonArray>())
        for (float v : a["domains"].as<JsonArray>()) { m.domTotal++; if (v > 0) m.domActive++; }
      m.online = true; m.lastOk = millis();
      announce(m, d["bestSessionDiff"] | 0.0, d["blockFound"] | 0);
    } else m.online = false;
  } else m.online = false;
  http.end();
}

void splash(const char* l1, const char* l2, const char* l3 = "") {
  tft.fillScreen(BG); tft.setTextDatum(middle_center);
  tft.setTextColor(TFT_WHITE, BG); tft.setFont(&fonts::FreeSansBold12pt7b); tft.drawString(l1, 120, 110);
  tft.setFont(&fonts::Font2); tft.setTextColor(TFT_CYAN, BG); tft.drawString(l2, 120, 150);
  tft.setTextColor(MUTED, BG); tft.drawString(l3, 120, 175);
  tft.pushSprite(0, 0);
}

void portalStarted(WiFiManager*) {
  splash("Wi-Fi setup", "Join hotspot: FleetMonitor-Setup", "then open 192.168.4.1");
}

void setup() {
  // Create the I2S channel first, while internal RAM is plentiful (GDMA ctx must be internal).
  heap_caps_malloc_extmem_enable(1 << 20);
  audioPtr = new (heap_caps_malloc(sizeof(Audio), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) Audio();
  heap_caps_malloc_extmem_enable(4096);
  Serial.begin(115200);
  Serial.printf("[audio] obj in %s\n", esp_ptr_internal(audioPtr) ? "internal" : "PSRAM");
  Wire1.begin(EXP_SDA, EXP_SCL, 400000); Wire1.setTimeOut(25);
  Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000); Wire.setTimeOut(25);
  lcd.init(); lcd.setRotation(0); backlight(true);
  es8311Init();
  { Preferences p; p.begin("fleet", true); soundOn = p.getBool("sound", false); p.end(); }
  expUpdate(3, 0x80, soundOn ? 0x80 : 0);
  Serial.printf("[sound] boot: %s\n", soundOn ? "ON" : "OFF");
  audio.setPinout(I2S_BCLK, I2S_WS, I2S_DOUT);
  audio.setVolume(18);
  tft.setColorDepth(16); tft.setPsram(true); tft.createSprite(240, 320);
  splash("Hash Fleet", "connecting to Wi-Fi...");

  // Hold the screen during boot to re-open Wi-Fi setup.
  int tx, ty; bool forcePortal = false;
  delay(300);
  if (readTouch(tx, ty)) { delay(2500); forcePortal = readTouch(tx, ty); }

  WiFiManager wm;
  wm.setAPCallback(portalStarted);
  wm.setConfigPortalTimeout(0);
  wm.setConnectTimeout(20);
  wm.setTitle("Hash Fleet Monitor");
  bool ok = forcePortal ? wm.startConfigPortal("FleetMonitor-Setup")
                        : wm.autoConnect("FleetMonitor-Setup");
  if (!ok) ESP.restart();
  WiFi.setAutoReconnect(true);
  splash("Connected", WiFi.localIP().toString().c_str(), "starting Tailscale...");
  {
    static char devName[24];
    uint8_t mac[6]; WiFi.macAddress(mac);
    snprintf(devName, sizeof devName, "hashfleet-%02x%02x", mac[4], mac[5]);
    microlink_config_t c = {};
    c.auth_key = TS_AUTH_KEY; c.device_name = devName;
    c.enable_derp = true; c.enable_stun = true; c.enable_disco = true; c.max_peers = 16;
    // Subnet router for the four miners only (/32 each). Needs one-time approval in the
    // Tailscale admin console. Lets apps on the tailnet use the devices' normal LAN addresses.
    static char routes[256] = "";
    for (int i = 0; i < N; i++) {
      strlcat(routes, miners[i].ip, sizeof routes); strlcat(routes, i < N - 1 ? "/32\n" : "/32", sizeof routes);
      if (i < 8) minerAddr[i] = ipaddr_addr(miners[i].ip);
    }
    c.advertise_routes = routes;
    ml = microlink_init(&c);
    if (ml && microlink_start(ml) != ESP_OK) ml = nullptr;
    Serial.printf("[ts] %s as %s\n", ml ? "started" : "FAILED", devName);
  }
  splash("Connected", WiFi.localIP().toString().c_str(), "loading miners...");
  pollAll();
  lastPoll = millis(); redraw();
  ArduinoOTA.setHostname("fleet-monitor");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { audio.stopSong(); splash("Updating...", "over Wi-Fi", "do not unplug"); });
  ArduinoOTA.begin();
  web.on("/", HTTP_GET, handleRoot);
  web.on("/api/status", HTTP_GET, handleJson);
  web.onNotFound([]() { web.send(404, "text/plain", "not found"); });
  web.begin();
  say("Hash fleet monitor online.");
}

void loop() {
  static bool wasDown = false;
  int x, y; bool down = readTouch(x, y);
  if (down && !wasDown) {
    if (selected >= 0) selected = -1;
    else { for (int i = 0; i < N; i++) if (y >= 94 + i * 56 && y < 144 + i * 56) selected = i; }
    redraw();
  }
  wasDown = down;
  if (millis() - lastPoll >= POLL_MS) {
    if (!WiFi.isConnected()) WiFi.reconnect();
    pollAll();
    lastPoll = millis(); redraw();
  }
  tailscaleLoop();
  if (millis() - lastFrame >= 1000) redraw();  // keeps the "Xs ago" counter live
  ArduinoOTA.handle();
  speechLoop();
  delay(speaking ? 1 : 20);
}

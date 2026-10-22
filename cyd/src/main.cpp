#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include "secrets.h"

time_t parseIsoEpoch(const String& s);

// ---------------------------------------------------------------------------
// Matomo Notifier card, mirroring frontend/src (main.ts + style.css):
// ocean background, glass visit card, history dots, tap to step through history
// ---------------------------------------------------------------------------
const unsigned long POLL_MS = 2000;
const int HISTORY_MAX = 10;

// Mirror of frontend/src/style.css palette
#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
constexpr uint16_t COL_SKY_TOP = RGB565(0x3D, 0x74, 0xE0);
constexpr uint16_t COL_SKY_BOT = RGB565(0x9F, 0xBD, 0xF3);
constexpr uint16_t COL_WATER_TOP = RGB565(0x25, 0x63, 0xD1);
constexpr uint16_t COL_WATER_BOT = RGB565(0x0D, 0x36, 0x90);
constexpr uint16_t COL_ICE = RGB565(0x8F, 0xD0, 0xFF);
constexpr uint16_t COL_BORDER = RGB565(0xBE, 0xDC, 0xFF);
constexpr uint16_t COL_HIGHLIGHT = RGB565(0xCF, 0xE4, 0xFF);
constexpr uint16_t COL_TEXT = 0xFFFF;
constexpr uint16_t COL_MUTED = RGB565(0xDE, 0xEA, 0xFD);
constexpr uint16_t COL_RED = RGB565(0xFF, 0xD2, 0xD2);
constexpr uint16_t COL_LETTER = RGB565(0x4F, 0x8C, 0xFF);
constexpr uint16_t COL_BUBBLE = RGB565(0xBF, 0xDA, 0xFD);
constexpr uint16_t COL_DOT = RGB565(0x5D, 0x84, 0xCD);
// White at ~24% over the water blue the card sits on
constexpr uint16_t COL_CARD = RGB565(0x59, 0x88, 0xDC);

TFT_eSPI tft;

struct Visit {
  String id;
  String siteName;
  String city;
  String country;
  String deviceType;
  String visitStart;
  int actions = 0;
  int goals = 0;
};

Visit history[HISTORY_MAX];
int historyLen = 0;
int viewIndex = 0;
unsigned long cursor_ = 0;
bool hasError = false;
String lastRenderKey;
unsigned long lastPoll = 0;

// ---------------------------------------------------------------------------
// XPT2046 resistive touch (bit-banged on the touch-only SPI pins) — same
// device + Freenove calibration as csstats/cyd
// ---------------------------------------------------------------------------
constexpr int T_CS = 33;
constexpr int T_CLK = 25;
constexpr int T_DIN = 32;
constexpr int T_DOUT = 39;

void touchInit() {
  pinMode(T_CS, OUTPUT);
  digitalWrite(T_CS, HIGH);
  pinMode(T_CLK, OUTPUT);
  digitalWrite(T_CLK, LOW);
  pinMode(T_DIN, OUTPUT);
  digitalWrite(T_DIN, LOW);
  pinMode(T_DOUT, INPUT);
}

uint16_t touchReadAxis(bool axis) {
  digitalWrite(T_CS, LOW);
  const uint8_t cmd = axis ? 0x90 : 0xD0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(T_DIN, (cmd >> i) & 1);
    digitalWrite(T_CLK, HIGH);
    digitalWrite(T_CLK, LOW);
  }
  digitalWrite(T_CLK, HIGH);
  digitalWrite(T_CLK, LOW);
  uint16_t v = 0;
  for (int i = 11; i >= 0; i--) {
    v |= (uint16_t)digitalRead(T_DOUT) << i;
    digitalWrite(T_CLK, HIGH);
    digitalWrite(T_CLK, LOW);
  }
  digitalWrite(T_CS, HIGH);
  digitalWrite(T_DIN, LOW);
  return v;
}

bool screenTouched() {
  const uint16_t xr = touchReadAxis(true);
  const uint16_t yr = touchReadAxis(false);
  return xr > 6 && xr < 4090 && yr > 6 && yr < 4090;
}

// ---------------------------------------------------------------------------
// Layout: 320x240 landscape. Same vertical flow as the web card (head, hero,
// caption, two stat columns), with y values given as text baselines.
// ---------------------------------------------------------------------------
constexpr int16_t SCREEN_W = 320;
constexpr int16_t SCREEN_H = 240;
constexpr int16_t HEADER_H = 30;
constexpr int16_t HEADER_BASE = 21;
constexpr int16_t WATERLINE = 82;  // --waterline: 34vh
constexpr int16_t CARD_X = 10;
constexpr int16_t CARD_Y = 34;
constexpr int16_t CARD_W = SCREEN_W - 2 * CARD_X;
constexpr int16_t CARD_H = 192;
constexpr int16_t EMPTY_H = 96;
constexpr int16_t DOTS_Y = CARD_Y + CARD_H + 7;
constexpr int16_t DOT_STEP = 12;

// Card content box
constexpr int16_t PAD = 12;
constexpr int16_t CX = CARD_X + PAD;
constexpr int16_t CR = CARD_X + CARD_W - PAD;
// .card-head: favicon + title block (site name / meta line)
constexpr int16_t FAV_Y = CARD_Y + 10;
constexpr int16_t FAV_S = 34;
constexpr int16_t TITLE_X = CX + FAV_S + 10;
constexpr int16_t SITE_BASE = FAV_Y + 17;
constexpr int16_t META_BASE = FAV_Y + 33;
// .hero + .hero-caption
constexpr int16_t HERO_BASE = FAV_Y + FAV_S + 12 + 33;
constexpr int16_t CAPTION_BASE = HERO_BASE + 18;
// .stats: icon, value, label per column; dashed divider between them.
// Columns are not 1fr/1fr: "Goal conversions" needs ~139px in FreeSans9pt.
constexpr int16_t STATS_Y = CAPTION_BASE + 15;
constexpr int16_t ICON_R = 9;
constexpr int16_t VALUE_BASE = STATS_Y + 2 * ICON_R + 4 + 17;
constexpr int16_t LABEL_BASE = VALUE_BASE + 17;
constexpr int16_t DIV_X = CX + 122;
constexpr int16_t COL2_X = DIV_X + 12;

uint16_t lerpColor(uint16_t a, uint16_t b, int t, int total) {
  const int ra = (a >> 11) & 0x1F, ga = (a >> 5) & 0x3F, ba = a & 0x1F;
  const int rb = (b >> 11) & 0x1F, gb = (b >> 5) & 0x3F, bb = b & 0x1F;
  return (uint16_t)(((ra + (rb - ra) * t / total) << 11) |
                    ((ga + (gb - ga) * t / total) << 5) |
                    (ba + (bb - ba) * t / total));
}

uint16_t backgroundColorAt(int16_t y) {
  if (y < WATERLINE) return lerpColor(COL_SKY_TOP, COL_SKY_BOT, y, WATERLINE);
  return lerpColor(COL_WATER_TOP, COL_WATER_BOT, y - WATERLINE, SCREEN_H - WATERLINE);
}

void drawBackground(int16_t x = 0, int16_t y0 = 0, int16_t y1 = SCREEN_H) {
  for (int y = y0; y < y1; y++) tft.drawFastHLine(x, y, SCREEN_W - x, backgroundColorAt(y));
}

void drawBubbles() {
  // Decorative bubbles under the surface (mostly behind the card)
  tft.fillSmoothCircle(28, 160, 8, COL_BUBBLE, backgroundColorAt(160));
  tft.fillSmoothCircle(37, 175, 5, COL_BUBBLE, backgroundColorAt(175));
  tft.fillSmoothCircle(286, 140, 6, COL_BUBBLE, backgroundColorAt(140));
  tft.fillSmoothCircle(278, 156, 4, COL_BUBBLE, backgroundColorAt(156));
  tft.fillSmoothCircle(272, 98, 3, COL_BUBBLE, backgroundColorAt(98));
}

// Shorten s with "..." until it fits maxW px in the current font
// (CSS text-overflow: ellipsis). Never cuts a UTF-8 sequence in half.
String fitText(String s, int16_t maxW) {
  if (tft.textWidth(s) <= maxW) return s;
  while (s.length() && tft.textWidth(s + "...") > maxW) {
    int n = s.length() - 1;
    while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) n--;
    s.remove(n);
  }
  s.trim();
  return s + "...";
}

void drawHeader() {
  tft.setTextDatum(L_BASELINE);
  tft.setTextColor(COL_TEXT);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.drawString("MATOMO NOTIFIER", CARD_X, HEADER_BASE);
}

void drawStatus() {
  // .status is the smallest text on the page (0.8rem vs the 1.6rem title), so
  // use the small built-in font; it also keeps "backend unreachable" clear of
  // the title. Repaint only the right-hand part of the header.
  constexpr int16_t STATUS_X = 181;
  drawBackground(STATUS_X, 0, HEADER_H);
  tft.setTextDatum(R_BASELINE);
  tft.setTextColor(hasError ? COL_RED : COL_MUTED);
  tft.setTextFont(2);
  tft.drawString(hasError ? "backend unreachable" : "connected", CARD_X + CARD_W, HEADER_BASE);
}

// ---------------------------------------------------------------------------
// Card (frontend: .card / .card-head / .hero / .stats)
// ---------------------------------------------------------------------------
void drawGlass(int16_t h, uint16_t fill) {
  tft.fillSmoothRoundRect(CARD_X, CARD_Y, CARD_W, h, 12, COL_BORDER, backgroundColorAt(CARD_Y + h / 2));
  tft.fillSmoothRoundRect(CARD_X + 2, CARD_Y + 2, CARD_W - 4, h - 4, 10, fill, COL_BORDER);
  tft.drawFastHLine(CARD_X + 12, CARD_Y + 2, CARD_W - 24, COL_HIGHLIGHT);
}

void drawFavicon(const String& siteName) {
  // Web loads the site favicon; the CYD fallback is the letter placeholder
  // (utils.ts placeholder(): #4f8cff letter on a white tile)
  tft.fillSmoothRoundRect(CX, FAV_Y, FAV_S, FAV_S, 8, COL_TEXT, COL_CARD);
  tft.setTextColor(COL_LETTER);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b);
  char letter[2] = { siteName.isEmpty() ? '?' : (char)toupper(siteName[0]), '\0' };
  tft.drawString(letter, CX + FAV_S / 2, FAV_Y + FAV_S / 2 + 1);
}

void drawRing(int16_t cx, int16_t cy, int16_t r) {
  tft.fillSmoothCircle(cx, cy, r, COL_ICE, COL_CARD);
  tft.fillSmoothCircle(cx, cy, r - 2, COL_CARD, COL_ICE);
}

void drawIconClock(int16_t cx, int16_t cy) {
  drawRing(cx, cy, ICON_R);
  tft.drawWideLine(cx, cy, cx, cy - 5, 2, COL_ICE, COL_CARD);
  tft.drawWideLine(cx, cy, cx + 3, cy + 2, 2, COL_ICE, COL_CARD);
}

void drawIconTarget(int16_t cx, int16_t cy) {
  drawRing(cx, cy, ICON_R);
  drawRing(cx, cy, 5);
  tft.fillSmoothCircle(cx, cy, 1, COL_ICE, COL_CARD);
}

void drawStat(int16_t x, int16_t maxW, bool clockIcon, const char* value, const char* label) {
  if (clockIcon) drawIconClock(x + ICON_R, STATS_Y + ICON_R);
  else drawIconTarget(x + ICON_R, STATS_Y + ICON_R);
  tft.setTextDatum(L_BASELINE);
  tft.setTextColor(COL_TEXT);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.drawString(value, x, VALUE_BASE);
  tft.setTextColor(COL_MUTED);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.drawString(fitText(label, maxW), x, LABEL_BASE);
}

void drawCard(const Visit& v) {
  drawGlass(CARD_H, COL_CARD);
  drawFavicon(v.siteName);

  // Text is drawn transparently: render() repaints the whole card, and an
  // opaque text box would clip neighbouring lines
  tft.setTextDatum(L_BASELINE);
  tft.setTextColor(COL_TEXT);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.drawString(fitText(v.siteName, CR - TITLE_X), TITLE_X, SITE_BASE);

  // .meta: "<city, country> · <device>", the location is what gets truncated
  tft.setTextColor(COL_MUTED);
  tft.setFreeFont(&FreeSans9pt7b);
  String location = v.city.length() && v.country.length() ? v.city + ", " + v.country
                    : v.city.length() ? v.city
                    : v.country.length() ? v.country
                    : String("Unknown location");
  const int16_t deviceW = tft.textWidth(v.deviceType);
  const int16_t locW = tft.drawString(fitText(location, CR - TITLE_X - deviceW - 18), TITLE_X, META_BASE);
  const int16_t sepX = TITLE_X + locW + 8;
  tft.fillSmoothCircle(sepX, META_BASE - 4, 1, lerpColor(COL_CARD, COL_MUTED, 6, 10), COL_CARD);
  tft.drawString(v.deviceType, sepX + 9, META_BASE);

  // .hero: big action count with the unit on the same baseline
  tft.setTextColor(COL_TEXT);
  tft.setFreeFont(&FreeSansBold24pt7b);
  char actions[12];
  snprintf(actions, sizeof(actions), "%d", v.actions);
  const int16_t heroW = tft.drawString(actions, CX, HERO_BASE);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.drawString(v.actions == 1 ? "action" : "actions", CX + heroW + 6, HERO_BASE);

  tft.setTextColor(COL_MUTED);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.drawString(fitText("New visit on " + v.siteName + ".", CR - CX), CX, CAPTION_BASE);

  // .stats: Visit started | Goals
  char timeStr[8] = "--:--";
  const time_t ts = parseIsoEpoch(v.visitStart);
  if (ts > 0) {
    struct tm tmInfo;
    const time_t now = time(nullptr);
    tmInfo = now > 100000 ? *localtime(&ts) : *gmtime(&ts);
    strftime(timeStr, sizeof(timeStr), "%H:%M", &tmInfo);
  }
  char goals[8];
  snprintf(goals, sizeof(goals), "%d", v.goals);

  const uint16_t dash = lerpColor(COL_CARD, COL_TEXT, 45, 100);
  for (int y = STATS_Y; y < LABEL_BASE + 4; y += 6) tft.drawFastVLine(DIV_X, y, 3, dash);
  drawStat(CX, DIV_X - 8 - CX, true, timeStr, "Visit started");
  drawStat(COL2_X, CR - COL2_X, false, goals, v.goals == 1 ? "Goal conversion" : "Goal conversions");
}

void drawEmpty() {
  // .empty: a shorter glass box with the waiting text centred
  drawGlass(EMPTY_H, COL_DOT);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_MUTED);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.drawString("Waiting for visits...", SCREEN_W / 2, CARD_Y + EMPTY_H / 2);
}

void drawDots() {
  if (historyLen < 2) return;
  const uint16_t bg = backgroundColorAt(DOTS_Y);
  int x = (SCREEN_W - (historyLen - 1) * DOT_STEP) / 2;
  for (int i = 0; i < historyLen; i++) {
    if (i == viewIndex) {
      // .dot.active glow
      tft.fillSmoothCircle(x, DOTS_Y, 5, lerpColor(bg, COL_ICE, 1, 2), bg);
      tft.fillSmoothCircle(x, DOTS_Y, 4, COL_TEXT, lerpColor(bg, COL_ICE, 1, 2));
    } else {
      tft.fillSmoothCircle(x, DOTS_Y, 4, lerpColor(bg, COL_TEXT, 3, 10), bg);
    }
    x += DOT_STEP;
  }
}

void render() {
  String key = hasError ? "err" : "ok";
  key += "|";
  for (int i = 0; i < historyLen; i++) key += history[i].id + ",";
  key += "|" + String(viewIndex);
  if (key == lastRenderKey) return;
  lastRenderKey = key;

  drawBackground();
  drawBubbles();
  drawHeader();
  drawStatus();
  if (historyLen == 0) {
    drawEmpty();
  } else {
    drawCard(history[viewIndex]);
  }
  drawDots();
}

// ---------------------------------------------------------------------------
time_t parseIsoEpoch(const String& s) {
  // "2026-10-22T09:04:49.000Z" -> epoch seconds (UTC)
  if (s.length() < 19) return 0;
  const int y = s.substring(0, 4).toInt();
  const int mo = s.substring(5, 7).toInt();
  const int d = s.substring(8, 10).toInt();
  const int hh = s.substring(11, 13).toInt();
  const int mm = s.substring(14, 16).toInt();
  const int ss = s.substring(17, 19).toInt();
  if (y < 2020 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  const int yy = y - (mo <= 2 ? 1 : 0);
  const long era = (yy >= 0 ? yy : yy - 399) / 400;
  const int yoe = yy - era * 400;
  const int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = era * 146097L + doe - 719468L;
  return (time_t)(days * 86400L + hh * 3600 + mm * 60 + ss);
}

// Piezo speaker on GPIO26 (bit-banged: version-proof across Arduino cores).
// Same two-tone as utils.ts playChime()
void chime() {
  auto buzz = [](int freq, int ms) {
    const uint32_t half = 500000UL / freq;
    const uint32_t end = millis() + ms;
    while (millis() < end) {
      digitalWrite(26, HIGH);
      delayMicroseconds(half);
      digitalWrite(26, LOW);
      delayMicroseconds(half);
    }
  };
  pinMode(26, OUTPUT);
  buzz(880, 120);
  buzz(1320, 280);
  digitalWrite(26, LOW);
}

bool poll() {
  HTTPClient http;
  String url = String(API_BASE) + "/api/visits?after=" + cursor_;
  http.begin(url);
  http.setTimeout(5000);
  const int code = http.GET();
  if (code != 200) {
    hasError = true;
    http.end();
    return false;
  }
  const String body = http.getString();
  http.end();
  hasError = false;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    hasError = true;
    return false;
  }

  cursor_ = doc["cursor"] | 0UL;
  for (JsonVariant item : doc["visits"].as<JsonArray>()) {
    Visit v;
    v.id = item["id"].as<String>();
    v.siteName = item["siteName"].as<String>();
    v.city = item["city"].as<String>();
    v.country = item["country"].as<String>();
    String rawDevice = item["deviceType"] | "desktop";
    v.deviceType = (rawDevice == "mobile") ? "Mobile" : "Desktop";
    v.visitStart = item["visitStart"].as<String>();
    v.actions = item["actions"] | 0;
    v.goals = item["goals"] | 0;

    bool known = false;
    for (int i = 0; i < historyLen; i++) known = known || history[i].id == v.id;
    if (known) continue;

    // history.unshift(v); drop oldest past HISTORY_MAX; jump to newest
    for (int i = min(historyLen, HISTORY_MAX - 1); i > 0; i--) history[i] = history[i - 1];
    history[0] = v;
    if (historyLen < HISTORY_MAX) historyLen++;
    viewIndex = 0;
    chime();
  }
  return true;
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  drawBackground();
  drawHeader();
  tft.setTextColor(COL_TEXT);
  tft.setFreeFont(&FreeSans12pt7b);
  tft.setTextDatum(L_BASELINE);
  const int16_t textW = tft.drawString("Connecting to WiFi", CARD_X, 130);
  const uint32_t start = millis();
  tft.setCursor(CARD_X + textW + 2, 130);
  tft.setTextColor(COL_MUTED);
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    tft.print('.');
    delay(400);
  }
  if (WiFi.status() != WL_CONNECTED) {
    tft.setTextColor(COL_RED);
    tft.drawString("WiFi failed - rebooting", CARD_X, 160);
    delay(3000);
    ESP.restart();
  }
  // Local time for the "Visit started" column
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setSwapBytes(true);
  tft.setRotation(1);  // landscape 320x240
  tft.fillScreen(COL_WATER_BOT);
  touchInit();

  connectWifi();
  render();
  poll();
  lastPoll = millis();
}

void loop() {
  // Tap anywhere to step back through history (frontend: clicking the card);
  // wraps around to the newest notification at the end
  if (screenTouched()) {
    unsigned long start = millis();
    while (screenTouched() && millis() - start < 1500) delay(20);
    if (historyLen > 0) {
      viewIndex = (viewIndex + 1) % historyLen;
      render();
    }
  }

  const unsigned long now = millis();
  if (now - lastPoll >= POLL_MS) {
    lastPoll = now;
    if (WiFi.status() != WL_CONNECTED) {
      connectWifi();
      lastRenderKey = "";
    }
    poll();
    drawStatus();
    render();
  }
  delay(50);
}

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <driver/dac_common.h>
#include <TFT_eSPI.h>
#include "secrets.h"
#include "letsencrypt_roots.h"

// FreeSans with U+0020-U+017F (åäö etc.) instead of TFT_eSPI's 7-bit ASCII
// versions; regenerate with tools/make-fonts.sh
#include "fonts/FreeSans6pt8b.h"
#include "fonts/FreeSans9pt8b.h"
#include "fonts/FreeSans12pt8b.h"
#include "fonts/FreeSansBold12pt8b.h"

time_t parseIsoEpoch(const String& s);

// POSIX TZ rule for Sweden: CET (UTC+1), CEST (UTC+2) from the last Sunday
// of March 02:00 to the last Sunday of October 03:00
#define TZ_SWEDEN "CET-1CEST,M3.5.0,M10.5.0/3"

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
  String host;
  String city;
  String country;
  String countryCode;
  String deviceType;
  String visitStart;
  String lastPageTitle;
  int actions = 0;
  int goals = 0;
};

Visit history[HISTORY_MAX];
int historyLen = 0;
int viewIndex = 0;
unsigned long cursor_ = 0;
bool hasError = false;
bool muted = false;  // long press toggles; kept in NVS across reboots
int imageGen = 0;  // bumps when a favicon/flag arrives, so render() redraws
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
constexpr int16_t CARD_W = SCREEN_W - 2 * CARD_X;
constexpr int16_t EMPTY_H = 96;
constexpr int16_t DOT_STEP = 12;

// Card content box
constexpr int16_t PAD = 12;
constexpr int16_t CX = CARD_X + PAD;
constexpr int16_t CR = CARD_X + CARD_W - PAD;
// Vertical rhythm, as offsets from the top of the card:
// .card-head: favicon + title block (site name / meta line)
constexpr int16_t FAV_DY = 10;
constexpr int16_t FAV_S = 34;
// .hero + .hero-caption: action count and unit in one size
constexpr int16_t HERO_DY = FAV_DY + FAV_S + 12 + 17;
constexpr int16_t CAPTION_DY = HERO_DY + 18;
// .stats: value and label per column (no icons)
constexpr int16_t STATS_DY = CAPTION_DY + 5 + 12;  // top of the value caps
constexpr int16_t LABEL_DY = STATS_DY + 17 + 17;
// Card height follows the content (label descenders + bottom padding)
constexpr int16_t CARD_H = LABEL_DY + 5 + PAD;

// Card + history dots centred vertically in the space below the header;
// the empty state box is centred on its own
constexpr int16_t DOTS_GAP = 7;
constexpr int16_t DOT_R = 5;
constexpr int16_t CARD_Y = HEADER_H + (SCREEN_H - HEADER_H - (CARD_H + DOTS_GAP + DOT_R)) / 2;
constexpr int16_t EMPTY_Y = HEADER_H + (SCREEN_H - HEADER_H - EMPTY_H) / 2;
constexpr int16_t DOTS_Y = CARD_Y + CARD_H + DOTS_GAP;

constexpr int16_t FAV_Y = CARD_Y + FAV_DY;
constexpr int16_t TITLE_X = CX + FAV_S + 10;
constexpr int16_t SITE_BASE = FAV_Y + 17;
constexpr int16_t META_BASE = FAV_Y + 33;
constexpr int16_t HERO_BASE = CARD_Y + HERO_DY;
constexpr int16_t CAPTION_BASE = CARD_Y + CAPTION_DY;
constexpr int16_t STATS_Y = CARD_Y + STATS_DY;
constexpr int16_t VALUE_BASE = STATS_Y + 17;
constexpr int16_t LABEL_BASE = CARD_Y + LABEL_DY;
// Columns are not 1fr/1fr: "Goal conversions" needs ~139px in FreeSans9pt
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

// The fonts stop at U+017F; map common typographic punctuation outside that
// range (as used in site names) to ASCII so it isn't silently dropped
String cleanText(String s) {
  static const char* const map[][2] = {
    { "\u2018", "'" }, { "\u2019", "'" }, { "\u201C", "\"" }, { "\u201D", "\"" },
    { "\u2013", "-" }, { "\u2014", "-" }, { "\u2026", "..." }, { "\u2022", "-" },
  };
  for (const auto& m : map) s.replace(m[0], m[1]);
  return s;
}

// First character of s, upper-cased: ASCII, Latin-1 (å -> Å) and
// Latin Extended-A (ł -> Ł), returned as UTF-8
String initial(const String& s) {
  if (s.isEmpty()) return "?";
  const uint8_t* p = (const uint8_t*)s.c_str();
  uint16_t c = p[0];
  if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) c = ((c & 0x1F) << 6) | (p[1] & 0x3F);
  else if (c >= 0x80) return "?";
  if (c > 0x17F) return "?";  // not in the fonts

  if (c >= 'a' && c <= 'z') c -= 0x20;
  else if (c >= 0xE0 && c <= 0xFE && c != 0xF7) c -= 0x20;
  else if (c == 0xFF) c = 0x178;
  else if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) c &= ~1;
  else if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) && !(c & 1)) c -= 1;

  char out[3] = { (char)c, 0, 0 };
  if (c >= 0x80) {
    out[0] = (char)(0xC0 | (c >> 6));
    out[1] = (char)(0x80 | (c & 0x3F));
  }
  return out;
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
  tft.setFreeFont(&FreeSans9pt8b);
  tft.drawString("MATOMO NOTIFIER", CARD_X, HEADER_BASE);
}

// #sound-toggle: speaker with sound waves, or with an X when muted.
// 12x12 box ending at the card's right edge, centred on the title's caps.
constexpr int16_t SPEAKER_X = CARD_X + CARD_W - 12;
constexpr int16_t SPEAKER_CY = HEADER_BASE - 6;

void drawSpeaker() {
  const int16_t x = SPEAKER_X, cy = SPEAKER_CY;
  const uint16_t bg = backgroundColorAt(cy);
  tft.fillRect(x, cy - 2, 3, 5, COL_MUTED);
  tft.fillTriangle(x + 2, cy - 2, x + 6, cy - 6, x + 6, cy + 6, COL_MUTED);
  tft.fillTriangle(x + 2, cy - 2, x + 2, cy + 2, x + 6, cy + 6, COL_MUTED);
  if (muted) {
    tft.drawWideLine(x + 8, cy - 3, x + 12, cy + 3, 1.5, COL_RED, bg);
    tft.drawWideLine(x + 8, cy + 3, x + 12, cy - 3, 1.5, COL_RED, bg);
  } else {
    // TFT_eSPI arc angles: 0 = 6 o'clock, clockwise, so 225-315 faces right
    tft.drawArc(x + 6, cy, 4, 3, 225, 315, COL_MUTED, bg);
    tft.drawArc(x + 6, cy, 7, 6, 225, 315, COL_MUTED, bg);
  }
}

void drawStatus() {
  // .status is the smallest text on the page (0.8rem vs the 1.6rem title);
  // at 6pt "backend unreachable" still clears the title with the speaker
  // icon beside it. Repaint only the right-hand part of the header.
  constexpr int16_t STATUS_X = 181;
  drawBackground(STATUS_X, 0, HEADER_H);
  tft.setTextDatum(R_BASELINE);
  tft.setTextColor(hasError ? COL_RED : COL_MUTED);
  tft.setFreeFont(&FreeSans6pt8b);
  // Baseline chosen so its caps are vertically centred on the title's
  tft.drawString(hasError ? "backend unreachable" : "connected", SPEAKER_X - 4, HEADER_BASE - 2);
  drawSpeaker();
}

// ---------------------------------------------------------------------------
// Card (frontend: .card / .card-head / .hero / .stats)
// ---------------------------------------------------------------------------
void drawGlass(int16_t y, int16_t h, uint16_t fill) {
  tft.fillSmoothRoundRect(CARD_X, y, CARD_W, h, 12, COL_BORDER, backgroundColorAt(y + h / 2));
  tft.fillSmoothRoundRect(CARD_X + 2, y + 2, CARD_W - 4, h - 4, 10, fill, COL_BORDER);
  tft.drawFastHLine(CARD_X + 12, y + 2, CARD_W - 24, COL_HIGHLIGHT);
}

// ---------------------------------------------------------------------------
// Backend API over HTTP or HTTPS, depending on API_BASE. One persistent
// connection, so keep-alive reuses the TLS session across polls instead of a
// full handshake (~2.4 s) every 2 s. The HTTPClient is global too: its
// destructor stop()s the connection, so a local one per request would close
// it every time.
// ---------------------------------------------------------------------------
WiFiClient plainClient;
WiFiClientSecure tlsClient;
HTTPClient api;
const bool apiHttps = String(API_BASE).startsWith("https://");

WiFiClient& apiClient() {
  return apiHttps ? tlsClient : plainClient;
}

void apiInit() {
  // Verify the server against the Let's Encrypt roots. mbedTLS here doesn't
  // check validity dates, so this works before NTP has synced.
  tlsClient.setCACert(LETSENCRYPT_ROOTS);
  tlsClient.setHandshakeTimeout(10);  // seconds; the default is 120
}

// GET API_BASE + path on `api`. Retries once on a transport error, e.g. a
// kept-alive connection the server has closed since the last request.
// Returns the HTTP status (or < 0); the caller reads the body from `api` and
// calls api.end(), which keeps the connection open for the next request.
int apiGet(const String& path, uint16_t timeoutMs) {
  int code = -1;
  for (int attempt = 0; attempt < 2 && code < 0; attempt++) {
    if (attempt) {
      api.end();
      apiClient().stop();
    }
    api.begin(apiClient(), String(API_BASE) + path);
    api.setTimeout(timeoutMs);
    code = api.GET();
  }
  if (code != 200) {
    Serial.printf("[api] GET %s -> %d %s\n", path.c_str(), code,
                  code < 0 ? HTTPClient::errorToString(code).c_str() : "");
  }
  return code;
}

// ---------------------------------------------------------------------------
// Images: the backend converts favicons and country flags to raw RGB565
// (GET /api/favicon.rgb565, /api/flag.rgb565), cached here per host/country
// code. One slot per history entry is always enough. Little-endian uint16s
// land in px[] as-is; pushImage() swaps them for the panel (setSwapBytes).
// ---------------------------------------------------------------------------
constexpr unsigned long IMAGE_RETRY_MS = 10UL * 60 * 1000;

bool fetchImage(const String& path, uint16_t* px, int bytes) {
  // 8 s: a new favicon makes the backend fetch it first
  const int code = apiGet(path, 8000);
  bool ok = code == 200 && api.getSize() == bytes &&
            api.getStreamPtr()->readBytes((uint8_t*)px, bytes) == bytes;
  api.end();
  return ok;
}

template <int W, int H>
struct ImageCache {
  struct Slot {
    String key;
    bool ok = false;
    unsigned long fetchedAt = 0;
    uint16_t px[W * H];
  };
  Slot slots[HISTORY_MAX];
  String Visit::*field;              // which visit field is the key
  String (*path)(const String& key);  // backend API path for a key

  ImageCache(String Visit::*field, String (*path)(const String&)) : field(field), path(path) {}

  const uint16_t* get(const String& key) const {
    for (const auto& s : slots) {
      if (s.ok && s.key.length() && s.key == key) return s.px;
    }
    return nullptr;
  }

  // Fetch images for history entries that don't have one (or whose last
  // attempt failed long enough ago). Returns true if any arrived.
  bool update() {
    bool arrived = false;
    for (int i = 0; i < historyLen; i++) {
      const String& key = history[i].*field;
      if (key.isEmpty()) continue;
      Slot* slot = find(key);
      if (slot && (slot->ok || millis() - slot->fetchedAt < IMAGE_RETRY_MS)) continue;
      if (!slot) {
        for (auto& s : slots) {
          if (s.key.isEmpty() || !inHistory(s.key)) {
            slot = &s;
            break;
          }
        }
        if (!slot) return arrived;
        slot->key = key;
      }
      slot->ok = fetchImage(path(key), slot->px, W * H * 2);
      slot->fetchedAt = millis();
      arrived = arrived || slot->ok;
    }
    return arrived;
  }

 private:
  Slot* find(const String& key) {
    for (auto& s : slots) {
      if (s.key.length() && s.key == key) return &s;
    }
    return nullptr;
  }

  bool inHistory(const String& key) const {
    for (int i = 0; i < historyLen; i++) {
      if (history[i].*field == key) return true;
    }
    return false;
  }
};

constexpr int16_t FAV_IMG = 28;  // .favicon: 40px tile with 4px padding, scaled
constexpr int16_t FLAG_W = 18;   // 3:2, as tall as the meta line's caps
constexpr int16_t FLAG_H = 12;

ImageCache<FAV_IMG, FAV_IMG> favicons(&Visit::host, [](const String& host) -> String {
  return String("/api/favicon.rgb565?size=") + FAV_IMG + "&host=" + host;
});
ImageCache<FLAG_W, FLAG_H> flags(&Visit::countryCode, [](const String& cc) -> String {
  return String("/api/flag.rgb565?w=") + FLAG_W + "&h=" + FLAG_H + "&cc=" + cc;
});

void updateImages() {
  const bool favs = favicons.update();
  if (flags.update() || favs) imageGen++;
}

void drawFavicon(const Visit& v) {
  // White tile like the web's .favicon; the bitmap if we have one, otherwise
  // the letter placeholder (utils.ts placeholder(): #4f8cff site initial)
  tft.fillSmoothRoundRect(CX, FAV_Y, FAV_S, FAV_S, 8, COL_TEXT, COL_CARD);
  if (const uint16_t* px = favicons.get(v.host)) {
    tft.pushImage(CX + (FAV_S - FAV_IMG) / 2, FAV_Y + (FAV_S - FAV_IMG) / 2, FAV_IMG, FAV_IMG, px);
    return;
  }
  // Centre the cap height (17px): MC_DATUM would centre on the tallest glyph,
  // which with accented capitals (Å) sits a few pixels too low
  tft.setTextColor(COL_LETTER);
  tft.setTextDatum(C_BASELINE);
  tft.setFreeFont(&FreeSansBold12pt8b);
  tft.drawString(initial(v.siteName), CX + FAV_S / 2, FAV_Y + (FAV_S + 17) / 2);
}

void drawStat(int16_t x, int16_t maxW, const char* value, const char* label) {
  tft.setTextDatum(L_BASELINE);
  tft.setTextColor(COL_TEXT);
  tft.setFreeFont(&FreeSansBold12pt8b);
  tft.drawString(value, x, VALUE_BASE);
  tft.setTextColor(COL_MUTED);
  tft.setFreeFont(&FreeSans9pt8b);
  tft.drawString(fitText(label, maxW), x, LABEL_BASE);
}

void drawCard(const Visit& v) {
  drawGlass(CARD_Y, CARD_H, COL_CARD);
  drawFavicon(v);

  // Text is drawn transparently: render() repaints the whole card, and an
  // opaque text box would clip neighbouring lines
  tft.setTextDatum(L_BASELINE);
  tft.setTextColor(COL_TEXT);
  tft.setFreeFont(&FreeSansBold12pt8b);
  tft.drawString(fitText(v.siteName, CR - TITLE_X), TITLE_X, SITE_BASE);

  // .meta: "<flag> <city> · <device>"; the flag stands in for the country
  // name (the web shows it as the flag's tooltip), which keeps the line from
  // truncating. Without a flag: "<city, country>". The flag spans the caps.
  const uint16_t* flag = flags.get(v.countryCode);
  int16_t metaX = TITLE_X;
  if (flag) {
    tft.pushImage(TITLE_X, META_BASE - FLAG_H, FLAG_W, FLAG_H, flag);
    metaX += FLAG_W + 7;
  }
  tft.setTextColor(COL_MUTED);
  tft.setFreeFont(&FreeSans9pt8b);
  String location = v.city.length() && flag ? v.city
                    : v.city.length() && v.country.length() ? v.city + ", " + v.country
                    : v.city.length() ? v.city
                    : v.country.length() ? v.country
                    : String("Unknown location");
  const int16_t deviceW = tft.textWidth(v.deviceType);
  const int16_t locW = tft.drawString(fitText(location, CR - metaX - deviceW - 18), metaX, META_BASE);
  const int16_t sepX = metaX + locW + 8;
  tft.fillSmoothCircle(sepX, META_BASE - 4, 1, lerpColor(COL_CARD, COL_MUTED, 6, 10), COL_CARD);
  tft.drawString(v.deviceType, sepX + 9, META_BASE);

  // .hero: action count and unit, same size
  tft.setTextColor(COL_TEXT);
  tft.setFreeFont(&FreeSansBold12pt8b);
  char actions[24];
  snprintf(actions, sizeof(actions), "%d %s", v.actions, v.actions == 1 ? "action" : "actions");
  tft.drawString(actions, CX, HERO_BASE);

  tft.setTextColor(COL_MUTED);
  tft.setFreeFont(&FreeSans9pt8b);
  // Title of the latest page visited, as much as fits
  const String caption = v.lastPageTitle.length() ? v.lastPageTitle : "New visit on " + v.siteName + ".";
  tft.drawString(fitText(caption, CR - CX), CX, CAPTION_BASE);

  // .stats: Visit started | Goals
  char timeStr[8] = "--:--";
  const time_t ts = parseIsoEpoch(v.visitStart);
  if (ts > 0) {
    // ts is absolute (UTC epoch), so this needs only TZ, not a synced clock
    struct tm tmInfo;
    localtime_r(&ts, &tmInfo);
    strftime(timeStr, sizeof(timeStr), "%H:%M", &tmInfo);
  }
  char goals[8];
  snprintf(goals, sizeof(goals), "%d", v.goals);

  const uint16_t dash = lerpColor(COL_CARD, COL_TEXT, 45, 100);
  for (int y = STATS_Y; y < LABEL_BASE + 4; y += 6) tft.drawFastVLine(DIV_X, y, 3, dash);
  drawStat(CX, DIV_X - 8 - CX, timeStr, "Visit started");
  drawStat(COL2_X, CR - COL2_X, goals, v.goals == 1 ? "Goal conversion" : "Goal conversions");
}

void drawEmpty() {
  // .empty: a shorter glass box with the waiting text centred
  drawGlass(EMPTY_Y, EMPTY_H, COL_DOT);
  tft.setTextDatum(C_BASELINE);
  tft.setTextColor(COL_MUTED);
  tft.setFreeFont(&FreeSans9pt8b);
  tft.drawString("Waiting for visits...", SCREEN_W / 2, EMPTY_Y + (EMPTY_H + 12) / 2);
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
  for (int i = 0; i < historyLen; i++) {
    key += history[i].id + ":" + history[i].actions + ":" + history[i].goals + ",";
  }
  key += "|" + String(viewIndex) + "|" + String(imageGen);
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

// ---------------------------------------------------------------------------
// Speaker connector: SC8002B amplifier fed from the DAC on GPIO26, with its
// SHUTDOWN pin on GPIO4 (low = amp on). Per Freenove's FNK0114B schematic.
// ---------------------------------------------------------------------------
constexpr int AUDIO_EN = 4;
constexpr dac_channel_t AUDIO_DAC = DAC_CHANNEL_2;  // GPIO26
constexpr uint8_t DAC_MID = 128;
// Peak amplitude in DAC steps (max 127); lower it if the speaker is too loud
constexpr float CHIME_VOLUME = 30;
constexpr uint32_t SAMPLE_RATE = 16000;

void audioInit() {
  // Enable the DAC once. Arduino's dacWrite() re-runs dac_output_enable()
  // (RTC pad init, power-on) on every call, i.e. on every sample.
  dac_output_enable(AUDIO_DAC);
  dac_output_voltage(AUDIO_DAC, DAC_MID);  // park at the midpoint before the amp wakes
  pinMode(AUDIO_EN, OUTPUT);
  digitalWrite(AUDIO_EN, LOW);
}

// utils.ts playChime(): sine at 880 Hz, 1320 Hz from 120 ms, gain ramping
// exponentially down to 1/150 over 400 ms. Synthesised sample by sample on
// the DAC; blocks for the 400 ms, like the old bit-banged beep did.
void chime() {
  constexpr float TWO_PI_F = 6.2831853f;
  constexpr uint32_t total = SAMPLE_RATE * 400 / 1000;
  constexpr uint32_t step = SAMPLE_RATE * 120 / 1000;
  const float decay = powf(1.0f / 150.0f, 1.0f / total);
  float phase = 0, gain = CHIME_VOLUME;
  const uint32_t started = micros();
  for (uint32_t i = 0; i < total; i++) {
    const float freq = i < step ? 880.0f : 1320.0f;
    phase += TWO_PI_F * freq / SAMPLE_RATE;
    if (phase >= TWO_PI_F) phase -= TWO_PI_F;
    const float attack = i < 32 ? i / 32.0f : 1.0f;  // 2 ms fade-in, no pop
    dac_output_voltage(AUDIO_DAC, (uint8_t)lroundf(DAC_MID + attack * gain * sinf(phase)));
    gain *= decay;
    // Exact 62.5 us spacing (1000000 / 16000 isn't an integer)
    const uint32_t next = started + (uint32_t)((i + 1) * 1000000ULL / SAMPLE_RATE);
    while ((int32_t)(micros() - next) < 0) {}
  }
  dac_output_voltage(AUDIO_DAC, DAC_MID);
}

// Returns the number of new visits, or -1 on error
int poll() {
  static bool first = true;
  const uint32_t t0 = millis();
  const int code = apiGet(String("/api/visits?after=") + cursor_ + "&limit=" + HISTORY_MAX, 5000);
  if (first) {
    first = false;
    Serial.printf("[api] first poll %s: %d in %lu ms, free heap %u\n", API_BASE, code,
                  (unsigned long)(millis() - t0), ESP.getFreeHeap());
  }
  if (code != 200) {
    hasError = true;
    api.end();
    return -1;
  }
  const String body = api.getString();
  api.end();
  hasError = false;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    hasError = true;
    return -1;
  }

  cursor_ = doc["cursor"] | 0UL;
  int fresh = 0;
  for (JsonVariant item : doc["visits"].as<JsonArray>()) {
    Visit v;
    v.id = item["id"].as<String>();
    v.siteName = cleanText(item["siteName"] | "");
    v.host = item["host"] | "";
    v.city = cleanText(item["city"] | "");
    v.country = cleanText(item["country"] | "");
    v.countryCode = item["countryCode"] | "";
    String rawDevice = item["deviceType"] | "desktop";
    v.deviceType = (rawDevice == "mobile") ? "Mobile" : "Desktop";
    v.visitStart = item["visitStart"].as<String>();
    v.lastPageTitle = cleanText(item["lastPageTitle"] | "");
    v.actions = item["actions"] | 0;
    v.goals = item["goals"] | 0;

    // Already shown: an update (Matomo keeps counting actions while the
    // visitor browses). Replace it in place, no chime, no jump.
    int known = -1;
    for (int i = 0; i < historyLen && known < 0; i++) {
      if (history[i].id == v.id) known = i;
    }
    if (known >= 0) {
      history[known] = v;
      continue;
    }

    // history.unshift(v); drop oldest past HISTORY_MAX; jump to newest
    for (int i = min(historyLen, HISTORY_MAX - 1); i > 0; i--) history[i] = history[i - 1];
    history[0] = v;
    if (historyLen < HISTORY_MAX) historyLen++;
    viewIndex = 0;
    fresh++;
  }
  return fresh;
}

constexpr unsigned long LONG_PRESS_MS = 800;
Preferences prefs;

void toggleMute() {
  muted = !muted;
  prefs.putBool("muted", muted);
  drawStatus();
  if (!muted) chime();  // confirms the speaker works again
}

// Poll, show the new card, then play the chime (once, however many visits
// arrived), then fetch any missing favicons, which can take a moment
void refresh() {
  // The first successful poll loads visits detected before boot: show them,
  // but don't chime for them
  static bool primed = false;
  const int fresh = poll();
  drawStatus();
  render();
  if (fresh > 0 && primed && !muted) chime();
  if (fresh >= 0) primed = true;
  updateImages();
  render();
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  drawBackground();
  drawHeader();
  tft.setTextColor(COL_TEXT);
  tft.setFreeFont(&FreeSans12pt8b);
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
  // Swedish time (CET/CEST) for the "Visit started" column. Not configTime():
  // that overwrites TZ with a fixed offset ("UTC0" for 0, 0)
  configTzTime(TZ_SWEDEN, "pool.ntp.org", "time.nist.gov");
}

void setup() {
  Serial.begin(115200);
  setenv("TZ", TZ_SWEDEN, 1);
  tzset();
  tft.init();
  tft.setSwapBytes(true);
  tft.setRotation(1);  // landscape 320x240
  tft.fillScreen(COL_WATER_BOT);
  touchInit();
  audioInit();
  apiInit();
  prefs.begin("notifier");
  muted = prefs.getBool("muted", false);

  connectWifi();
  render();
  refresh();
  lastPoll = millis();
}

void loop() {
  // Tap anywhere to step back through history (frontend: clicking the card),
  // wrapping around to the newest at the end. Long press toggles mute
  // (frontend: the sound toggle button).
  if (screenTouched()) {
    const unsigned long start = millis();
    bool longPress = false;
    int released = 0;  // consecutive untouched reads, to ride out glitches
    while (released < 3 && millis() - start < 10000) {
      released = screenTouched() ? 0 : released + 1;
      if (!longPress && millis() - start >= LONG_PRESS_MS) {
        longPress = true;
        toggleMute();  // immediately, so the icon flips while still held
      }
      delay(20);
    }
    if (!longPress && historyLen > 0) {
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
    refresh();
  }
  delay(50);
}

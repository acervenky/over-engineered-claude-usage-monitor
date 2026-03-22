/*
 * Claude Usage Monitor — M5Stack ATOMS3R (ESP32-S3)
 * Display: GC9A01 0.85" round IPS LCD, 128x128, color
 * Self-contained unit — no external wiring needed.
 *
 * Credentials pushed from Chrome extension — no manual config portal needed.
 * Hold built-in button 2s to reset WiFi only.
 *
 * Dependencies (PlatformIO):
 *   - M5Unified       (display, button, hardware abstraction)
 *   - ArduinoJson v6  (JSON parsing)
 *   - WiFiManager     (WiFi setup only)
 *   - WebServer       (built-in with ESP32 core)
 *   - HTTPClient      (built-in)
 */

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

// ╔════════════════════════════════════════════════════════════════════════╗
// ║  USER CONFIG — edit these to match your setup                        ║
// ╚════════════════════════════════════════════════════════════════════════╝

// WiFi access point name and password for first-boot setup.
// Leave AP_PASS as nullptr for an open network (no password).
#define AP_NAME  "ClaudeMonitor"
#define AP_PASS  nullptr              // e.g. "mypassword" for WPA2

// Timezone offset from UTC in seconds. Used for NTP clock and countdown
// timers. Both configTime() and parseResetsAt() depend on this value.
// Pick yours from the list below and replace the #define.
//
//   UTC-12:00  Baker Island          (-12 * 3600)
//   UTC-11:00  American Samoa        (-11 * 3600)
//   UTC-10:00  HST   Hawaii          (-10 * 3600)
//   UTC-09:00  AKST  Alaska          (-9 * 3600)
//   UTC-08:00  PST   US Pacific      (-8 * 3600)
//   UTC-07:00  MST   US Mountain     (-7 * 3600)
//   UTC-06:00  CST   US Central      (-6 * 3600)
//   UTC-05:00  EST   US Eastern      (-5 * 3600)
//   UTC-04:00  AST   Atlantic        (-4 * 3600)
//   UTC-03:30  NST   Newfoundland    (-3 * 3600 - 30 * 60)
//   UTC-03:00  BRT   Brazil          (-3 * 3600)
//   UTC+00:00  GMT/UTC               (0)
//   UTC+01:00  CET   Central Europe  (1 * 3600)
//   UTC+02:00  EET   Eastern Europe  (2 * 3600)
//   UTC+03:00  MSK   Moscow          (3 * 3600)
//   UTC+03:30  IRST  Iran            (3 * 3600 + 30 * 60)
//   UTC+04:00  GST   Gulf            (4 * 3600)
//   UTC+05:00  PKT   Pakistan        (5 * 3600)
//   UTC+05:30  IST   India           (5 * 3600 + 30 * 60)
//   UTC+05:45  NPT   Nepal           (5 * 3600 + 45 * 60)
//   UTC+06:00  BST   Bangladesh      (6 * 3600)
//   UTC+07:00  ICT   Indochina       (7 * 3600)
//   UTC+08:00  CST   China/SGT       (8 * 3600)
//   UTC+09:00  JST   Japan/KST       (9 * 3600)
//   UTC+09:30  ACST  Aus Central     (9 * 3600 + 30 * 60)
//   UTC+10:00  AEST  Aus Eastern     (10 * 3600)
//   UTC+12:00  NZST  New Zealand     (12 * 3600)
//
// Note: standard time only, no automatic DST switching.
#define TZ_OFFSET_SEC  (5 * 3600 + 30 * 60)  // IST (UTC+5:30)

// How often to poll claude.ai for usage data (milliseconds)
#define POLL_INTERVAL_MS  (5UL * 60 * 1000)   // 5 minutes

// NTP server for time sync
#define NTP_SERVER  "pool.ntp.org"

// ── Internal config (no need to edit below this line) ────────────────────
#define HTTP_PORT  80

// ── Display ──────────────────────────────────────────────────────────────
M5Canvas canvas(&M5.Display);

// ── Colors (RGB565) ──────────────────────────────────────────────────────
#define CLR_BG        0x0000
#define CLR_TEXT      0xFFFF
#define CLR_DIM       0x7BEF
#define CLR_LINE      0x4208
#define CLR_GREEN     0x07E0
#define CLR_YELLOW    0xFFE0
#define CLR_RED       0xF800
#define CLR_BAR_BG    0x2104

// ── State ────────────────────────────────────────────────────────────────
struct UsageData {
  float fiveHour     = -1;
  float sevenDay     = -1;
  long  fiveHourSecs = -1;
  long  sevenDaySecs = -1;
  unsigned long fetchedAt = 0;
  bool  valid = false;
};

char      gOrgUUID[40]     = {0};
char      gSessionKey[460] = {0};
UsageData gUsage;
bool      gConfigured      = false; // cached NVS state, updated on save/load
unsigned long gLastPoll    = 0;

Preferences prefs;
WebServer server(HTTP_PORT);

// ── Credential helpers (NVS) ─────────────────────────────────────────────
void saveCredentials(const char* uuid, const char* key) {
  prefs.begin("claude", false);
  prefs.putString("orgUUID", uuid);
  prefs.putString("sessKey", key);
  prefs.putBool("configured", true);
  prefs.end();
  strncpy(gOrgUUID, uuid, sizeof(gOrgUUID) - 1);
  strncpy(gSessionKey, key, sizeof(gSessionKey) - 1);
  gConfigured = true;
}

void loadCredentials() {
  prefs.begin("claude", true);
  String uuid = prefs.getString("orgUUID", "");
  String key  = prefs.getString("sessKey", "");
  gConfigured = prefs.getBool("configured", false);
  strncpy(gOrgUUID, uuid.c_str(), sizeof(gOrgUUID) - 1);
  strncpy(gSessionKey, key.c_str(), sizeof(gSessionKey) - 1);
  prefs.end();
}

// ── CORS ─────────────────────────────────────────────────────────────────
void addCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

// ── Display helpers ──────────────────────────────────────────────────────

uint16_t barColor(float pct) {
  if (pct >= 80.0f) return CLR_RED;
  if (pct >= 50.0f) return CLR_YELLOW;
  return CLR_GREEN;
}

void drawBar(int x, int y, int w, int h, float pct) {
  canvas.fillRoundRect(x, y, w, h, 2, CLR_BAR_BG);
  int fill = constrain((int)((pct / 100.0f) * w), 0, w);
  if (fill > 0) {
    canvas.fillRoundRect(x, y, fill, h, 2, barColor(pct));
  }
}

void formatCountdown(long secs, char* buf, int bufLen) {
  if (secs <= 0) { snprintf(buf, bufLen, "ready"); return; }
  long d = secs / 86400, h = (secs % 86400) / 3600, m = (secs % 3600) / 60;
  if (d > 0)      snprintf(buf, bufLen, "resets %ldd %ldh", d, h);
  else if (h > 0) snprintf(buf, bufLen, "resets %ldh %ldm", h, m);
  else            snprintf(buf, bufLen, "resets %ldm", m);
}

void getTimeStr(char* buf, int bufLen) {
  time_t now = time(nullptr);
  struct tm* ti = localtime(&now);
  if (ti->tm_year < 120) { snprintf(buf, bufLen, "--:--"); return; }
  int h = ti->tm_hour;
  const char* ampm = h >= 12 ? "p" : "a";
  if (h == 0) h = 12; else if (h > 12) h -= 12;
  snprintf(buf, bufLen, "%d:%02d%s", h, ti->tm_min, ampm);
}

void drawCentered(int y, const char* text) {
  int w = canvas.textWidth(text);
  canvas.drawString(text, (128 - w) / 2, y);
}

// Single-screen status messages (boot splash, wifi status, errors)
void showStatus(uint16_t color, const char* line1, const char* line2 = nullptr) {
  canvas.fillSprite(CLR_BG);
  canvas.setTextColor(color);
  drawCentered(line2 ? 48 : 56, line1);
  if (line2) drawCentered(62, line2);
  canvas.pushSprite(0, 0);
}

// Draw one usage block (5-hour or 7-day) within the round display safe zone
void drawUsageBlock(int y, int L, int R, int W,
                    const char* label, float pct, long resetSecs, long elapsed) {
  char pctBuf[10], cdBuf[24];
  snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", pct);
  formatCountdown(resetSecs - elapsed, cdBuf, sizeof(cdBuf));

  canvas.setTextColor(CLR_TEXT);
  canvas.drawString(label, L, y);
  int pw = canvas.textWidth(pctBuf);
  canvas.drawString(pctBuf, R - pw, y);
  drawBar(L, y + 11, W, 8, pct);
  canvas.setTextColor(CLR_DIM);
  canvas.drawString(cdBuf, L, y + 22);
}

// ── Main display ─────────────────────────────────────────────────────────
// Layout tuned for round 128x128 GC9A01 display.
// Content stays within ~x:[20,107] y:[18,107] to avoid circular clipping.
void drawScreen() {
  canvas.fillSprite(CLR_BG);

  const int L = 20;
  const int R = 107;
  const int W = R - L;  // 87px usable

  // Header
  canvas.setTextColor(CLR_TEXT);
  canvas.setTextSize(1);
  canvas.setFont(&fonts::Font0);
  canvas.drawString("CLAUDE", L, 18);
  char timeBuf[10];
  getTimeStr(timeBuf, sizeof(timeBuf));
  canvas.drawString(timeBuf, R - canvas.textWidth(timeBuf), 18);
  canvas.drawFastHLine(L, 28, W, CLR_LINE);

  if (!gUsage.valid) {
    canvas.setTextColor(CLR_DIM);
    if (WiFi.status() != WL_CONNECTED) {
      drawCentered(56, "no wifi...");
    } else if (!gConfigured) {
      drawCentered(40, "waiting for");
      drawCentered(52, "config push");
      canvas.setTextColor(CLR_TEXT);
      drawCentered(68, WiFi.localIP().toString().c_str());
      canvas.setTextColor(CLR_DIM);
      drawCentered(82, "extension > IoT");
    } else {
      drawCentered(56, "fetching...");
    }
    canvas.pushSprite(0, 0);
    return;
  }

  // Elapsed since last fetch, handles millis() overflow
  unsigned long now = millis();
  long elapsed = (long)((now >= gUsage.fetchedAt)
    ? (now - gUsage.fetchedAt) / 1000
    : (0xFFFFFFFFUL - gUsage.fetchedAt + now) / 1000);

  drawUsageBlock(32, L, R, W, "5HR",  gUsage.fiveHour, gUsage.fiveHourSecs, elapsed);
  canvas.drawFastHLine(L, 65, W, CLR_LINE);
  drawUsageBlock(68, L, R, W, "7DAY", gUsage.sevenDay, gUsage.sevenDaySecs, elapsed);

  // Footer
  canvas.setTextColor(CLR_DIM);
  char syncBuf[24];
  if (elapsed < 60)        snprintf(syncBuf, sizeof(syncBuf), "synced just now");
  else if (elapsed < 3600) snprintf(syncBuf, sizeof(syncBuf), "synced %ldm ago", elapsed / 60);
  else                     snprintf(syncBuf, sizeof(syncBuf), "synced %ldh ago", elapsed / 3600);
  drawCentered(98, syncBuf);

  canvas.pushSprite(0, 0);
}

// ── Parse ISO8601 to seconds from now ────────────────────────────────────
// Assumes timestamps from the API are UTC. mktime() interprets as local
// (IST per configTime), so we add TZ_OFFSET_SEC to compensate.
long parseResetsAt(const char* iso) {
  struct tm t = {};
  sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2d",
    &t.tm_year, &t.tm_mon, &t.tm_mday,
    &t.tm_hour, &t.tm_min, &t.tm_sec);
  t.tm_year -= 1900; t.tm_mon -= 1;
  time_t resetUtc = mktime(&t) + TZ_OFFSET_SEC;
  return (long)(resetUtc - time(nullptr));
}

// ── Poll claude.ai ───────────────────────────────────────────────────────
void pollUsage() {
  if (gOrgUUID[0] == '\0' || gSessionKey[0] == '\0') return;

  char url[128];
  snprintf(url, sizeof(url),
    "https://claude.ai/api/organizations/%s/usage", gOrgUUID);

  WiFiClientSecure client;
  client.setInsecure(); // no cert validation; acceptable for personal LAN widget
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Cookie",     gSessionKey);
  http.addHeader("User-Agent", "Mozilla/5.0 ClaudeUsageMonitor/1.0");
  http.addHeader("Accept",     "application/json");
  http.setTimeout(5000);

  int code = http.GET();
  if (code == 200) {
    StaticJsonDocument<512> doc;
    if (!deserializeJson(doc, http.getStream())) {
      gUsage.fiveHour     = doc["five_hour"]["utilization"]  | -1.0f;
      gUsage.sevenDay     = doc["seven_day"]["utilization"]  | -1.0f;
      const char* r5      = doc["five_hour"]["resets_at"];
      const char* r7      = doc["seven_day"]["resets_at"];
      gUsage.fiveHourSecs = r5 ? parseResetsAt(r5) : -1;
      gUsage.sevenDaySecs = r7 ? parseResetsAt(r7) : -1;
      gUsage.fetchedAt    = millis();
      gUsage.valid        = true;
    }
  } else if (code == 401 || code == 403) {
    gUsage.valid = false;
  }
  http.end();
}

// ── POST /configure ──────────────────────────────────────────────────────
// Body: { "orgId": "...", "sessionKey": "sessionKey=..." }
void handleConfigure() {
  addCORSHeaders();
  if (server.method() == HTTP_OPTIONS) { server.send(204); return; }

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"no body\"}");
    return;
  }

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }

  const char* uuid = doc["orgId"];
  const char* key  = doc["sessionKey"];

  if (!uuid || strlen(uuid) != 36) {
    server.send(400, "application/json", "{\"error\":\"invalid orgId\"}");
    return;
  }
  if (!key || strlen(key) == 0) {
    server.send(400, "application/json", "{\"error\":\"missing sessionKey\"}");
    return;
  }

  saveCredentials(uuid, key);
  server.send(200, "application/json", "{\"ok\":true}");

  showStatus(CLR_GREEN, "configured!", "fetching data...");
  gLastPoll = 0; // trigger immediate poll
}

// ── GET /status ──────────────────────────────────────────────────────────
void handleStatus() {
  addCORSHeaders();
  StaticJsonDocument<256> doc;
  doc["configured"]    = gConfigured;
  doc["orgId"]         = gOrgUUID;
  doc["hasSessionKey"] = gSessionKey[0] != '\0';
  doc["fiveHour"]      = gUsage.fiveHour;
  doc["sevenDay"]      = gUsage.sevenDay;
  doc["valid"]         = gUsage.valid;
  doc["ip"]            = WiFi.localIP().toString();
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleNotFound() {
  addCORSHeaders();
  server.send(404, "application/json", "{\"error\":\"not found\"}");
}

// ── Setup ────────────────────────────────────────────────────────────────
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.setRotation(0);
  M5.Display.setBrightness(80);

  canvas.createSprite(128, 128);
  canvas.setFont(&fonts::Font0);
  canvas.setTextSize(1);

  // Boot splash
  canvas.fillSprite(CLR_BG);
  canvas.setTextColor(CLR_TEXT);
  drawCentered(48, "CLAUDE");
  drawCentered(60, "USAGE MON");
  canvas.pushSprite(0, 0);
  delay(1000);

  loadCredentials();

  // WiFi
  showStatus(CLR_DIM, "connecting wifi...");

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);

  wm.setAPCallback([](WiFiManager* wm) {
    canvas.fillSprite(CLR_BG);
    canvas.setTextColor(CLR_TEXT);
    drawCentered(16, "WIFI SETUP");
    canvas.drawFastHLine(20, 26, 87, CLR_LINE);
    canvas.setTextColor(CLR_DIM);
    drawCentered(32, "Connect to:");
    canvas.setTextColor(CLR_TEXT);
    drawCentered(44, AP_NAME);
    if (AP_PASS) {
      canvas.setTextColor(CLR_DIM);
      drawCentered(56, "pwd: see firmware");
    }
    canvas.setTextColor(CLR_DIM);
    drawCentered(AP_PASS ? 72 : 60, "then open");
    drawCentered(AP_PASS ? 84 : 72, "192.168.4.1");
    canvas.pushSprite(0, 0);
  });

  if (!wm.autoConnect(AP_NAME, AP_PASS)) {
    showStatus(CLR_RED, "wifi failed");
    delay(3000);
    ESP.restart();
  }

  configTime(TZ_OFFSET_SEC, 0, NTP_SERVER);

  showStatus(CLR_DIM, "syncing time...");
  unsigned long ntpStart = millis();
  while (time(nullptr) < 1000000000UL && millis() - ntpStart < 10000) {
    delay(200);
  }

  // Show IP
  canvas.fillSprite(CLR_BG);
  canvas.setTextColor(CLR_GREEN);
  drawCentered(30, "WiFi connected!");
  canvas.setTextColor(CLR_TEXT);
  drawCentered(50, WiFi.localIP().toString().c_str());
  canvas.setTextColor(CLR_DIM);
  drawCentered(70, "enter IP in");
  drawCentered(82, "extension settings");
  canvas.pushSprite(0, 0);
  delay(5000);

  server.on("/configure", HTTP_POST,    handleConfigure);
  server.on("/configure", HTTP_OPTIONS, handleConfigure);
  server.on("/status",    HTTP_GET,     handleStatus);
  server.onNotFound(handleNotFound);
  server.begin();

  if (gConfigured) pollUsage();
  drawScreen();
}

// ── Loop ─────────────────────────────────────────────────────────────────
void loop() {
  server.handleClient();
  M5.update();

  if (M5.BtnA.pressedFor(2000)) {
    showStatus(CLR_YELLOW, "resetting", "wifi...");
    delay(500);
    WiFiManager wm;
    wm.resetSettings();
    ESP.restart();
  }

  if (WiFi.status() != WL_CONNECTED) {
    delay(5000);
    ESP.restart();
    return;
  }

  static unsigned long lastDraw = 0;

  if (millis() - gLastPoll >= POLL_INTERVAL_MS || gLastPoll == 0) {
    gLastPoll = millis();
    pollUsage();
    drawScreen();
    lastDraw = millis();
  } else if (millis() - lastDraw >= 30000) {
    lastDraw = millis();
    drawScreen();
  }

  delay(100);
}

/*
 * Claude Usage Monitor — ESP8266 NodeMCU (ESP-12E)
 * Display: SSD1306 0.96" OLED, I2C
 *   SDA → D2 (GPIO4)
 *   SCL → D1 (GPIO5)
 *
 * Credentials pushed from Chrome extension — no manual config portal needed.
 * Hold FLASH button 2s to reset WiFi only.
 *
 * Dependencies (Arduino Library Manager):
 *   - U8g2            (display)
 *   - WiFiManager     (WiFi setup only)
 *   - ArduinoJson v6  (JSON parsing)
 *   - ESP8266WebServer (built-in with ESP8266 core)
 *   - ESP8266HTTPClient (built-in)
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <WiFiClientSecureBearSSL.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <time.h>

// ── OLED ──────────────────────────────────────────────────────────────────
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ── EEPROM layout ─────────────────────────────────────────────────────────
#define EEPROM_SIZE      512
#define ADDR_MAGIC       0    // 2 bytes
#define ADDR_ORG_UUID    2    // 40 bytes
#define ADDR_SESSION_KEY 42   // 460 bytes
#define MAGIC_VAL        0xCA75

// ── Config ────────────────────────────────────────────────────────────────
#define POLL_INTERVAL_MS (5UL * 60 * 1000)
#define NTP_SERVER       "pool.ntp.org"
#define HTTP_PORT        80
#define TZ_OFFSET_SEC    (5 * 3600 + 30 * 60)  // IST (UTC+5:30)

// ── State ─────────────────────────────────────────────────────────────────
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
bool      gWifiOk          = false;
unsigned long gLastPoll    = 0;

ESP8266WebServer server(HTTP_PORT);

// ── EEPROM helpers ────────────────────────────────────────────────────────
void eepromWriteStr(int addr, const char* s, int maxLen) {
  int i;
  for (i = 0; i < maxLen - 1 && s[i]; i++) {
    EEPROM.write(addr + i, s[i]);
  }
  EEPROM.write(addr + i, 0); // always write null terminator
}

void eepromReadStr(int addr, char* buf, int maxLen) {
  for (int i = 0; i < maxLen; i++) {
    buf[i] = EEPROM.read(addr + i);
    if (!buf[i]) break;
  }
  buf[maxLen - 1] = 0;
}

bool credentialsSaved() {
  uint16_t magic = (EEPROM.read(ADDR_MAGIC) << 8) | EEPROM.read(ADDR_MAGIC + 1);
  return magic == MAGIC_VAL;
}

void saveCredentials(const char* uuid, const char* key) {
  EEPROM.write(ADDR_MAGIC,     (MAGIC_VAL >> 8) & 0xFF);
  EEPROM.write(ADDR_MAGIC + 1, MAGIC_VAL & 0xFF);
  eepromWriteStr(ADDR_ORG_UUID,    uuid, 40);
  eepromWriteStr(ADDR_SESSION_KEY, key,  460);
  EEPROM.commit();
}

void loadCredentials() {
  eepromReadStr(ADDR_ORG_UUID,    gOrgUUID,    sizeof(gOrgUUID));
  eepromReadStr(ADDR_SESSION_KEY, gSessionKey, sizeof(gSessionKey));
}

// ── CORS ──────────────────────────────────────────────────────────────────
void addCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

// ── POST /configure ───────────────────────────────────────────────────────
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
  loadCredentials();
  server.send(200, "application/json", "{\"ok\":true}");

  // Confirm on display briefly
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(10, 28, "configured!");
  u8g2.drawStr(5, 42, "fetching data...");
  u8g2.sendBuffer();

  gLastPoll = 0; // trigger immediate poll on next loop
}

// ── GET /status ───────────────────────────────────────────────────────────
void handleStatus() {
  addCORSHeaders();
  StaticJsonDocument<256> doc;
  doc["configured"]    = credentialsSaved();
  doc["orgId"]         = gOrgUUID;
  doc["hasSessionKey"] = strlen(gSessionKey) > 0;
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

// ── Display ───────────────────────────────────────────────────────────────
void drawBar(int x, int y, int w, int h, float pct) {
  u8g2.drawFrame(x, y, w, h);
  int fill = (int)((pct / 100.0f) * (w - 2));
  fill = max(0, min(fill, w - 2));
  if (fill > 0) u8g2.drawBox(x + 1, y + 1, fill, h - 2);
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

void drawScreen() {
  u8g2.clearBuffer();

  // Header
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 7, "CLAUDE USAGE");
  char timeBuf[10];
  getTimeStr(timeBuf, sizeof(timeBuf));
  u8g2.drawStr(128 - u8g2.getStrWidth(timeBuf), 7, timeBuf);
  u8g2.drawHLine(0, 9, 128);

  if (!gUsage.valid) {
    u8g2.setFont(u8g2_font_5x7_tf);
    if (!gWifiOk) {
      u8g2.drawStr(10, 35, "no wifi...");
    } else if (!credentialsSaved()) {
      char ipBuf[32];
      snprintf(ipBuf, sizeof(ipBuf), "IP: %s", WiFi.localIP().toString().c_str());
      u8g2.drawStr(0, 22, "waiting for config");
      u8g2.drawStr(0, 34, "push from extension");
      u8g2.setFont(u8g2_font_4x6_tf);
      u8g2.drawStr(0, 46, ipBuf);
      u8g2.drawStr(0, 56, "settings > IoT push");
    } else {
      u8g2.drawStr(20, 32, "fetching...");
    }
    u8g2.sendBuffer();
    return;
  }

  // 5-hour
  char pct5[10], cd5[24];
  snprintf(pct5, sizeof(pct5), "%.1f%%", gUsage.fiveHour);
  // Safe elapsed — handles millis() overflow after 49 days
  unsigned long now = millis();
  long elapsed = (long)((now >= gUsage.fetchedAt)
    ? (now - gUsage.fetchedAt) / 1000
    : (0xFFFFFFFFUL - gUsage.fetchedAt + now) / 1000);
  formatCountdown(gUsage.fiveHourSecs - elapsed, cd5, sizeof(cd5));

  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 19, "5HR");
  int pw = u8g2.getStrWidth(pct5);
  u8g2.drawStr(128 - pw, 19, pct5);
  drawBar(22, 12, 128 - 22 - pw - 3, 8, gUsage.fiveHour);
  u8g2.setFont(u8g2_font_4x6_tf);
  u8g2.drawStr(22, 27, cd5);

  u8g2.drawHLine(0, 30, 128);

  // 7-day
  char pct7[10], cd7[24];
  snprintf(pct7, sizeof(pct7), "%.1f%%", gUsage.sevenDay);
  formatCountdown(gUsage.sevenDaySecs - elapsed, cd7, sizeof(cd7));

  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 41, "7DAY");
  pw = u8g2.getStrWidth(pct7);
  u8g2.drawStr(128 - pw, 41, pct7);
  drawBar(27, 34, 128 - 27 - pw - 3, 8, gUsage.sevenDay);
  u8g2.setFont(u8g2_font_4x6_tf);
  u8g2.drawStr(27, 49, cd7);

  // Footer
  u8g2.drawHLine(0, 54, 128);
  u8g2.setFont(u8g2_font_4x6_tf);
  char syncBuf[20];
  long ageSecs = elapsed;
  if (ageSecs < 60)        snprintf(syncBuf, sizeof(syncBuf), "synced just now");
  else if (ageSecs < 3600) snprintf(syncBuf, sizeof(syncBuf), "synced %ldm ago", ageSecs / 60);
  else                     snprintf(syncBuf, sizeof(syncBuf), "synced %ldh ago", ageSecs / 3600);
  u8g2.drawStr(0, 63, syncBuf);

  u8g2.sendBuffer();
}

// ── Parse ISO8601 to seconds from now ─────────────────────────────────────
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

// ── Poll claude.ai ────────────────────────────────────────────────────────
void pollUsage() {
  if (strlen(gOrgUUID) == 0 || strlen(gSessionKey) == 0) return;

  char url[128];
  snprintf(url, sizeof(url),
    "https://claude.ai/api/organizations/%s/usage", gOrgUUID);

  BearSSL::WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Cookie",     gSessionKey);
  http.addHeader("User-Agent", "Mozilla/5.0 ClaudeUsageMonitor/1.0");
  http.addHeader("Accept",     "application/json");
  http.setTimeout(10000);

  int code = http.GET();
  if (code == 200) {
    StaticJsonDocument<512> doc;
    if (!deserializeJson(doc, http.getString())) {
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

// ── Setup ─────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  u8g2.begin();

  // Boot screen
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(28, 28, "CLAUDE");
  u8g2.drawStr(20, 40, "USAGE MON");
  u8g2.sendBuffer();
  delay(1000);

  if (credentialsSaved()) loadCredentials();

  // WiFi
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(10, 32, "connecting wifi...");
  u8g2.sendBuffer();

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);

  // Only show portal instructions if WiFiManager actually opens the AP
  wm.setAPCallback([](WiFiManager* wm) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 10, "WIFI SETUP");
    u8g2.drawHLine(0, 12, 128);
    u8g2.drawStr(0, 24, "Connect to:");
    u8g2.drawStr(0, 34, "ClaudeMonitor");
    u8g2.setFont(u8g2_font_4x6_tf);
    u8g2.drawStr(0, 46, "then open 192.168.4.1");
    u8g2.drawStr(0, 56, "to enter WiFi password");
    u8g2.sendBuffer();
  });

  if (!wm.autoConnect("ClaudeMonitor")) {
    u8g2.clearBuffer();
    u8g2.drawStr(5, 32, "wifi failed");
    u8g2.sendBuffer();
    delay(3000);
    ESP.restart();
  }

  gWifiOk = true;

  configTime(TZ_OFFSET_SEC, 0, NTP_SERVER);

  // Wait up to 10s for time sync
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(10, 32, "syncing time...");
  u8g2.sendBuffer();

  unsigned long ntpStart = millis();
  while (time(nullptr) < 1000000000UL && millis() - ntpStart < 10000) {
    delay(200);
  }

  // Show IP
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 16, "WiFi connected!");
  char ipBuf[32];
  snprintf(ipBuf, sizeof(ipBuf), "%s", WiFi.localIP().toString().c_str());
  u8g2.drawStr(0, 30, ipBuf);
  u8g2.setFont(u8g2_font_4x6_tf);
  u8g2.drawStr(0, 44, "enter this IP in");
  u8g2.drawStr(0, 52, "extension settings");
  u8g2.sendBuffer();
  delay(5000);

  // Start web server
  server.on("/configure", HTTP_POST,    handleConfigure);
  server.on("/configure", HTTP_OPTIONS, handleConfigure);
  server.on("/status",    HTTP_GET,     handleStatus);
  server.onNotFound(handleNotFound);
  server.begin();

  if (credentialsSaved()) pollUsage();
  drawScreen();

  pinMode(0, INPUT_PULLUP);
}

// ── Loop ──────────────────────────────────────────────────────────────────
void loop() {
  server.handleClient();

  // FLASH button 2s → reset WiFi
  static unsigned long btnDown = 0;
  if (digitalRead(0) == LOW) {
    if (!btnDown) btnDown = millis();
    if (millis() - btnDown > 2000) {
      WiFiManager wm;
      wm.resetSettings();
      ESP.restart();
    }
  } else btnDown = 0;

  if (WiFi.status() != WL_CONNECTED) {
    gWifiOk = false;
    delay(5000);
    ESP.restart();
    return;
  }
  gWifiOk = true;

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

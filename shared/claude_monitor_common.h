/*
 * Shared logic for Claude Usage Monitor firmware (ESP8266 + ESP32 variants).
 *
 * Before including this header, the platform file must:
 *   1. #include platform headers (WiFi, WebServer, crypto, display)
 *   2. #define user-configurable constants: API_KEY, HTTP_PORT, TZ_OFFSET_SEC
 *   3. typedef/using MonitorWebServer = <platform WebServer type>
 *   4. Define: MonitorWebServer server(HTTP_PORT);
 *   5. Define: const char* generateNonce();  (platform-specific RNG)
 *   6. Define: void computeHmacSha256(...);  (platform-specific crypto)
 *   7. Declare: void drawScreen();           (platform-specific display)
 */

#ifndef CLAUDE_MONITOR_COMMON_H
#define CLAUDE_MONITOR_COMMON_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>

// -- Require user config before inclusion ---------------------------------
#ifndef API_KEY
  #error "Define API_KEY before including claude_monitor_common.h"
#endif
#ifndef HTTP_PORT
  #error "Define HTTP_PORT before including claude_monitor_common.h"
#endif
#ifndef TIMEZONE
  #error "Define TIMEZONE before including claude_monitor_common.h (e.g. \"CST6CDT,M3.2.0,M11.1.0\")"
#endif

// -- Optional defaults ----------------------------------------------------
#ifndef NTP_SERVER
  #define NTP_SERVER "pool.ntp.org"
#endif
#ifndef STALE_THRESHOLD_MS
  #define STALE_THRESHOLD_MS (10UL * 60 * 1000)
#endif

// -- Nonce pool constants -------------------------------------------------
#define NONCE_POOL_SIZE 8
#define NONCE_HEX_LEN   16  // 8 random bytes -> 16 hex chars

// -- State ----------------------------------------------------------------
struct UsageData {
  float fiveHour     = -1;
  float sevenDay     = -1;
  long  fiveHourSecs = -1;
  long  sevenDaySecs = -1;
  unsigned long receivedAt = 0;
  bool  valid = false;
  bool  daemonSeen = false;        // true after first /ping or /usage from daemon
  unsigned long lastAuthFailMs = 0; // millis() of last auth failure, 0 = none
};

UsageData gUsage;
char noncePool[NONCE_POOL_SIZE][NONCE_HEX_LEN + 1];
uint8_t nonceHead = 0;

// Firmware loop should check this to avoid redundant redraws.
unsigned long lastDrawMs = 0;

// -- Platform-provided symbols (declared, not defined) --------------------
extern MonitorWebServer server;
void drawScreen();
const char* generateNonce();
void computeHmacSha256(const char* key, size_t keyLen,
                       const uint8_t* data, size_t dataLen,
                       uint8_t out[32]);

// -- Auth helpers ---------------------------------------------------------

static void hexEncode(const uint8_t* data, size_t len, char* out) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[i * 2]     = hex[data[i] >> 4];
    out[i * 2 + 1] = hex[data[i] & 0x0F];
  }
  out[len * 2] = '\0';
}

// Constant-time comparison to avoid timing side-channels.
static bool constantTimeCompare(const char* a, const char* b, size_t len) {
  uint8_t result = 0;
  for (size_t i = 0; i < len; i++) {
    result |= a[i] ^ b[i];
  }
  return result == 0;
}

// Look up nonce in ring buffer. If found, zero the slot (single-use) and return true.
bool findAndInvalidateNonce(const char* nonce) {
  if (!nonce || strlen(nonce) != NONCE_HEX_LEN) return false;
  for (int i = 0; i < NONCE_POOL_SIZE; i++) {
    if (noncePool[i][0] != '\0' && constantTimeCompare(noncePool[i], nonce, NONCE_HEX_LEN)) {
      memset(noncePool[i], 0, NONCE_HEX_LEN + 1);
      return true;
    }
  }
  return false;
}

// Authenticate a request. Tries HMAC (X-Auth-Nonce + X-Auth-Signature) first,
// falls back to legacy X-API-Key header with a serial warning.
bool checkAuth() {
  // HMAC path
  if (server.hasHeader("X-Auth-Nonce") && server.hasHeader("X-Auth-Signature")) {
    String nonce = server.header("X-Auth-Nonce");
    String sig   = server.header("X-Auth-Signature");

    if (!findAndInvalidateNonce(nonce.c_str())) return false;

    // Build message: nonce + body
    String body = server.hasArg("plain") ? server.arg("plain") : String();
    String message = nonce + body;

    uint8_t expected[32];
    computeHmacSha256(API_KEY, strlen(API_KEY),
                      (const uint8_t*)message.c_str(), message.length(),
                      expected);
    char expectedHex[65];
    hexEncode(expected, 32, expectedHex);

    if (sig.length() != 64) return false;
    return constantTimeCompare(sig.c_str(), expectedHex, 64);
  }

  // Legacy path: raw API key
  if (server.hasHeader("X-API-Key")) {
    Serial.println("WARN: legacy X-API-Key auth used; upgrade client to HMAC");
    String provided = server.header("X-API-Key");
    const char* expected = API_KEY;
    if (provided.length() != strlen(expected)) return false;
    return constantTimeCompare(provided.c_str(), expected, provided.length());
  }

  return false;
}

void sendUnauthorized() {
  gUsage.daemonSeen = true;  // something tried to talk to us
  gUsage.lastAuthFailMs = millis();
  Serial.println("AUTH FAIL: request rejected (bad key or signature)");
  server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
  drawScreen();
  lastDrawMs = millis();
}

// -- Time helpers ---------------------------------------------------------

// Convert a broken-down UTC time to epoch seconds without depending on
// the C library's timezone state. Portable across ESP8266/ESP32.
static time_t utcToEpoch(int year, int mon, int mday, int hour, int min, int sec) {
  // Days from year 0 to start of each month (non-leap).
  static const int mdays[] = {0,31,59,90,120,151,181,212,243,273,304,334};
  long y = year;
  long days = 365 * y + y / 4 - y / 100 + y / 400 + mdays[mon - 1] + mday - 1;
  // Leap day adjustment: if month <= Feb of a leap year, subtract 1.
  if (mon <= 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) days--;
  // Epoch is 1970-01-01, which is day 719162 from year 0.
  days -= 719162L;
  return (time_t)(days * 86400L + hour * 3600L + min * 60L + sec);
}

// Parse ISO8601 UTC timestamp to seconds from now.
// Input is always UTC (e.g. "2026-03-22T18:00:00Z").
long parseResetsAt(const char* iso) {
  int y, mo, d, h, mi, s;
  sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s);
  time_t resetUtc = utcToEpoch(y, mo, d, h, mi, s);
  return (long)(resetUtc - time(nullptr));
}

// Elapsed time since last push (overflow-safe).
unsigned long millisSinceReceived() {
  unsigned long now = millis();
  return (now >= gUsage.receivedAt)
    ? (now - gUsage.receivedAt)
    : (0xFFFFFFFFUL - gUsage.receivedAt + now);
}

bool isStale() {
  if (!gUsage.valid) return false;
  return millisSinceReceived() > STALE_THRESHOLD_MS;
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

void formatSyncAge(long secs, char* buf, int bufLen) {
  if (secs < 60)        snprintf(buf, bufLen, "synced just now");
  else if (secs < 3600) snprintf(buf, bufLen, "synced %ldm ago", secs / 60);
  else                  snprintf(buf, bufLen, "synced %ldh ago", secs / 3600);
}

// -- HTTP handlers --------------------------------------------------------

void handleUsage() {
  if (server.method() == HTTP_OPTIONS) { server.send(204); return; }

  if (!checkAuth()) { sendUnauthorized(); return; }

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"no body\"}");
    return;
  }

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }

  gUsage.fiveHour = doc["five_hour"] | -1.0f;
  gUsage.sevenDay = doc["seven_day"] | -1.0f;

  const char* r5 = doc["five_hour_resets_at"];
  const char* r7 = doc["seven_day_resets_at"];
  gUsage.fiveHourSecs = r5 ? parseResetsAt(r5) : -1;
  gUsage.sevenDaySecs = r7 ? parseResetsAt(r7) : -1;
  gUsage.receivedAt = millis();
  gUsage.valid = true;

  server.send(200, "application/json", "{\"ok\":true}");
  drawScreen();
  lastDrawMs = millis();  // Reset draw timer to avoid duplicate redraw.
}

void handleStatus() {
  if (!checkAuth()) { sendUnauthorized(); return; }

  StaticJsonDocument<256> doc;
  doc["fiveHour"] = gUsage.fiveHour;
  doc["sevenDay"] = gUsage.sevenDay;
  doc["valid"]    = gUsage.valid;
  doc["stale"]    = isStale();
  doc["ip"]       = WiFi.localIP().toString();
  doc["port"]     = HTTP_PORT;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// Lightweight health check for daemon connectivity verification.
void handlePing() {
  bool wasUnseen = !gUsage.daemonSeen;
  gUsage.daemonSeen = true;

  StaticJsonDocument<192> doc;
  doc["ok"] = true;
  doc["uptime"] = millis() / 1000;
  doc["nonce"] = generateNonce();
  if (gUsage.valid) {
    doc["last_push_secs_ago"] = millisSinceReceived() / 1000;
  } else {
    doc["last_push_secs_ago"] = (char*)nullptr;
  }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);

  // Redraw so display transitions from "waiting for daemon" to "connected".
  if (wasUnseen) {
    drawScreen();
    lastDrawMs = millis();
  }
}

void handleNotFound() {
  server.send(404, "application/json", "{\"error\":\"not found\"}");
}

#endif // CLAUDE_MONITOR_COMMON_H

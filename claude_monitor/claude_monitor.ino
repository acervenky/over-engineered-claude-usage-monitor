/*
 * Claude Usage Monitor — ESP8266 NodeMCU (ESP-12E)
 * Display: SSD1306 0.96" OLED, I2C
 *   SDA -> D2 (GPIO4)
 *   SCL -> D1 (GPIO5)
 *
 * Push-only display: receives usage data from the Rust daemon.
 * No credentials stored, no outbound HTTPS. Device is a pure display.
 * Hold FLASH button 2s to reset WiFi only.
 *
 * Dependencies (Arduino Library Manager):
 *   - U8g2            (display)
 *   - WiFiManager     (WiFi setup only)
 *   - ArduinoJson v6  (JSON parsing)
 *   - ESP8266WebServer (built-in with ESP8266 core)
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <time.h>
#include <bearssl/bearssl_hmac.h>

// -- User config ----------------------------------------------------------

// HTTP port the device listens on. Must match --device-port on the daemon.
#define HTTP_PORT  8080

// Shared API key. The daemon must send this in the X-API-Key header.
// Change this to a unique value. Same key goes in the daemon's --api-key flag.
#define API_KEY  "sup3rs3cr3t"

#define NTP_SERVER       "pool.ntp.org"
#define TZ_OFFSET_SEC    (5 * 3600 + 30 * 60)  // IST (UTC+5:30)

// Data is considered stale if no push received within this window.
#define STALE_THRESHOLD_MS (10UL * 60 * 1000)  // 10 minutes

// -- Platform type alias + server -----------------------------------------
using MonitorWebServer = ESP8266WebServer;
MonitorWebServer server(HTTP_PORT);

// -- Shared logic (auth, handlers, time helpers) --------------------------
#include "claude_monitor_common.h"

// -- Platform-specific: RNG (ESP8266 hardware register) -------------------
const char* generateNonce() {
  uint8_t raw[NONCE_HEX_LEN / 2];
  for (size_t i = 0; i < sizeof(raw); i += 4) {
    uint32_t r = RANDOM_REG32;
    size_t remain = sizeof(raw) - i;
    memcpy(raw + i, &r, remain < 4 ? remain : 4);
  }
  hexEncode(raw, sizeof(raw), noncePool[nonceHead]);
  const char* nonce = noncePool[nonceHead];
  nonceHead = (nonceHead + 1) % NONCE_POOL_SIZE;
  return nonce;
}

// -- Platform-specific: HMAC-SHA256 via BearSSL ---------------------------
void computeHmacSha256(const char* key, size_t keyLen,
                       const uint8_t* data, size_t dataLen,
                       uint8_t out[32]) {
  br_hmac_key_context kc;
  br_hmac_key_init(&kc, &br_sha256_vtable, key, keyLen);
  br_hmac_context ctx;
  br_hmac_init(&ctx, &kc, 0);
  br_hmac_update(&ctx, data, dataLen);
  br_hmac_out(&ctx, out);
}

// -- OLED -----------------------------------------------------------------
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
bool gWifiOk = false;

// -- Display --------------------------------------------------------------
void drawBar(int x, int y, int w, int h, float pct) {
  u8g2.drawFrame(x, y, w, h);
  int fill = (int)((pct / 100.0f) * (w - 2));
  fill = max(0, min(fill, w - 2));
  if (fill > 0) u8g2.drawBox(x + 1, y + 1, fill, h - 2);
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
    } else {
      // Show IP:port and waiting message
      char addrBuf[32];
      snprintf(addrBuf, sizeof(addrBuf), "%s:%d",
        WiFi.localIP().toString().c_str(), HTTP_PORT);
      u8g2.drawStr(0, 22, "waiting for daemon");
      u8g2.drawHLine(0, 25, 128);
      u8g2.setFont(u8g2_font_4x6_tf);
      u8g2.drawStr(0, 36, "connect daemon to:");
      u8g2.setFont(u8g2_font_5x7_tf);
      u8g2.drawStr(0, 48, addrBuf);
      u8g2.setFont(u8g2_font_4x6_tf);
      u8g2.drawStr(0, 60, "--device-ip <this IP>");
    }
    u8g2.sendBuffer();
    return;
  }

  long elapsed = (long)(millisSinceReceived() / 1000);

  // Staleness indicator
  if (isStale()) {
    u8g2.setFont(u8g2_font_4x6_tf);
    u8g2.drawStr(60, 7, "[STALE]");
  }

  // 5-hour
  char pct5[10], cd5[24];
  snprintf(pct5, sizeof(pct5), "%.1f%%", gUsage.fiveHour);
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
  formatSyncAge(elapsed, syncBuf, sizeof(syncBuf));
  u8g2.drawStr(0, 63, syncBuf);

  u8g2.sendBuffer();
}

// -- Setup ----------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  u8g2.begin();

  // Boot screen
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(28, 28, "CLAUDE");
  u8g2.drawStr(20, 40, "USAGE MON");
  u8g2.sendBuffer();
  delay(1000);

  // WiFi
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(10, 32, "connecting wifi...");
  u8g2.sendBuffer();

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);

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

  // Keep DST disabled. parseResetsAt() relies on a fixed offset here.
  configTime(TZ_OFFSET_SEC, 0, NTP_SERVER);

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(10, 32, "syncing time...");
  u8g2.sendBuffer();

  unsigned long ntpStart = millis();
  while (time(nullptr) < 1000000000UL && millis() - ntpStart < 10000) {
    delay(200);
  }

  // Show IP:port and connection instructions
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 10, "WiFi connected!");
  u8g2.drawHLine(0, 13, 128);
  char addrBuf[32];
  snprintf(addrBuf, sizeof(addrBuf), "%s:%d",
    WiFi.localIP().toString().c_str(), HTTP_PORT);
  u8g2.drawStr(0, 26, addrBuf);
  u8g2.setFont(u8g2_font_4x6_tf);
  u8g2.drawStr(0, 40, "waiting for daemon...");
  u8g2.drawStr(0, 52, "run: claude-usage-daemon");
  u8g2.drawStr(0, 60, "  --device-ip <this IP>");
  u8g2.sendBuffer();
  delay(5000);

  // Collect auth headers so we can read them in handlers.
  server.collectHeaders("X-API-Key", "X-Auth-Nonce", "X-Auth-Signature");

  server.on("/usage",  HTTP_POST,    handleUsage);
  server.on("/usage",  HTTP_OPTIONS, handleUsage);
  server.on("/status", HTTP_GET,     handleStatus);
  server.on("/ping",   HTTP_GET,     handlePing);
  server.onNotFound(handleNotFound);
  server.begin();

  drawScreen();

  pinMode(0, INPUT_PULLUP);
}

// -- Loop -----------------------------------------------------------------
void loop() {
  server.handleClient();

  // FLASH button 2s -> reset WiFi
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

  // Redraw every 30s for countdown timer updates.
  // lastDrawMs is reset by handleUsage() after push-triggered redraws.
  if (millis() - lastDrawMs >= 30000) {
    lastDrawMs = millis();
    drawScreen();
  }

  delay(100);
}

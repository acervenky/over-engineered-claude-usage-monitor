/*
 * Claude Usage Monitor — M5Stack ATOMS3R (ESP32-S3)
 * Display: ST7735S/GC9107 0.85" round IPS LCD, 128x128, color
 * Self-contained unit, no external wiring needed.
 *
 * Push-only display: receives usage data from the Rust daemon.
 * No credentials stored, no outbound HTTPS. Device is a pure display.
 * Hold built-in button 2s to reset WiFi only.
 *
 * Dependencies (PlatformIO):
 *   - M5Unified       (display, button, hardware abstraction)
 *   - ArduinoJson v6  (JSON parsing)
 *   - WiFiManager     (WiFi setup only)
 *   - WebServer       (built-in with ESP32 core)
 */

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <time.h>
#include <mbedtls/md.h>
#include <esp_random.h>

// -- User config ----------------------------------------------------------

// WiFi access point name and password for first-boot setup.
// Leave AP_PASS as nullptr for an open network (no password).
#define AP_NAME  "ClaudeMonitor"
#define AP_PASS  nullptr              // e.g. "mypassword" for WPA2

// HTTP port the device listens on. Must match --device-port on the daemon.
#define HTTP_PORT  8080

// Shared API key. The daemon must send this in the X-API-Key header.
// Change this to a unique value. Same key goes in the daemon's --api-key flag.
#define API_KEY  "sup3rs3cr3t"

// Timezone offset from UTC in seconds. Used for NTP clock and countdown
// timers. Both configTime() and parseResetsAt() depend on this value.
//
//   UTC-08:00  PST   US Pacific      (-8 * 3600)
//   UTC-05:00  EST   US Eastern      (-5 * 3600)
//   UTC+00:00  GMT/UTC               (0)
//   UTC+01:00  CET   Central Europe  (1 * 3600)
//   UTC+05:30  IST   India           (5 * 3600 + 30 * 60)
//   UTC+09:00  JST   Japan/KST       (9 * 3600)
//
// Note: standard time only, no automatic DST switching.
#define TZ_OFFSET_SEC  (5 * 3600 + 30 * 60)  // IST (UTC+5:30)

// NTP server for time sync
#define NTP_SERVER  "pool.ntp.org"

// Data is considered stale if no push received within this window.
#define STALE_THRESHOLD_MS (10UL * 60 * 1000)  // 10 minutes

// -- Platform type alias + server -----------------------------------------
using MonitorWebServer = WebServer;
MonitorWebServer server(HTTP_PORT);

// -- Shared logic (auth, handlers, time helpers) --------------------------
#include "claude_monitor_common.h"

// -- Platform-specific: RNG (ESP32 hardware RNG) --------------------------
const char* generateNonce() {
  uint8_t raw[NONCE_HEX_LEN / 2];
  esp_fill_random(raw, sizeof(raw));
  hexEncode(raw, sizeof(raw), noncePool[nonceHead]);
  const char* nonce = noncePool[nonceHead];
  nonceHead = (nonceHead + 1) % NONCE_POOL_SIZE;
  return nonce;
}

// -- Platform-specific: HMAC-SHA256 via mbedTLS ---------------------------
void computeHmacSha256(const char* key, size_t keyLen,
                       const uint8_t* data, size_t dataLen,
                       uint8_t out[32]) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const uint8_t*)key, keyLen);
  mbedtls_md_hmac_update(&ctx, data, dataLen);
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
}

// -- Display --------------------------------------------------------------
M5Canvas canvas(&M5.Display);

// -- Colors (RGB565) ------------------------------------------------------
#define CLR_BG        0x0000
#define CLR_TEXT      0xFFFF
#define CLR_DIM       0x7BEF
#define CLR_LINE      0x4208
#define CLR_GREEN     0x07E0
#define CLR_YELLOW    0xFFE0
#define CLR_RED       0xF800
#define CLR_ORANGE    0xFD20
#define CLR_BAR_BG    0x2104

// -- Display helpers ------------------------------------------------------

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

void drawCentered(int y, const char* text) {
  int w = canvas.textWidth(text);
  canvas.drawString(text, (128 - w) / 2, y);
}

void showStatus(uint16_t color, const char* line1, const char* line2 = nullptr) {
  canvas.fillSprite(CLR_BG);
  canvas.setTextColor(color);
  drawCentered(line2 ? 48 : 56, line1);
  if (line2) drawCentered(62, line2);
  canvas.pushSprite(0, 0);
}

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

// -- Main display ---------------------------------------------------------
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
    } else {
      // Show IP:port and waiting message
      char addrBuf[24];
      snprintf(addrBuf, sizeof(addrBuf), "%s",
        WiFi.localIP().toString().c_str());
      char portBuf[12];
      snprintf(portBuf, sizeof(portBuf), "port %d", HTTP_PORT);

      drawCentered(36, "waiting for");
      drawCentered(46, "daemon");
      canvas.drawFastHLine(L, 52, W, CLR_LINE);
      canvas.setTextColor(CLR_TEXT);
      drawCentered(58, addrBuf);
      canvas.setTextColor(CLR_DIM);
      drawCentered(70, portBuf);
    }
    canvas.pushSprite(0, 0);
    return;
  }

  long elapsed = (long)(millisSinceReceived() / 1000);

  // Staleness indicator in header
  if (isStale()) {
    canvas.setTextColor(CLR_ORANGE);
    canvas.drawString("STALE", L, 18);
    canvas.setTextColor(CLR_TEXT);
  }

  drawUsageBlock(32, L, R, W, "5HR",  gUsage.fiveHour, gUsage.fiveHourSecs, elapsed);
  canvas.drawFastHLine(L, 65, W, CLR_LINE);
  drawUsageBlock(68, L, R, W, "7DAY", gUsage.sevenDay, gUsage.sevenDaySecs, elapsed);

  // Footer
  canvas.setTextColor(CLR_DIM);
  char syncBuf[24];
  formatSyncAge(elapsed, syncBuf, sizeof(syncBuf));
  drawCentered(98, syncBuf);

  canvas.pushSprite(0, 0);
}

// -- Setup ----------------------------------------------------------------
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

  // Keep DST disabled. parseResetsAt() relies on a fixed offset here.
  configTime(TZ_OFFSET_SEC, 0, NTP_SERVER);

  showStatus(CLR_DIM, "syncing time...");
  unsigned long ntpStart = millis();
  while (time(nullptr) < 1000000000UL && millis() - ntpStart < 10000) {
    delay(200);
  }

  // Show IP:port and connection instructions
  {
    char addrBuf[24];
    snprintf(addrBuf, sizeof(addrBuf), "%s",
      WiFi.localIP().toString().c_str());
    char portBuf[12];
    snprintf(portBuf, sizeof(portBuf), "port %d", HTTP_PORT);

    canvas.fillSprite(CLR_BG);
    canvas.setTextColor(CLR_GREEN);
    drawCentered(22, "WiFi connected!");
    canvas.drawFastHLine(20, 32, 87, CLR_LINE);
    canvas.setTextColor(CLR_TEXT);
    drawCentered(40, addrBuf);
    canvas.setTextColor(CLR_DIM);
    drawCentered(52, portBuf);
    canvas.drawFastHLine(20, 60, 87, CLR_LINE);
    drawCentered(68, "waiting for");
    drawCentered(80, "daemon push...");
    canvas.pushSprite(0, 0);
  }
  delay(5000);

  // Collect auth headers so we can read them in handlers.
  const char* hdrs[] = {"X-API-Key", "X-Auth-Nonce", "X-Auth-Signature"};
  server.collectHeaders(hdrs, 3);

  server.on("/usage",  HTTP_POST,    handleUsage);
  server.on("/usage",  HTTP_OPTIONS, handleUsage);
  server.on("/status", HTTP_GET,     handleStatus);
  server.on("/ping",   HTTP_GET,     handlePing);
  server.onNotFound(handleNotFound);
  server.begin();

  drawScreen();
}

// -- Loop -----------------------------------------------------------------
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

  // Redraw every 30s for countdown timer updates.
  // lastDrawMs is reset by handleUsage() after push-triggered redraws.
  if (millis() - lastDrawMs >= 30000) {
    lastDrawMs = millis();
    drawScreen();
  }

  delay(100);
}

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
#include <esp_wifi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <time.h>
#include <mbedtls/md.h>
#include <esp_random.h>

// -- User config ----------------------------------------------------------

// Fallback config portal AP. Only used when the device can't connect to
// a saved network. You join this temporary AP to configure your real WiFi.
// This is NOT your home network name -- pick something distinct.
// Leave AP_PASS as nullptr for an open portal (no password).
#define AP_NAME  "ClaudeMonitor-Setup"
#define AP_PASS  nullptr

// Optional Static IP configuration. Uncomment and set to use a static IP instead of DHCP.
// Ensure you use commas, not dots, for the IP octets.
// #define STATIC_IP 192, 168, 1, 100
// #define STATIC_GW 192, 168, 1, 1
// #define STATIC_SN 255, 255, 255, 0

// HTTP port the device listens on. Must match --device-port on the daemon.
#define HTTP_PORT  8080

// Shared API key. The daemon must send this in the X-API-Key header.
// Change this to a unique value. Same key goes in the daemon's --api-key flag.
#define API_KEY  "sup3rs3cr3t"

// POSIX timezone string. Handles DST transitions automatically.
// Pick one and uncomment, or write your own POSIX TZ string.
//
// -- US --
#define TIMEZONE  "CST6CDT,M3.2.0,M11.1.0"   // US Central
// #define TIMEZONE  "EST5EDT,M3.2.0,M11.1.0"   // US Eastern
// #define TIMEZONE  "MST7MDT,M3.2.0,M11.1.0"   // US Mountain
// #define TIMEZONE  "PST8PDT,M3.2.0,M11.1.0"   // US Pacific
// #define TIMEZONE  "MST7"                       // Arizona (no DST)
// #define TIMEZONE  "HST10"                      // Hawaii (no DST)
//
// -- Europe --
// #define TIMEZONE  "GMT0BST,M3.5.0/1,M10.5.0"  // UK
// #define TIMEZONE  "CET-1CEST,M3.5.0,M10.5.0/3" // Central Europe
// #define TIMEZONE  "EET-2EEST,M3.5.0/3,M10.5.0/4" // Eastern Europe
//
// -- Asia / Oceania --
// #define TIMEZONE  "IST-5:30"                   // India (no DST)
// #define TIMEZONE  "JST-9"                      // Japan (no DST)
// #define TIMEZONE  "KST-9"                      // Korea (no DST)
// #define TIMEZONE  "CST-8"                      // China (no DST)
// #define TIMEZONE  "AEST-10AEDT,M10.1.0,M4.1.0/3" // Australia Eastern
//
// -- Other --
// #define TIMEZONE  "UTC0"                       // UTC (no DST)

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

// -- WiFi disconnect reason tracking --------------------------------------
// Captured by event handler so the AP callback can display why connection failed.
volatile uint8_t lastDisconnectReason = 0;

const char* wifiReasonStr(uint8_t reason) {
  switch (reason) {
    case 2:   return "auth expired";
    case 3:   return "AP deauthed";
    case 8:   return "left AP";
    case 15:  return "4way handshake timeout";
    case 16:  return "group key timeout";
    case 201: return "AP not found";
    case 202: return "wrong password";
    case 203: return "assoc rejected";
    default:  return nullptr;  // unknown, show raw code
  }
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
    if (WiFi.status() != WL_CONNECTED) {
      canvas.setTextColor(CLR_DIM);
      drawCentered(56, "no wifi...");
    } else if (gUsage.lastAuthFailMs != 0) {
      // Daemon tried to push but auth failed
      canvas.setTextColor(CLR_RED);
      drawCentered(36, "AUTH FAILED");
      canvas.drawFastHLine(L, 46, W, CLR_LINE);
      canvas.setTextColor(CLR_DIM);
      drawCentered(52, "check API_KEY");
      drawCentered(62, "matches daemon");
      canvas.drawFastHLine(L, 72, W, CLR_LINE);
      canvas.setTextColor(CLR_TEXT);
      char addrBuf[24];
      snprintf(addrBuf, sizeof(addrBuf), "%s", WiFi.localIP().toString().c_str());
      drawCentered(80, addrBuf);
    } else if (gUsage.daemonSeen) {
      // Daemon pinged us but hasn't pushed data yet
      canvas.setTextColor(CLR_GREEN);
      drawCentered(36, "daemon found");
      canvas.drawFastHLine(L, 46, W, CLR_LINE);
      canvas.setTextColor(CLR_DIM);
      drawCentered(56, "fetching data...");
    } else {
      // No contact from daemon yet
      canvas.setTextColor(CLR_DIM);
      char addrBuf[24];
      snprintf(addrBuf, sizeof(addrBuf), "%s", WiFi.localIP().toString().c_str());
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
  // Capture disconnect reason so we can display it if connection fails.
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    lastDisconnectReason = info.wifi_sta_disconnected.reason;
    Serial.printf("WiFi disconnect reason: %d\n", lastDisconnectReason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);

  // Show which saved network we're attempting, or "no saved network"
  {
    wifi_config_t conf;
    esp_wifi_get_config(WIFI_IF_STA, &conf);
    const char* savedSSID = (const char*)conf.sta.ssid;
    if (savedSSID[0] != '\0') {
      canvas.fillSprite(CLR_BG);
      canvas.setTextColor(CLR_DIM);
      drawCentered(40, "connecting to");
      canvas.setTextColor(CLR_TEXT);
      drawCentered(54, savedSSID);
      canvas.pushSprite(0, 0);
    } else {
      showStatus(CLR_DIM, "no saved network");
    }
  }

#ifdef STATIC_IP
  IPAddress ip(STATIC_IP);
  IPAddress gw(STATIC_GW);
  IPAddress sn(STATIC_SN);
  wm.setSTAStaticIPConfig(ip, gw, sn);
#endif

  // This callback fires when autoConnect can't reach the saved network
  // and falls back to hosting a config portal AP.
  wm.setAPCallback([](WiFiManager* wm) {
    // Build human-readable reason string
    char reasonBuf[28];
    const char* known = wifiReasonStr(lastDisconnectReason);
    if (lastDisconnectReason == 0) {
      snprintf(reasonBuf, sizeof(reasonBuf), "no saved network");
    } else if (known) {
      snprintf(reasonBuf, sizeof(reasonBuf), "%s", known);
    } else {
      snprintf(reasonBuf, sizeof(reasonBuf), "error code %d", lastDisconnectReason);
    }
    Serial.printf("WiFi connect failed: %s\n", reasonBuf);

    canvas.fillSprite(CLR_BG);
    canvas.setTextColor(CLR_RED);
    drawCentered(6, "CONNECT FAILED");
    // Show why it failed
    canvas.setTextColor(CLR_ORANGE);
    drawCentered(16, reasonBuf);
    canvas.drawFastHLine(20, 26, 87, CLR_LINE);
    canvas.setTextColor(CLR_DIM);
    drawCentered(30, "Join WiFi AP:");
    canvas.setTextColor(CLR_TEXT);
    drawCentered(42, AP_NAME);
    if (AP_PASS) {
      canvas.setTextColor(CLR_DIM);
      drawCentered(54, "pwd: see firmware");
    }
    canvas.setTextColor(CLR_DIM);
    drawCentered(AP_PASS ? 68 : 58, "then open:");
    canvas.setTextColor(CLR_TEXT);
    drawCentered(AP_PASS ? 80 : 70, "192.168.4.1");
    canvas.setTextColor(CLR_DIM);
    drawCentered(AP_PASS ? 96 : 86, "to pick network");
    canvas.pushSprite(0, 0);
  });

  if (!wm.autoConnect(AP_NAME, AP_PASS)) {
    // Portal timed out with no config. Countdown then restart.
    for (int i = 10; i > 0; i--) {
      char buf[24];
      snprintf(buf, sizeof(buf), "retry in %ds", i);
      showStatus(CLR_RED, "wifi failed", buf);
      delay(1000);
    }
    ESP.restart();
  }

  // POSIX TZ string handles DST transitions automatically.
  configTzTime(TIMEZONE, NTP_SERVER);

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

    Serial.println();
    Serial.println("===============================");
    Serial.print("WiFi connected! IP: ");
    Serial.println(addrBuf);
    Serial.print("Listening on HTTP port: ");
    Serial.println(HTTP_PORT);
    Serial.println("===============================");

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

# Claude Usage Monitor -- ESP8266 Firmware

## Hardware
- NodeMCU ESP-12E (ESP8266)
- SSD1306 0.96" OLED (I2C)
  - SDA: D2 (GPIO4)
  - SCL: D1 (GPIO5)
  - VCC: 3.3V
  - GND: GND

## Arduino IDE Setup

### Board
- Board: "NodeMCU 1.0 (ESP-12E Module)"
- Flash Size: 4MB (FS:2MB OTA:~1019KB)
- Upload Speed: 115200

### Libraries (install via Library Manager)
- U8g2 by oliver
- WiFiManager by tzapu
- ArduinoJson by Benoit Blanchon (v6.x)

Or from the project root: `make setup-esp8266`

## Firmware Configuration

Edit `claude_monitor/claude_monitor.ino` before flashing:

```c
#define HTTP_PORT  8080                          // port the device listens on
#define API_KEY    "change-me-to-something-secret"  // CHANGE THIS — must match daemon --api-key
#define TZ_OFFSET_SEC  (5 * 3600 + 30 * 60)     // your timezone offset from UTC
```

## First Boot Setup (WiFi Only)

1. Flash the firmware.
2. On first boot, it creates a WiFi AP: **ClaudeMonitor**.
3. Connect to that AP on your phone/laptop; browser opens captive portal.
4. Enter your WiFi credentials so the device joins your Local Area Network.
5. The device will show its IP address, port, and "waiting for daemon" on the OLED.

## Running

1. Note the IP:port shown on the device display.
2. Start the Rust daemon pointed at the device:

```bash
claude-usage-daemon --device-ip <IP> --api-key "your-shared-secret"
```

The daemon pushes usage data every 5 minutes. The device renders progress bars and countdown timers between pushes. If no data is received for 10 minutes, a "STALE" indicator appears.

Authentication uses HMAC-SHA256 challenge-response so the raw API key never crosses the wire. See [Security](docs/SECURITY.md) for details.

## WiFi Reset

Hold the **FLASH button** (GPIO0) for 2 seconds to wipe WiFi settings and reboot into Access Point mode.

## Display Layout

```text
CLAUDE USAGE            9:42p
------------------------------
5HR   [########.....]      9.0%
      resets 4h 57m
------------------------------
7DAY  [###########..]     21.0%
      resets 6d 12h
------------------------------
synced 3m ago
```

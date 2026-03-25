# Claude Usage Monitor -- M5Stack ATOMS3R Firmware

## Quick Start

### 1. Flash the firmware

Requires [PlatformIO](https://platformio.org/). Connect the ATOMS3R via USB-C.

```bash
cd claude_monitor_atoms3r
pio run -e atoms3r -t upload
```

Or from the project root: `make flash-atoms3r`

Before flashing, edit `src/main.cpp` to set your timezone and API key:

```c
#define HTTP_PORT      8080                      // must match daemon --device-port
#define API_KEY        "change-me-to-something-secret"  // must match daemon --api-key
#define TZ_OFFSET_SEC  (-6 * 3600)               // your UTC offset (e.g. CST = -6h)
```

### 2. Configure WiFi

The device does not store WiFi credentials in firmware. On first boot (or after a WiFi reset), it creates a temporary setup network:

1. The display shows **CONNECT FAILED / no saved network**.
2. On your phone or laptop, join the WiFi network **ClaudeMonitor-Setup** (open, no password).
3. A captive portal opens automatically (or browse to **192.168.4.1**).
4. Select your 2.4GHz WiFi network from the scan list and enter the password.
5. The device saves credentials to flash, reboots, and connects to your network.
6. The display shows the assigned DHCP IP address and listening port.

Credentials persist across reboots. To reconfigure WiFi, hold the **built-in button** for 2 seconds.

### 3. Start the daemon

Point the Rust daemon at the IP:port shown on the device display:

```bash
claude-usage-daemon --device-ip <IP> --api-key "your-shared-secret"
```

The daemon pushes usage data every 5 minutes. The device renders color-coded progress bars and countdown timers between pushes. If no data is received for 10 minutes, a "STALE" indicator appears in orange.

## Hardware

M5Stack ATOMS3R (C126): self-contained, no external wiring.

- ESP32-S3 (dual-core LX7, 240MHz)
- 0.85" round IPS LCD (GC9A01, 128x128, color)
- 8MB Flash, 8MB PSRAM
- Built-in button (GPIO41)
- USB-C
- WiFi + Bluetooth 5.0

### Dependencies (managed by PlatformIO)

- M5Unified by M5Stack (display, button, hardware abstraction)
- ArduinoJson by Benoit Blanchon (v6.x)
- WiFiManager by tzapu (WiFi provisioning)

## WiFi Troubleshooting

If the device fails to connect to a saved network, the display shows the reason:

| Message | Meaning |
|---|---|
| no saved network | First boot or after WiFi reset, no credentials stored |
| wrong password | Saved password doesn't match the AP |
| AP not found | SSID not visible (out of range, powered off, or 5GHz only) |
| 4way handshake timeout | WPA2 handshake failed (weak signal or AP busy) |
| auth expired | AP dropped the session |
| error code N | Uncommon failure, check ESP32 WiFi reason codes |

After 3 minutes in the config portal with no input, the device reboots with a 10-second countdown and retries.

## WiFi Reset

Hold the **built-in button** for 2 seconds to wipe WiFi settings and reboot into Access Point mode.

## Display Layout (128x128 round color)

```text
       .--________________--.
      /  CLAUDE USAGE 3:42p  \
     | _______________________ |
     |                         |
     |  5HR            12.3%   |
     |  ########_________      |
     |  resets 4h 22m          |
     |                         |
     |  7DAY           34.5%   |
     |  ##############___      |
     |  resets 5d 11h          |
     |                         |
     |  synced 2m ago          |
      \                      /
       '--________________--'
```

Progress bars are color-coded:
- Green: < 50% utilization
- Yellow: 50-79% utilization
- Red: >= 80% utilization

## Differences from ESP8266 Version

| Feature | ESP8266 | ATOMS3R |
|---|---|---|
| MCU | ESP8266 (160MHz single-core) | ESP32-S3 (240MHz dual-core) |
| Display | SSD1306 0.96" OLED, 128x64, mono | GC9A01 0.85" IPS, 128x128, color |
| Wiring | 4 wires (I2C) | None (self-contained) |
| Flash | 4MB | 8MB |
| RAM | 80KB | 512KB + 8MB PSRAM |
| Build | Arduino IDE | PlatformIO |
| WiFi reset | FLASH button (GPIO0) | Built-in button (GPIO41) |

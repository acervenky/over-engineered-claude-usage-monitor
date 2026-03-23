# Claude Usage Monitor -- M5Stack ATOMS3R Firmware

## Hardware

M5Stack ATOMS3R (C126): self-contained, no external wiring.

- ESP32-S3 (dual-core LX7, 240MHz)
- 0.85" round IPS LCD (GC9A01, 128x128, color)
- 8MB Flash, 8MB PSRAM
- Built-in button (GPIO41)
- USB-C
- WiFi + Bluetooth 5.0

## Firmware Configuration

Edit `src/main.cpp` before flashing:

```c
#define HTTP_PORT  8080                          // port the device listens on
#define API_KEY    "change-me-to-something-secret"  // must match daemon --api-key
#define TZ_OFFSET_SEC  (5 * 3600 + 30 * 60)     // your timezone offset from UTC
```

## PlatformIO Setup

```bash
cd claude_monitor_atoms3r
pio run -e atoms3r             # compile
pio run -e atoms3r -t upload   # flash
pio device monitor             # serial monitor
```

Or from the project root: `make flash-atoms3r`

### Dependencies (managed by PlatformIO)

- M5Unified by M5Stack (display, button, hardware abstraction)
- ArduinoJson by Benoit Blanchon (v6.x)
- WiFiManager by tzapu (WiFi provisioning)

## First Boot Setup (WiFi Only)

1. Flash the firmware via USB-C.
2. On first boot, it creates a WiFi AP: **ClaudeMonitor**.
3. Connect to that AP on your phone/laptop; browser opens captive portal.
4. Enter your WiFi credentials so the device joins your Local Area Network.
5. The device shows its IP address and port on the round display.

## Running

1. Note the IP:port shown on the device display.
2. Start the Rust daemon pointed at the device:

```bash
claude-usage-daemon --device-ip <IP> --api-key "your-shared-secret"
```

The daemon pushes usage data every 5 minutes. The device renders color-coded progress bars and countdown timers between pushes. If no data is received for 10 minutes, a "STALE" indicator appears in orange.

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

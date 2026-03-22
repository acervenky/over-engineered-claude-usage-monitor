# Claude Usage Monitor — M5Stack ATOMS3R Firmware

## Hardware

M5Stack ATOMS3R (C126): self-contained, no external wiring.

- ESP32-S3 (dual-core LX7, 240MHz)
- 0.85" round IPS LCD (GC9A01, 128x128, color)
- 8MB Flash, 8MB PSRAM
- Built-in button (GPIO41)
- USB-C
- WiFi + Bluetooth 5.0

## PlatformIO Setup

```bash
cd claude_monitor_atoms3r
pio run -e atoms3r        # compile
pio run -e atoms3r -t upload   # flash
pio device monitor        # serial monitor
```

### Dependencies (managed by PlatformIO)

- M5Unified by M5Stack (display, button, hardware abstraction)
- ArduinoJson by Benoit Blanchon (v6.x)
- WiFiManager by tzapu (WiFi provisioning)

## First Boot Setup (WiFi Only)

1. Flash the firmware via USB-C.
2. On first boot, it creates a WiFi AP: **ClaudeMonitor**.
3. Connect to that AP on your phone/laptop, browser opens captive portal.
4. Enter your WiFi credentials so the device joins your Local Area Network.
5. The device shows its IP address on the round display.

## Device Configuration (Extension Push)

1. Connect your computer to the same WiFi network as the device.
2. Note the IP address displayed on the screen.
3. Open the Claude Usage Monitor Chrome Extension **Settings**.
4. Enter the device IP under "IoT Push Target" and click **Push to device**.

The extension will read your active `sessionKey` cookie and Org UUID, and HTTP POST them directly over your local network to the ATOMS3R.

## Updating Credentials & WiFi

- **Session updates:** The Chrome extension automatically repushes the latest session cookie in the background whenever the browser is open.
- **WiFi reset:** Hold the **built-in button** for 2 seconds to wipe WiFi settings and reboot into Access Point mode.

## Security Note

`setInsecure()` skips TLS cert validation. This is acceptable for a personal
local widget, the data is non-sensitive usage stats on your own account.

## Display Layout (128x128 round color)

```text
       .--──────────────--.
      /  CLAUDE USAGE 3:42p \
     | ─────────────────────  |
     |                        |
     |  5HR           12.3%   |
     |  ████████░░░░░░░░░░░   |
     |  resets 4h 22m         |
     |                        |
     |  7DAY           34.5%  |
     |  ██████████████░░░░░   |
     |  resets 5d 11h         |
     |                        |
     |  synced 2m ago         |
      \   192.168.1.42      /
       '--──────────────--'
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
| Storage | EEPROM (512 bytes, raw) | NVS Preferences (key-value) |
| Flash | 4MB | 8MB |
| RAM | 80KB | 512KB + 8MB PSRAM |
| Build | Arduino IDE | PlatformIO |
| WiFi reset | FLASH button (GPIO0) | Built-in button (GPIO41) |

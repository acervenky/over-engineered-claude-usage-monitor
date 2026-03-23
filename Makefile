.PHONY: all daemon daemon-debug clean flash-atoms3r flash-esp8266 install help \
       build-atoms3r build-esp8266 monitor-atoms3r monitor-esp8266 \
       daemon-check daemon-clippy setup-esp8266 setup-atoms3r setup deps

DAEMON_DIR  := claude_usage_daemon
ATOMS3R_DIR := claude_monitor_atoms3r
DAEMON_BIN  := $(DAEMON_DIR)/target/release/claude-usage-daemon

ESP8266_FQBN     := esp8266:esp8266:nodemcuv2
ESP8266_BOARD_URL := https://arduino.esp8266.com/stable/package_esp8266com_index.json

UNAME_S := $(shell uname -s)

all: daemon ## Build everything

# -- OS-aware dependency install -------------------------------------------
# Installs arduino-cli, platformio, and Rust toolchain if missing.
# macOS: uses Homebrew.  Linux: uses curl installers and pip.

deps: ## Install build tools (arduino-cli, platformio, cargo) if missing
ifeq ($(UNAME_S),Darwin)
	@command -v brew >/dev/null || { echo "Error: Homebrew is required on macOS. Install from https://brew.sh"; exit 1; }
	@command -v arduino-cli >/dev/null || { echo "Installing arduino-cli..."; brew install arduino-cli; }
	@command -v pio >/dev/null || { echo "Installing platformio..."; brew install platformio; }
	@command -v cargo >/dev/null || { echo "Installing rust..."; brew install rust; }
else ifeq ($(UNAME_S),Linux)
	@command -v arduino-cli >/dev/null || { echo "Installing arduino-cli..."; \
		curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh && \
		sudo mv bin/arduino-cli /usr/local/bin/ && rm -rf bin; }
	@command -v pio >/dev/null || { echo "Installing platformio..."; pip3 install --user platformio; }
	@command -v cargo >/dev/null || { echo "Installing rust..."; \
		curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y; }
else
	$(error Unsupported OS: $(UNAME_S). Supported: Darwin (macOS), Linux)
endif
	@echo "All build tools installed."

# -- Setup (one-time, after deps) ------------------------------------------

setup: deps setup-esp8266 setup-atoms3r ## Install all toolchains, board cores, and libraries

setup-esp8266: ## Install ESP8266 board core and Arduino libraries
	arduino-cli core update-index --additional-urls $(ESP8266_BOARD_URL)
	arduino-cli core install esp8266:esp8266 --additional-urls $(ESP8266_BOARD_URL)
	arduino-cli lib install U8g2 WiFiManager "ArduinoJson@6"

setup-atoms3r: ## Install ATOMS3R dependencies (PlatformIO auto-downloads on first build)
	@echo "PlatformIO downloads dependencies on first build. Nothing to do."

# -- Rust Daemon -----------------------------------------------------------

daemon: ## Build the daemon (release)
	cd $(DAEMON_DIR) && cargo build --release
	@echo ""
	@echo "Binary: $(DAEMON_BIN)"
	@echo "Run:    $(DAEMON_BIN) --device-ip <IP> --api-key <KEY>"

daemon-debug: ## Build the daemon (debug, faster compile)
	cd $(DAEMON_DIR) && cargo build

daemon-check: ## Check the daemon compiles without building
	cd $(DAEMON_DIR) && cargo check

daemon-clippy: ## Run clippy lints on the daemon
	cd $(DAEMON_DIR) && cargo clippy -- -W clippy::all

install: daemon ## Install the daemon binary to ~/.cargo/bin
	cd $(DAEMON_DIR) && cargo install --path .

# -- Serial Port Detection ------------------------------------------------
# Override with: make flash-esp8266 SERIAL_PORT=/dev/ttyACM0

ifeq ($(UNAME_S),Linux)
    SERIAL_PORT ?= $(firstword $(wildcard /dev/ttyUSB* /dev/ttyACM*))
else
    SERIAL_PORT ?= $(firstword $(wildcard /dev/cu.usbserial*))
endif

# -- Firmware: ESP8266 ----------------------------------------------------

build-esp8266: ## Build ESP8266 firmware without flashing
	arduino-cli compile --fqbn $(ESP8266_FQBN) claude_monitor/

flash-esp8266: ## Build and flash ESP8266 firmware (requires arduino-cli)
ifndef SERIAL_PORT
	$(error No serial port detected. Plug in the device or set SERIAL_PORT manually)
endif
	arduino-cli compile --fqbn $(ESP8266_FQBN) claude_monitor/
	arduino-cli upload --fqbn $(ESP8266_FQBN) -p $(SERIAL_PORT) claude_monitor/

monitor-esp8266: ## Open serial monitor for ESP8266
ifndef SERIAL_PORT
	$(error No serial port detected. Plug in the device or set SERIAL_PORT manually)
endif
	arduino-cli monitor -p $(SERIAL_PORT) --config baudrate=115200

# -- Firmware: ATOMS3R ----------------------------------------------------

build-atoms3r: ## Build ATOMS3R firmware without flashing
	cd $(ATOMS3R_DIR) && pio run -e atoms3r

flash-atoms3r: ## Build and flash ATOMS3R firmware (requires PlatformIO)
	cd $(ATOMS3R_DIR) && pio run -e atoms3r -t upload

monitor-atoms3r: ## Open serial monitor for ATOMS3R
	cd $(ATOMS3R_DIR) && pio device monitor

# -- Housekeeping ----------------------------------------------------------

clean: ## Remove build artifacts
	cd $(DAEMON_DIR) && cargo clean
	rm -rf $(ATOMS3R_DIR)/.pio

help: ## Show this help
	@grep -E '^[a-zA-Z0-9_-]+:' $(MAKEFILE_LIST) | grep ' ## ' | \
		sort | \
		awk 'BEGIN {FS = ":.* ## "}; {printf "  \033[36m%-18s\033[0m %s\n", $$1, $$2}'

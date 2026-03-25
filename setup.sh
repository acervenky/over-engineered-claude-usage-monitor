#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# Claude Usage Monitor -- Interactive Setup
# =============================================================================

REPO="whit3rabbit/over-engineered-claude-usage-monitor"
BINARY_NAME="claude-usage-daemon"
INSTALL_DIR="$HOME/.local/bin"
LAUNCHD_LABEL="com.claude.usage-daemon"
LAUNCHD_PLIST="$HOME/Library/LaunchAgents/${LAUNCHD_LABEL}.plist"
SYSTEMD_UNIT="$HOME/.config/systemd/user/${BINARY_NAME}.service"

ESP8266_FQBN="esp8266:esp8266:nodemcuv2"
ESP8266_BOARD_URL="https://arduino.esp8266.com/stable/package_esp8266com_index.json"
ESP8266_DIR="claude_monitor"
ATOMS3R_DIR="claude_monitor_atoms3r"

# Resolve project root (directory containing this script)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---- Colors -----------------------------------------------------------------

if [ -t 1 ]; then
    GREEN='\033[32m'
    RED='\033[31m'
    YELLOW='\033[33m'
    CYAN='\033[36m'
    BOLD='\033[1m'
    RESET='\033[0m'
else
    GREEN='' RED='' YELLOW='' CYAN='' BOLD='' RESET=''
fi

ok()   { printf "  %-16s ${GREEN}[INSTALLED]${RESET}  %s\n" "$1" "${2:-}"; }
fail() { printf "  %-16s ${RED}[NOT FOUND]${RESET}  %s\n" "$1" "${2:-}"; }
act()  { printf "  %-16s ${GREEN}[ACTIVE]${RESET}    %s\n" "$1" "${2:-}"; }
inact(){ printf "  %-16s ${YELLOW}[INACTIVE]${RESET}  %s\n" "$1" "${2:-}"; }
none() { printf "  %-16s ${RED}[NOT SET]${RESET}   %s\n" "$1" "${2:-}"; }
die()  { echo "Error: $*" >&2; exit 1; }
info() { echo "==> $*"; }

# ---- Platform Detection ----------------------------------------------------

detect_platform() {
    RAW_OS="$(uname -s)"
    ARCH="$(uname -m)"

    case "$RAW_OS" in
        Darwin) OS="darwin" ;;
        Linux)  OS="linux"  ;;
        *)      die "Unsupported OS: $RAW_OS" ;;
    esac

    case "$ARCH" in
        x86_64)         ARCH="x86_64" ;;
        aarch64|arm64)  ARCH="aarch64" ;;
        *)              die "Unsupported architecture: $ARCH" ;;
    esac

    if [ "$OS" = "linux" ]; then
        TARGET="${ARCH}-unknown-linux-gnu"
    else
        TARGET="${ARCH}-apple-darwin"
    fi
}

# ---- Status Dashboard -------------------------------------------------------

show_status() {
    detect_platform

    echo ""
    printf "${BOLD}Claude Usage Monitor Setup${RESET}\n"
    echo "=========================="
    echo ""
    printf "System:   %s %s\n" "$RAW_OS" "$ARCH"
    echo ""

    # -- Tools --
    printf "${BOLD}Tools:${RESET}\n"
    if command -v cargo &>/dev/null; then
        ok "cargo" "$(cargo --version 2>/dev/null | head -1)"
    else
        fail "cargo"
    fi
    if command -v arduino-cli &>/dev/null; then
        ok "arduino-cli" "$(arduino-cli version 2>/dev/null | head -1)"
    else
        fail "arduino-cli"
    fi
    if command -v pio &>/dev/null; then
        ok "platformio" "$(pio --version 2>/dev/null | head -1)"
    else
        fail "platformio"
    fi
    echo ""

    # -- Daemon --
    printf "${BOLD}Daemon:${RESET}\n"

    if [ -x "${INSTALL_DIR}/${BINARY_NAME}" ]; then
        ok "binary" "${INSTALL_DIR}/${BINARY_NAME}"
    else
        fail "binary" "${INSTALL_DIR}/${BINARY_NAME}"
    fi

    case ":${PATH}:" in
        *":${INSTALL_DIR}:"*) printf "  %-16s ${GREEN}[OK]${RESET}          %s\n" "PATH" "~/.local/bin in PATH" ;;
        *)                    fail "PATH" "~/.local/bin not in PATH" ;;
    esac

    # Service status
    local svc_active=false
    local device_host=""
    if [ "$RAW_OS" = "Darwin" ]; then
        if launchctl list 2>/dev/null | grep -q "$LAUNCHD_LABEL"; then
            act "startup svc" "launchd: ${LAUNCHD_LABEL}"
            svc_active=true
        elif [ -f "$LAUNCHD_PLIST" ]; then
            inact "startup svc" "launchd plist exists but not loaded"
        else
            none "startup svc"
        fi
        # Parse device host from plist
        if [ -f "$LAUNCHD_PLIST" ]; then
            # The host is the string element after --device-host in ProgramArguments
            device_host="$(/usr/libexec/PlistBuddy -c "Print :ProgramArguments:2" "$LAUNCHD_PLIST" 2>/dev/null || true)"
        fi
    else
        if systemctl --user is-active "${BINARY_NAME}.service" &>/dev/null; then
            act "startup svc" "systemd: ${BINARY_NAME}.service"
            svc_active=true
        elif [ -f "$SYSTEMD_UNIT" ]; then
            inact "startup svc" "systemd unit exists but not active"
        else
            none "startup svc"
        fi
        # Parse device host from service file
        if [ -f "$SYSTEMD_UNIT" ]; then
            device_host="$(grep -oP '(?<=--device-host )\S+' "$SYSTEMD_UNIT" 2>/dev/null || true)"
        fi
    fi

    if [ -n "$device_host" ]; then
        printf "  %-16s %s\n" "device host" "$device_host"
    fi
    echo ""

    # -- Serial Ports --
    printf "${BOLD}Serial Ports:${RESET}\n"
    local found_port=false
    if [ "$RAW_OS" = "Darwin" ]; then
        for port in /dev/cu.usbserial* /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART*; do
            [ -e "$port" ] || continue
            printf "  %s\n" "$port"
            found_port=true
        done
    else
        for port in /dev/ttyUSB* /dev/ttyACM*; do
            [ -e "$port" ] || continue
            printf "  %s\n" "$port"
            found_port=true
        done
    fi
    $found_port || printf "  ${YELLOW}(none detected)${RESET}\n"
    echo ""
}

# ---- Daemon Install/Uninstall -----------------------------------------------

do_install_daemon() {
    local binary_path="${OPT_BINARY:-}"

    detect_platform

    # Resolve binary: explicit path > cwd > download
    if [ -n "$binary_path" ]; then
        [ -f "$binary_path" ] || die "Binary not found: $binary_path"
    elif [ -f "${SCRIPT_DIR}/${BINARY_NAME}" ]; then
        binary_path="${SCRIPT_DIR}/${BINARY_NAME}"
        info "Using binary from project root"
    elif [ -f "${SCRIPT_DIR}/claude_usage_daemon/target/release/${BINARY_NAME}" ]; then
        binary_path="${SCRIPT_DIR}/claude_usage_daemon/target/release/${BINARY_NAME}"
        info "Using binary from local release build"
    else
        info "Downloading latest release for ${TARGET}..."
        local url="https://github.com/${REPO}/releases/latest/download/${BINARY_NAME}-${TARGET}.tar.gz"
        local tmpdir
        tmpdir="$(mktemp -d)"
        trap 'rm -rf "$tmpdir"' EXIT

        if command -v curl &>/dev/null; then
            curl -fsSL "$url" | tar xz -C "$tmpdir"
        elif command -v wget &>/dev/null; then
            wget -qO- "$url" | tar xz -C "$tmpdir"
        else
            die "curl or wget required to download the binary"
        fi

        binary_path="${tmpdir}/${BINARY_NAME}"
        [ -f "$binary_path" ] || die "Download failed: binary not found in archive"
    fi

    # Install binary
    mkdir -p "$INSTALL_DIR"
    cp "$binary_path" "${INSTALL_DIR}/${BINARY_NAME}"
    chmod 755 "${INSTALL_DIR}/${BINARY_NAME}"
    info "Installed ${BINARY_NAME} to ${INSTALL_DIR}/"

    # macOS: remove quarantine attribute from downloaded binaries
    if [ "$(uname -s)" = "Darwin" ] && xattr -l "${INSTALL_DIR}/${BINARY_NAME}" 2>/dev/null | grep -q quarantine; then
        xattr -d com.apple.quarantine "${INSTALL_DIR}/${BINARY_NAME}" 2>/dev/null || true
        info "Removed macOS quarantine attribute"
    fi

    # PATH check
    case ":${PATH}:" in
        *":${INSTALL_DIR}:"*) ;;
        *)
            echo ""
            echo "WARNING: ${INSTALL_DIR} is not in your PATH."
            echo "Add it by running one of the following:"
            echo ""
            echo "  # bash"
            echo "  echo 'export PATH=\"\$HOME/.local/bin:\$PATH\"' >> ~/.bashrc && source ~/.bashrc"
            echo ""
            echo "  # zsh"
            echo "  echo 'export PATH=\"\$HOME/.local/bin:\$PATH\"' >> ~/.zshrc && source ~/.zshrc"
            echo ""
            echo "  # fish"
            echo "  fish_add_path ~/.local/bin"
            echo ""
            ;;
    esac

    info "Done."
}

do_uninstall_daemon() {
    # Warn if service is active
    if [ "$(uname -s)" = "Darwin" ]; then
        if launchctl list 2>/dev/null | grep -q "$LAUNCHD_LABEL"; then
            echo "WARNING: Startup service is still active. Uninstall the service first (option 5)."
            read -rp "Continue anyway? [y/N] " answer
            case "$answer" in
                [yY]|[yY][eE][sS]) ;;
                *) echo "Aborted."; return ;;
            esac
        fi
    else
        if systemctl --user is-active "${BINARY_NAME}.service" &>/dev/null; then
            echo "WARNING: Startup service is still active. Uninstall the service first (option 5)."
            read -rp "Continue anyway? [y/N] " answer
            case "$answer" in
                [yY]|[yY][eE][sS]) ;;
                *) echo "Aborted."; return ;;
            esac
        fi
    fi

    if [ -f "${INSTALL_DIR}/${BINARY_NAME}" ]; then
        rm -f "${INSTALL_DIR}/${BINARY_NAME}"
        info "Removed ${INSTALL_DIR}/${BINARY_NAME}"
    else
        echo "Binary not found at ${INSTALL_DIR}/${BINARY_NAME}. Nothing to remove."
    fi
}

# ---- Service Management -----------------------------------------------------

do_install_service() {
    local device_host="${OPT_DEVICE_HOST:-}"

    [ -x "${INSTALL_DIR}/${BINARY_NAME}" ] || die "Daemon binary not installed. Install it first (option 1)."

    if [ -z "$device_host" ]; then
        read -rp "Device host IP or hostname: " device_host
        [ -n "$device_host" ] || die "Device host is required"
    fi

    local bin="${INSTALL_DIR}/${BINARY_NAME}"

    case "$(uname -s)" in
        Darwin) install_launchd "$bin" "$device_host" ;;
        Linux)  install_systemd "$bin" "$device_host" ;;
    esac
}

install_launchd() {
    local bin="$1" host="$2"

    # Stop existing service if loaded
    launchctl bootout "gui/$(id -u)/${LAUNCHD_LABEL}" 2>/dev/null \
        || launchctl unload "$LAUNCHD_PLIST" 2>/dev/null \
        || true

    mkdir -p "$(dirname "$LAUNCHD_PLIST")"

    cat > "$LAUNCHD_PLIST" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>${LAUNCHD_LABEL}</string>

    <key>ProgramArguments</key>
    <array>
        <string>${bin}</string>
        <string>--device-host</string>
        <string>${host}</string>
    </array>

    <key>RunAtLoad</key>
    <true/>

    <key>KeepAlive</key>
    <true/>

    <key>StandardOutPath</key>
    <string>/tmp/claude-usage-daemon.log</string>

    <key>StandardErrorPath</key>
    <string>/tmp/claude-usage-daemon.err</string>

    <key>ThrottleInterval</key>
    <integer>60</integer>
</dict>
</plist>
PLIST

    if launchctl bootstrap "gui/$(id -u)" "$LAUNCHD_PLIST" 2>/dev/null; then
        info "launchd service loaded"
    elif launchctl load "$LAUNCHD_PLIST" 2>/dev/null; then
        info "launchd service loaded (legacy)"
    else
        info "Plist written to ${LAUNCHD_PLIST}"
        echo "    Load manually: launchctl load ${LAUNCHD_PLIST}"
    fi
}

install_systemd() {
    local bin="$1" host="$2"

    systemctl --user stop "${BINARY_NAME}.service" 2>/dev/null || true

    mkdir -p "$(dirname "$SYSTEMD_UNIT")"

    cat > "$SYSTEMD_UNIT" <<UNIT
[Unit]
Description=Claude Usage Monitor Daemon
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=${bin} --device-host ${host}
Restart=on-failure
RestartSec=60
Environment=RUST_LOG=info

[Install]
WantedBy=default.target
UNIT

    systemctl --user daemon-reload
    systemctl --user enable --now "${BINARY_NAME}.service"
    info "systemd user service enabled and started"
    echo ""
    echo "    Status:  systemctl --user status ${BINARY_NAME}"
    echo "    Logs:    journalctl --user -u ${BINARY_NAME} -f"
    echo ""
    echo "    For headless servers (no login session), enable lingering:"
    echo "    sudo loginctl enable-linger \$USER"
}

do_change_host() {
    local new_host="${OPT_DEVICE_HOST:-}"

    case "$(uname -s)" in
        Darwin)
            [ -f "$LAUNCHD_PLIST" ] || die "No launchd plist found. Install the service first (option 3)."
            if [ -z "$new_host" ]; then
                local current
                current="$(/usr/libexec/PlistBuddy -c "Print :ProgramArguments:2" "$LAUNCHD_PLIST" 2>/dev/null || echo "unknown")"
                echo "Current device host: $current"
                read -rp "New device host IP or hostname: " new_host
                [ -n "$new_host" ] || die "Device host is required"
            fi
            # Update plist
            /usr/libexec/PlistBuddy -c "Set :ProgramArguments:2 $new_host" "$LAUNCHD_PLIST"
            # Reload
            launchctl bootout "gui/$(id -u)/${LAUNCHD_LABEL}" 2>/dev/null \
                || launchctl unload "$LAUNCHD_PLIST" 2>/dev/null \
                || true
            if launchctl bootstrap "gui/$(id -u)" "$LAUNCHD_PLIST" 2>/dev/null; then
                info "Service reloaded with host: $new_host"
            elif launchctl load "$LAUNCHD_PLIST" 2>/dev/null; then
                info "Service reloaded with host: $new_host (legacy)"
            else
                info "Plist updated. Reload manually: launchctl load ${LAUNCHD_PLIST}"
            fi
            ;;
        Linux)
            [ -f "$SYSTEMD_UNIT" ] || die "No systemd unit found. Install the service first (option 3)."
            if [ -z "$new_host" ]; then
                local current
                current="$(grep -oP '(?<=--device-host )\S+' "$SYSTEMD_UNIT" 2>/dev/null || echo "unknown")"
                echo "Current device host: $current"
                read -rp "New device host IP or hostname: " new_host
                [ -n "$new_host" ] || die "Device host is required"
            fi
            local bin="${INSTALL_DIR}/${BINARY_NAME}"
            # Rewrite service file with new host
            sed -i "s|--device-host .*|--device-host ${new_host}|" "$SYSTEMD_UNIT"
            systemctl --user daemon-reload
            systemctl --user restart "${BINARY_NAME}.service"
            info "Service restarted with host: $new_host"
            ;;
    esac
}

do_uninstall_service() {
    case "$(uname -s)" in
        Darwin)
            launchctl bootout "gui/$(id -u)/${LAUNCHD_LABEL}" 2>/dev/null \
                || launchctl unload "$LAUNCHD_PLIST" 2>/dev/null \
                || true
            rm -f "$LAUNCHD_PLIST"
            info "Removed launchd service"
            ;;
        Linux)
            systemctl --user disable --now "${BINARY_NAME}.service" 2>/dev/null || true
            rm -f "$SYSTEMD_UNIT"
            systemctl --user daemon-reload 2>/dev/null || true
            info "Removed systemd service"
            ;;
    esac
}

# ---- Firmware Dependencies ---------------------------------------------------

do_install_firmware_deps() {
    local choice="${1:-}"

    if [ -z "$choice" ]; then
        echo ""
        echo "Install dependencies for which firmware?"
        echo ""
        echo "  a) ESP8266 (NodeMCU)    -- arduino-cli + board core + libraries"
        echo "  b) ATOMS3R (M5Stack)    -- platformio"
        echo "  c) All"
        echo "  q) Back"
        echo ""
        read -rp "Choice [a/b/c/q]: " choice
    fi

    case "$choice" in
        a|A) install_esp8266_deps ;;
        b|B) install_atoms3r_deps ;;
        c|C) install_esp8266_deps; install_atoms3r_deps ;;
        q|Q) return ;;
        *)   echo "Invalid choice."; return ;;
    esac
}

install_esp8266_deps() {
    info "Installing ESP8266 dependencies..."

    # Install arduino-cli if missing
    if ! command -v arduino-cli &>/dev/null; then
        case "$(uname -s)" in
            Darwin)
                command -v brew &>/dev/null || die "Homebrew is required on macOS. Install from https://brew.sh"
                info "Installing arduino-cli via Homebrew..."
                brew install arduino-cli
                ;;
            Linux)
                info "Installing arduino-cli..."
                curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh
                sudo mv bin/arduino-cli /usr/local/bin/ && rm -rf bin
                ;;
        esac
    else
        info "arduino-cli already installed"
    fi

    # Install board core and libraries
    info "Installing ESP8266 board core..."
    arduino-cli core update-index --additional-urls "$ESP8266_BOARD_URL"
    arduino-cli core install esp8266:esp8266 --additional-urls "$ESP8266_BOARD_URL"

    info "Installing Arduino libraries..."
    arduino-cli lib install U8g2 WiFiManager "ArduinoJson@6"

    info "ESP8266 dependencies ready."
}

install_atoms3r_deps() {
    info "Installing ATOMS3R dependencies..."

    # Install platformio if missing
    if ! command -v pio &>/dev/null; then
        case "$(uname -s)" in
            Darwin)
                command -v brew &>/dev/null || die "Homebrew is required on macOS. Install from https://brew.sh"
                info "Installing PlatformIO via Homebrew..."
                brew install platformio
                ;;
            Linux)
                info "Installing PlatformIO via pip..."
                pip3 install --user platformio
                ;;
        esac
    else
        info "PlatformIO already installed"
    fi

    info "ATOMS3R dependencies ready. Board packages download automatically on first build."
}

# ---- Firmware Build/Flash ----------------------------------------------------

do_build_firmware() {
    echo ""
    echo "Build which firmware?"
    echo ""
    echo "  a) ESP8266 (NodeMCU)"
    echo "  b) ATOMS3R (M5Stack)"
    echo "  q) Back"
    echo ""
    read -rp "Choice [a/b/q]: " choice

    case "$choice" in
        a|A)
            command -v arduino-cli &>/dev/null || die "arduino-cli not found. Install dependencies first (option 6)."
            info "Building ESP8266 firmware..."
            arduino-cli compile --fqbn "$ESP8266_FQBN" "${SCRIPT_DIR}/${ESP8266_DIR}/"
            info "ESP8266 build complete."
            ;;
        b|B)
            command -v pio &>/dev/null || die "PlatformIO not found. Install dependencies first (option 6)."
            info "Building ATOMS3R firmware..."
            cd "${SCRIPT_DIR}/${ATOMS3R_DIR}" && pio run -e atoms3r
            info "ATOMS3R build complete."
            ;;
        q|Q) return ;;
        *)   echo "Invalid choice." ;;
    esac
}

do_flash_firmware() {
    echo ""
    echo "Flash which firmware?"
    echo ""
    echo "  a) ESP8266 (NodeMCU)"
    echo "  b) ATOMS3R (M5Stack)"
    echo "  q) Back"
    echo ""
    read -rp "Choice [a/b/q]: " choice

    case "$choice" in
        a|A) flash_esp8266 ;;
        b|B) flash_atoms3r ;;
        q|Q) return ;;
        *)   echo "Invalid choice." ;;
    esac
}

detect_serial_port() {
    local port=""
    case "$(uname -s)" in
        Darwin)
            for p in /dev/cu.usbserial* /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART*; do
                [ -e "$p" ] && port="$p" && break
            done
            ;;
        Linux)
            for p in /dev/ttyUSB* /dev/ttyACM*; do
                [ -e "$p" ] && port="$p" && break
            done
            ;;
    esac
    echo "$port"
}

flash_esp8266() {
    command -v arduino-cli &>/dev/null || die "arduino-cli not found. Install dependencies first (option 6)."

    local port
    port="$(detect_serial_port)"

    if [ -z "$port" ]; then
        echo "No serial port auto-detected."
        read -rp "Enter serial port path (e.g., /dev/ttyUSB0): " port
        [ -n "$port" ] || die "Serial port required"
    else
        echo "Detected serial port: $port"
        read -rp "Use this port? [Y/n] " answer
        case "$answer" in
            [nN]|[nN][oO])
                read -rp "Enter serial port path: " port
                [ -n "$port" ] || die "Serial port required"
                ;;
        esac
    fi

    info "Building and flashing ESP8266..."
    arduino-cli compile --fqbn "$ESP8266_FQBN" "${SCRIPT_DIR}/${ESP8266_DIR}/"
    arduino-cli upload --fqbn "$ESP8266_FQBN" -p "$port" "${SCRIPT_DIR}/${ESP8266_DIR}/"
    info "ESP8266 flash complete."
}

flash_atoms3r() {
    command -v pio &>/dev/null || die "PlatformIO not found. Install dependencies first (option 6)."

    info "Building and flashing ATOMS3R..."
    cd "${SCRIPT_DIR}/${ATOMS3R_DIR}" && pio run -e atoms3r -t upload
    info "ATOMS3R flash complete."
}

# ---- Menu --------------------------------------------------------------------

show_menu() {
    # Check whether a service config exists (controls visibility of options 4/5)
    local has_service=false
    if [ "$(uname -s)" = "Darwin" ]; then
        [ -f "$LAUNCHD_PLIST" ] && has_service=true
    else
        [ -f "$SYSTEMD_UNIT" ] && has_service=true
    fi

    echo "What would you like to do?"
    echo ""
    echo "  1) Install daemon binary"
    echo "  2) Uninstall daemon binary"
    echo "  3) Install startup service"
    if $has_service; then
        echo "  4) Change device host IP"
        echo "  5) Uninstall startup service"
    fi
    echo "  6) Install firmware dependencies"
    echo "  7) Build firmware"
    echo "  8) Flash firmware"
    echo "  9) Quit"
    echo ""
    read -rp "Choice [1-9]: " choice

    case "$choice" in
        1) do_install_daemon ;;
        2) do_uninstall_daemon ;;
        3) do_install_service ;;
        4) $has_service && do_change_host || echo "No service installed. Install the service first (option 3)." ;;
        5) $has_service && do_uninstall_service || echo "No service installed." ;;
        6) do_install_firmware_deps ;;
        7) do_build_firmware ;;
        8) do_flash_firmware ;;
        9) exit 0 ;;
        *) echo "Invalid choice." ;;
    esac
}

# ---- CLI Argument Parsing ----------------------------------------------------

OPT_BINARY=""
OPT_DEVICE_HOST=""
ACTION=""

parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --install-daemon)    ACTION="install-daemon"; shift ;;
            --uninstall-daemon)  ACTION="uninstall-daemon"; shift ;;
            --install-service)   ACTION="install-service"; shift ;;
            --change-host)       ACTION="change-host"; OPT_DEVICE_HOST="${2:-}"; shift; [ -n "$OPT_DEVICE_HOST" ] && shift ;;
            --uninstall-service) ACTION="uninstall-service"; shift ;;
            --status)            ACTION="status"; shift ;;
            --binary)            OPT_BINARY="$2"; shift 2 ;;
            --device-host)       OPT_DEVICE_HOST="$2"; shift 2 ;;
            -h|--help)
                echo "Usage: $0 [OPTIONS]"
                echo ""
                echo "Run without arguments for interactive menu."
                echo ""
                echo "Options:"
                echo "  --install-daemon           Download/install daemon to ~/.local/bin"
                echo "  --uninstall-daemon         Remove daemon binary"
                echo "  --install-service          Install startup service (launchd/systemd)"
                echo "  --change-host <host>       Update device host in startup service"
                echo "  --uninstall-service        Remove startup service"
                echo "  --status                   Show status dashboard only"
                echo "  --binary <path>            Path to prebuilt binary (with --install-daemon)"
                echo "  --device-host <host>       Device IP/hostname (with --install-service)"
                echo "  -h, --help                 Show this help"
                exit 0
                ;;
            *) die "Unknown option: $1. Use --help for usage." ;;
        esac
    done
}

# ---- Main --------------------------------------------------------------------

main() {
    parse_args "$@"

    case "$ACTION" in
        install-daemon)    do_install_daemon ;;
        uninstall-daemon)  do_uninstall_daemon ;;
        install-service)   do_install_service ;;
        change-host)       do_change_host ;;
        uninstall-service) do_uninstall_service ;;
        status)            show_status ;;
        "")
            # Interactive mode: show status then menu loop
            show_status
            while true; do
                show_menu
                echo ""
            done
            ;;
    esac
}

main "$@"

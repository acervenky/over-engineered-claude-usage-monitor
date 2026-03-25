# claude-usage-daemon

[![CI](https://github.com/whit3rabbit/over-engineered-claude-usage-monitor/actions/workflows/ci.yml/badge.svg)](https://github.com/whit3rabbit/over-engineered-claude-usage-monitor/actions/workflows/ci.yml)

Lightweight Rust daemon that reads Claude CLI OAuth credentials, polls the Anthropic usage API, and pushes results to IoT display devices over LAN.

## Install

### Prebuilt binaries

Download the latest release for your platform from [GitHub Releases](https://github.com/whit3rabbit/over-engineered-claude-usage-monitor/releases), or use the interactive setup script from the project root:

```bash
# Download and run the setup script
curl -fsSLO https://raw.githubusercontent.com/whit3rabbit/over-engineered-claude-usage-monitor/main/setup.sh
bash setup.sh
```

The setup script provides an interactive menu for daemon install/uninstall, startup service management, firmware dependencies, building, and flashing. It also shows a status dashboard with green/red indicators for installed tools and components.

### Non-interactive usage

```bash
./setup.sh --install-daemon                           # download + install binary
./setup.sh --install-daemon --binary ./claude-usage-daemon  # use a local binary
./setup.sh --install-service --device-host 192.168.1.50     # set up startup service
./setup.sh --uninstall-daemon                         # remove binary
./setup.sh --uninstall-service                        # remove startup service
./setup.sh --status                                   # show status dashboard
```

### macOS quarantine

Downloaded binaries may be quarantined by Gatekeeper. The setup script handles this automatically. If you downloaded manually:

```bash
xattr -d com.apple.quarantine ~/.local/bin/claude-usage-daemon
```

## Requirements

- [Rust](https://rustup.rs/) 1.70+
- Claude CLI logged in (`claude` command, which writes OAuth tokens to Keychain or `~/.claude/.credentials.json`)

## Build

```bash
cargo build --release
```

Binary: `target/release/claude-usage-daemon`

Or install to `~/.cargo/bin`:

```bash
cargo install --path .
```

## Quick Start

The only required argument is the device address. Defaults match the firmware out of the box.

```bash
# Minimal: just the device IP
claude-usage-daemon -H 192.168.1.50

# Or use a hostname
claude-usage-daemon -H my-device.local
```

This uses the default API key (`sup3rs3cr3t`), port 8080, and 5-minute poll interval, which all match the firmware defaults.

### Changing the API key

If you want a unique key (recommended for shared networks), change it in both places:

1. **Firmware**: edit `API_KEY` in `main.cpp` / `.ino` and re-flash
2. **Daemon**: pass `--api-key` or set the env var

```bash
# Via flag (visible in ps output)
claude-usage-daemon -H 192.168.1.50 -k "my-secret-key"

# Via env var (preferred, hidden from ps)
export CLAUDE_DEVICE_API_KEY="my-secret-key"
claude-usage-daemon -H 192.168.1.50
```

## Usage

### Foreground (default)

Logs to stderr. Ctrl+C to stop.

```bash
claude-usage-daemon -H 192.168.1.50
```

### Background daemon (Unix only)

Forks to background via `libc::daemon()`. Writes PID file for stop mechanism.

```bash
claude-usage-daemon -H 192.168.1.50 --daemon

# Stop it later:
kill $(cat /tmp/claude-usage-daemon.pid)
```

### All options

```
-H, --device-host <HOST>   Required. Device IP or hostname (e.g., 192.168.1.50, my-device.local).
-p, --device-port <PORT>   Port the device listens on. Default: 8080.
-k, --api-key <KEY>        Must match API_KEY in firmware. Default: sup3rs3cr3t.
-n, --interval <SECS>      Poll interval in seconds (min 10). Default: 300 (5 min).
-c, --config-dir <DIR>     Override Claude config directory. Default: ~/.claude.
-d, --daemon               Fork to background (Unix only). Foreground is default.
-f, --pid-file <PATH>      PID file path. Default: $TMPDIR/claude-usage-daemon.pid.
-h, --help                 Print help.
```

### Environment variables

- `CLAUDE_DEVICE_API_KEY`: Device API key (same as `-k`/`--api-key`, preferred to avoid `ps` exposure).
- `RUST_LOG`: Override log level (e.g., `RUST_LOG=debug`). Foreground defaults to `info`, daemon defaults to `warn`.
- `CLAUDE_CONFIG_DIR`: Override config directory (same as `--config-dir`).

## Credential Sources

The daemon reads OAuth tokens written by the Claude CLI. It tries sources in this order:

| Priority | Source | Platform |
|----------|--------|----------|
| 1 | macOS Keychain (service: `Claude Code-credentials`) | macOS only |
| 2 | `~/.claude/.credentials.json` | All platforms |

**macOS Keychain prompt:** On first run, macOS will show a dialog asking whether to allow the daemon to access the "Claude Code-credentials" keychain item. Click **Always Allow** so the daemon can read and refresh OAuth tokens without prompting on every launch.

### Token refresh

When the access token is within 60 seconds of expiry, the daemon refreshes it via `POST https://platform.claude.com/v1/oauth/token` using the stored refresh token. Refreshed credentials are written back atomically (see below).

### Credential file format

```json
{
  "claudeAiOauth": {
    "accessToken": "...",
    "refreshToken": "...",
    "expiresAt": 1742673600000,
    "scopes": ["user:profile", "user:inference"]
  }
}
```

A flat format (`accessToken` at root level) is also accepted as a fallback.

## API Endpoints

### Anthropic Usage API

```
GET https://api.anthropic.com/api/oauth/usage
Authorization: Bearer {accessToken}
anthropic-beta: oauth-2025-04-20
```

Returns per-window utilization percentages and reset timestamps.

### Device Push

```
POST http://{device-ip}:{port}/usage
X-API-Key: {api-key}
Content-Type: application/json

{
  "five_hour": 45.2,
  "five_hour_resets_at": "2026-03-22T18:00:00Z",
  "seven_day": 12.8,
  "seven_day_resets_at": "2026-03-25T00:00:00Z",
  "seven_day_opus": 8.1,
  "seven_day_sonnet": 15.0,
  "timestamp": 1742673600
}
```

### Device Health Check

```
GET http://{device-ip}:{port}/ping
```

No API key required. Returns `{"ok":true,"uptime":3600,"last_push_secs_ago":120}`.

The daemon pings the device on startup and on push failures to distinguish "device is down" from "push was rejected."

## Error Handling and Backoff

The daemon classifies errors and adjusts behavior accordingly.

### Error types

| Error | Cause | Behavior |
|-------|-------|----------|
| **AuthExpired** | 401/403 from usage API | Clear cached credentials, force reload + refresh next cycle |
| **AuthRevoked** | 401/403 from token refresh endpoint | Log loudly, retry from disk (CLI may have re-authed) |
| **Transient** | 429, 5xx, network timeout | Exponential backoff (interval * 2^n, max 1 hour) |
| **NotFound** | No credentials file, keychain empty | Log error, retry at normal interval |
| **DeviceOffline** | Device push failed | Log warning, continue normal interval |

### Backoff behavior

- **Transient errors**: Exponential backoff. Base interval doubles on each consecutive failure (300s, 600s, 1200s, ...), capped at 1 hour. Resets to base interval on success.
- **Auth failures**: After 3 consecutive auth failures, the daemon slows to 30-minute intervals and logs:
  ```
  ERROR: Authentication failed 3 times. Re-run `claude` CLI to refresh tokens. Retrying in 30 minutes.
  ```
  Resets when a successful cycle completes (e.g., user re-authenticates via CLI).
- **Device offline**: No backoff. The daemon continues fetching usage data on schedule and retries the push each cycle.

## Concurrency Safety

### File locking

The daemon and Claude CLI may both write to `.credentials.json`. To prevent corruption:

- Writes go to `.credentials.json.tmp` first, then atomically renamed over the target file.
- An exclusive advisory lock (`.credentials.lock`) is held during the write.
- Lock acquisition retries up to 5 times with randomized 1-2 second backoff, matching the Claude CLI's locking behavior.
- The lock file uses `flock()` on Unix and `LockFileEx` on Windows via the `fs2` crate.

### In-memory credential cache

Credentials are cached in-memory as `Arc<OAuthCredentials>` to avoid cloning strings on every poll cycle. The cache is invalidated when:
- The token expires (checked before each cycle)
- The usage API returns 401 (token may have been revoked externally)
- Token refresh fails (falls back to re-reading from disk)

## launchd (macOS)

A plist is provided for persistent background operation:

```bash
# Edit to set your device host (IP or hostname)
cp resources/com.claude.usage-daemon.plist ~/Library/LaunchAgents/
vim ~/Library/LaunchAgents/com.claude.usage-daemon.plist

# Load
launchctl load ~/Library/LaunchAgents/com.claude.usage-daemon.plist

# Unload
launchctl unload ~/Library/LaunchAgents/com.claude.usage-daemon.plist
```

Logs: `/tmp/claude-usage-daemon.log` and `/tmp/claude-usage-daemon.err`.

## systemd (Linux)

A systemd user service template is provided:

```bash
# Edit to set your device host
cp resources/claude-usage-daemon.service ~/.config/systemd/user/
vim ~/.config/systemd/user/claude-usage-daemon.service

# Enable and start
systemctl --user daemon-reload
systemctl --user enable --now claude-usage-daemon
```

The setup script (`./setup.sh`) handles this automatically if you choose to enable the startup service.

```bash
# Check status
systemctl --user status claude-usage-daemon

# View logs
journalctl --user -u claude-usage-daemon -f
```

For headless servers (no login session), enable lingering so the service runs without an active session:

```bash
sudo loginctl enable-linger $USER
```

## Architecture

```
src/
  main.rs          CLI args, poll loop, backoff state, daemon/foreground modes
  credentials.rs   Keychain + file reading, token refresh, atomic writes, file locking
  usage.rs         OAuth usage API fetch, typed errors (AuthExpired/Transient)
  push.rs          HTTP push to device, ping health check
```

## Platform Support

| Platform | Credential source | Daemon mode |
|----------|-------------------|-------------|
| macOS | Keychain + file | Yes (`--daemon` or launchd) |
| Linux | File only | Yes (`--daemon` or systemd) |
| Windows | File only | No (run in foreground) |

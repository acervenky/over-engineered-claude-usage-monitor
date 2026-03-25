# How Usage Data Is Retrieved

This project retrieves Claude usage data through two independent methods. Both return the same core data: utilization percentages and reset timestamps for rolling rate-limit windows. Neither method requires the other.

**Important:** Both methods require OAuth authentication. Standard API keys (`sk-ant-...`) cannot access subscription usage data. See [What About API Keys?](#what-about-api-keys) for details on what each auth type can and cannot access.

---

## Method 1: Rust Daemon + OAuth API

The daemon runs as a background process, reads OAuth credentials written by the Claude CLI, polls Anthropic's usage API, and pushes results to a hardware display over LAN.

### Authentication

The Claude CLI writes OAuth tokens on login. The daemon reads them without prompting.

**Credential sources (checked in order):**

| Platform | Primary | Fallback |
|----------|---------|----------|
| macOS | Keychain (service: `Claude Code-credentials`) | `~/.claude/.credentials.json` |
| Linux | -- | `~/.claude/.credentials.json` |
| Windows | -- | `%USERPROFILE%\.claude\.credentials.json` |

Current Claude CLI versions use the plain service name `Claude Code-credentials`. Older versions used a hash-suffixed name: `Claude Code-credentials-{sha256_8hex}` (SHA-256 of the config directory path, first 4 bytes as hex). The daemon tries both automatically. The account name is the current `$USER`.

**Credential JSON format (nested):**

```json
{
  "claudeAiOauth": {
    "accessToken": "...",
    "refreshToken": "...",
    "expiresAt": 1742673600000,
    "scopes": ["..."]
  }
}
```

A flat format (`accessToken`/`refreshToken`/`expiresAt` at root level) is also accepted as a fallback.

**Token refresh:**

When the access token has less than 60 seconds remaining, the daemon refreshes it automatically:

```
POST https://platform.claude.com/v1/oauth/token
Content-Type: application/json

{
  "grant_type": "refresh_token",
  "refresh_token": "<current_refresh_token>",
  "client_id": "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
}
```

The client ID is a public OAuth client (RFC 7636 public client flow). It is not a secret.

Refreshed tokens are written back atomically: write to `.credentials.json.tmp`, set `0600` permissions, rename over the original. An advisory file lock (`.credentials.lock`) prevents races with concurrent Claude CLI processes.

### Data Retrieval

```
GET https://api.anthropic.com/api/oauth/usage
Authorization: Bearer <access_token>
anthropic-beta: oauth-2025-04-20
Accept: application/json
```

**Response:**

```json
{
  "five_hour": {
    "utilization": 45.2,
    "resets_at": "2026-03-22T18:00:00Z"
  },
  "seven_day": {
    "utilization": 12.8,
    "resets_at": "2026-03-25T00:00:00Z"
  },
  "seven_day_opus": {
    "utilization": 8.1,
    "resets_at": "2026-03-25T00:00:00Z"
  },
  "seven_day_sonnet": {
    "utilization": 15.0,
    "resets_at": "2026-03-25T00:00:00Z"
  }
}
```

Each rate window contains:
- `utilization`: float, 0-100 (percentage of limit consumed)
- `resets_at`: ISO 8601 timestamp (when the window resets)

All four windows are optional in the response.

### Data Delivery to Hardware

The daemon flattens the response and pushes it to the device:

```
POST http://<device_ip>:<port>/usage
X-API-Key: <shared_secret>
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

Default poll interval: 300 seconds (5 minutes). The device stores zero credentials; it is a pure display target.

### Error Handling

| Condition | Behavior |
|-----------|----------|
| Auth failure (401/403) | Increment counter. After 3 consecutive failures, slow to 30-min intervals. Log message to re-run `claude` CLI. |
| Transient error (429/5xx/network) | Exponential backoff: 2x, 4x, 8x the base interval, capped at 1 hour. Resets on success. |
| Token expired mid-cycle | Clear cached credentials, force reload from Keychain/file on next cycle. |

### Source Files

- `claude_usage_daemon/src/credentials.rs` -- Keychain/file reading, token refresh, atomic save
- `claude_usage_daemon/src/usage.rs` -- API fetch, response models, payload flattening
- `claude_usage_daemon/src/push.rs` -- HTTP push to device, ping health check
- `claude_usage_daemon/src/main.rs` -- CLI args, poll loop, backoff state, daemonization

---

## Method 2: Chrome Extension + Cookie-Based API

The extension runs entirely in the browser. It discovers the user's organization ID, polls the usage endpoint using session cookies, and displays results as a badge and popup dashboard.

### Authentication

No explicit authentication is needed. The user must be logged into `claude.ai` in Chrome. The browser automatically includes session cookies (`sessionKey`, etc.) on requests to `claude.ai` via `credentials: 'include'`.

### Org ID Discovery

The extension needs the user's organization UUID to construct the usage URL. It discovers this by intercepting API calls:

1. `content.js` injects `injected.js` into the page's main world (not the extension's isolated world). This is necessary because MV3 content scripts cannot intercept `window.fetch` from the isolated context.

2. `injected.js` monkey-patches `window.fetch`:
   ```js
   const _fetch = window.fetch;
   window.fetch = async function (...args) {
     const url = typeof args[0] === 'string' ? args[0] : args[0]?.url ?? '';
     const match = url.match(/\/api\/organizations\/([0-9a-f-]{36})\//);
     if (match?.[1]) {
       window.postMessage({ type: 'CLAUDE_USAGE_ORG_ID', orgId: match[1] }, '*');
     }
     return _fetch.apply(this, args);
   };
   ```

3. `content.js` listens for the `postMessage` and relays the org ID to the background service worker via `chrome.runtime.sendMessage`.

4. The org ID is stored in `chrome.storage.local` and persists across sessions. Once discovered, the interceptor is no longer needed.

### Data Retrieval

```
GET https://claude.ai/api/organizations/<orgId>/usage
(cookies auto-included by browser)
```

The response format matches the OAuth API (same rate window structure). Polled every 5 minutes from `content.js` using `setInterval`.

### Data Storage and Display

The background service worker (`background.js`) processes each usage snapshot:

- **History:** 30 days of snapshots kept in `chrome.storage.local`, older entries pruned.
- **Badge:** Shows 5-hour utilization as a percentage. Color-coded:
  - Green: below goal threshold
  - Amber: at or above goal threshold
  - Red: at or above 90%
- **Popup:** 14-day heatmap, daily averages, reset countdown timers.
- **Sessions:** A new session is detected when 20+ minutes pass between API responses. Session start snapshot is saved for per-session usage tracking.
- **Notifications:** If pace notifications are enabled and utilization hits 90% with more than 1 hour remaining in the window, a Chrome notification fires (once per window).

### Hardware Device Push

The extension can also push usage data directly to hardware devices (ESP8266/ATOMS3R) using the same protocol as the Rust daemon. Configure in Settings > IoT Device Push:

1. Enter the device IP (shown on the device display after boot, e.g., `http://192.168.1.50:8080`)
2. Enter the shared API key (must match `API_KEY` in firmware)
3. Enable "Auto-push usage data on every poll"

On each 5-minute poll, the extension pushes:

```
POST http://<device_ip>:<port>/usage
X-API-Key: <shared_secret>
Content-Type: application/json

{
  "five_hour": 45.2,
  "five_hour_resets_at": "2026-03-22T18:00:00Z",
  "seven_day": 12.8,
  "seven_day_resets_at": "2026-03-25T00:00:00Z",
  "timestamp": 1742673600
}
```

This is the same `POST /usage` endpoint and `X-API-Key` header that the daemon uses. No credentials are sent to the device; only pre-parsed usage data. The extension requires the `http://*/*` host permission to reach LAN devices over plain HTTP.

### Keepalive

MV3 service workers are aggressively unloaded by Chrome. A 24-second alarm (`chrome.alarms.create('keepalive', { periodInMinutes: 0.4 })`) fires continuously to restore the badge after the worker wakes up.

### Source Files

- `injected.js` -- Main-world fetch interception for org ID extraction
- `content.js` -- Org ID relay, usage polling trigger
- `background.js` -- Service worker: storage, badge, notifications, keepalive, device push
- `options.js` -- Settings UI, manual device push button
- `popup.js` -- Heatmap rendering, stats UI

---

## Comparison

| Aspect | Daemon (OAuth) | Extension (Cookies) |
|--------|---------------|-------------------|
| API endpoint | `api.anthropic.com/api/oauth/usage` | `claude.ai/api/organizations/{orgId}/usage` |
| Auth mechanism | Bearer token from CLI OAuth credentials | Browser session cookies (implicit) |
| Token management | Auto-refresh with atomic file writes | Handled by browser cookie jar |
| Prerequisite | Claude CLI logged in (`claude` command) | Logged into `claude.ai` in Chrome |
| Runs where | Background process (macOS/Linux/Windows) | Chrome browser only |
| Output target | Hardware device over LAN HTTP | Extension badge, popup UI, notifications, and hardware device over LAN HTTP |
| Poll interval | 300s default (configurable via `--interval`) | 300s (hardcoded) |
| Offline behavior | Device shows last data, "STALE" after 10 min | Cached snapshots in `chrome.storage.local` |
| Credentials on device | None (stateless push target) | N/A |
| Setup complexity | Daemon binary + hardware device | Load unpacked extension, open `claude.ai` |

---

## What About API Keys?

Neither method in this project works with standard API keys. Here is why, and what each authentication type can actually access.

### Standard API Key (`sk-ant-...`)

**Cannot access subscription usage data.** The `/api/oauth/usage` endpoint requires an OAuth bearer token. The `claude.ai/api/organizations/{orgId}/usage` endpoint requires browser session cookies. A standard API key authenticates against `/v1/messages` and related endpoints only.

**What you do get:** rate-limit response headers on every `/v1/messages` call:

```
anthropic-ratelimit-requests-limit: 50
anthropic-ratelimit-requests-remaining: 49
anthropic-ratelimit-requests-reset: 2026-03-22T18:05:00Z
anthropic-ratelimit-tokens-limit: 40000
anthropic-ratelimit-tokens-remaining: 39000
anthropic-ratelimit-tokens-reset: 2026-03-22T18:01:00Z
anthropic-ratelimit-input-tokens-limit: ...
anthropic-ratelimit-input-tokens-remaining: ...
anthropic-ratelimit-output-tokens-limit: ...
anthropic-ratelimit-output-tokens-remaining: ...
```

These reflect your API tier rate limits (requests per minute, tokens per minute). They are a different concept from the subscription usage windows (5-hour rolling, 7-day rolling) that this project monitors. Token counts in the headers are rounded to the nearest thousand.

### Admin API Key (`sk-ant-admin...`)

Available only to organization admins. Provides historical token consumption and cost data via:

- `GET /v1/organizations/usage_report/messages` -- token breakdown by model, workspace, API key, with 1m/1h/1d granularity
- `GET /v1/organizations/cost_report` -- dollar cost breakdown

This is billing/accounting data (how many tokens were consumed, how much it cost). It does **not** include real-time subscription window utilization (the 5-hour and 7-day percentages this project displays). Data is delayed by approximately 5 minutes.

### OAuth Token (Bearer)

What this project uses. Obtained by running `claude` and completing the interactive login. Grants access to `api.anthropic.com/api/oauth/usage`, which returns the subscription usage windows with real-time utilization percentages and reset timestamps.

### Summary

| Data | Standard API Key | Admin API Key | OAuth Token | Browser Cookies |
|------|-----------------|---------------|-------------|-----------------|
| Subscription usage (5h/7d windows) | No | No | **Yes** | **Yes** |
| Rate-limit headers (per-request) | Yes | Yes | N/A | N/A |
| Historical token consumption | No | Yes | No | No |
| Cost reports | No | Yes | No | No |

To use this project, run `claude` and complete the interactive OAuth login flow. If Claude Code is configured with an API key via `ANTHROPIC_API_KEY`, the OAuth tokens will not be written and the daemon will have nothing to read.

### Related Projects

Other tools that monitor Claude usage face the same constraint:

- **CodexBar** (github.com/steipete/CodexBar) -- macOS menu bar app, uses OAuth/cookies
- **ccusage** (github.com/ryoppippi/ccusage) -- CLI tool for analyzing local usage logs

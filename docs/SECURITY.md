# Security

## Threat model

This project is a **LAN-only usage display**. The ESP device is a stateless
sink that receives usage percentages and renders them on a screen. It stores
no credentials, makes no outbound requests, and has no access to the Claude
API itself.

**TLS is not used** for device communication. The ESP8266 and ESP32-S3
targets lack the RAM and flash budget for a reliable TLS stack on a persistent
HTTP server. All device traffic is plaintext HTTP on the local network. This
is an accepted tradeoff for a display-only device on a home/office LAN.

What an attacker on the same LAN segment can do:

- **Sniff traffic** to see usage percentages (not sensitive).
- **Replay or forge requests** to push fake data to the display (mitigated by
  HMAC auth, see below).

What an attacker **cannot** do:

- Access Claude API credentials (they never leave the daemon or browser).
- Exfiltrate data through the device (it makes no outbound connections).

## HMAC authentication

To prevent replay attacks and avoid sending the raw API key on every request,
the device uses a challenge-response protocol based on HMAC-SHA256.

### Flow

```
Client                          Device
  |                               |
  |  GET /ping                    |
  |------------------------------>|
  |  {"ok":true, "nonce":"ab3f.."}|
  |<------------------------------|
  |                               |
  |  POST /usage                  |
  |  X-Auth-Nonce: ab3f..         |
  |  X-Auth-Signature: <hmac>     |
  |  Body: {"five_hour":42, ...}  |
  |------------------------------>|
  |  {"ok":true}                  |
  |<------------------------------|
```

1. Client calls `GET /ping`. The device returns a random 16-character hex
   nonce. Nonces are stored in a ring buffer of 8 slots, each single-use.

2. Client computes:
   ```
   signature = HMAC-SHA256(api_key, nonce + request_body)
   ```
   and sends the `X-Auth-Nonce` and `X-Auth-Signature` headers with the
   POST request.

3. Device looks up the nonce in its ring buffer. If found, it computes the
   same HMAC, does a constant-time comparison, and invalidates the nonce
   (preventing replay). If the nonce is unknown or the signature mismatches,
   the request is rejected with 401.

### Why this helps

- **The raw API key never crosses the wire.** A passive sniffer sees the
  nonce and signature but cannot derive the key from them.
- **Replay is blocked.** Each nonce is single-use. Replaying a captured
  request with the same nonce returns 401.
- **No TLS dependency.** The scheme works over plaintext HTTP.

### Limitations

- An active MITM on the LAN can intercept the nonce before the legitimate
  client uses it, then race to submit a forged request. This requires an
  active attacker, not just a passive sniffer.
- The nonce pool is 8 slots. If more than 8 pings arrive without a
  corresponding authenticated request, the oldest nonces are overwritten.

## API key management

Both firmware variants ship with a default API key:

```cpp
#define API_KEY  "change-me-to-something-secret"
```

**You must change this before flashing.** If you leave the default, anyone on
your LAN who knows (or guesses) the default can push data to your device.

### Where the key goes

| Component | How to set |
|-----------|-----------|
| Firmware | Edit `API_KEY` in source, recompile and flash |
| Rust daemon | `CLAUDE_DEVICE_API_KEY` env var (preferred) or `--api-key` flag |
| Chrome extension | Settings page, "Device API Key" field |

Use the **environment variable** for the daemon rather than the command-line
flag. Command-line arguments are visible to other users via `ps`.

### Key in Chrome extension

The key is stored in `chrome.storage.local`, which Chrome encrypts at rest as
part of the browser profile. It is never sent to any server other than the
device endpoint you configure.

## Legacy compatibility

Older firmware that does not return a `nonce` field in `/ping` is still
supported. The daemon and extension detect the missing nonce and fall back to
sending the raw `X-API-Key` header. The firmware logs a serial warning when
legacy auth is used:

```
WARN: legacy X-API-Key auth used; upgrade client to HMAC
```

To get HMAC protection, update all three components (firmware, daemon,
extension) to the latest version.

## Chrome extension permissions

The extension's `host_permissions` are restricted to RFC1918 private address
ranges (192.168.x.x, 10.x.x.x, 172.16-31.x.x, localhost) plus
`https://claude.ai`. It cannot make HTTP requests to public internet hosts.

## Unauthenticated endpoints

`GET /ping` requires no authentication. It returns device uptime, a nonce,
and the time since the last push. This is intentional: the daemon uses it for
connectivity checks before it has a nonce to sign with.

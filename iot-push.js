// Shared IoT device push logic (loaded by service worker and options page)

const DEFAULT_DEVICE_API_KEY = 'sup3rs3cr3t';

async function hmacSign(key, message) {
  const enc = new TextEncoder();
  const cryptoKey = await crypto.subtle.importKey(
    'raw', enc.encode(key), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']
  );
  const sig = await crypto.subtle.sign('HMAC', cryptoKey, enc.encode(message));
  return Array.from(new Uint8Array(sig))
    .map(b => b.toString(16).padStart(2, '0')).join('');
}

// Cache whether the device supports HMAC auth (keyed by baseUrl).
// Cleared on auth failure so the next push re-probes.
const _hmacCapable = {};

// Push usage data to hardware device over LAN. Returns the fetch Response.
// Uses HMAC challenge-response when device supports it (/ping returns nonce),
// falling back to X-API-Key for older firmware.
async function pushToDevice(baseUrl, apiKey, payload) {
  const body = JSON.stringify(payload);
  const headers = { 'Content-Type': 'application/json' };

  // Only ping for a nonce when we know (or suspect) the device supports HMAC.
  const shouldTryHmac = _hmacCapable[baseUrl] !== false;

  if (shouldTryHmac) {
    try {
      const pingRes = await fetch(`${baseUrl}/ping`);
      if (pingRes.ok) {
        const ping = await pingRes.json();
        if (ping.nonce) {
          _hmacCapable[baseUrl] = true;
          const sig = await hmacSign(apiKey, ping.nonce + body);
          headers['X-Auth-Nonce'] = ping.nonce;
          headers['X-Auth-Signature'] = sig;
        } else {
          _hmacCapable[baseUrl] = false;
          headers['X-API-Key'] = apiKey;
        }
      } else {
        _hmacCapable[baseUrl] = false;
        headers['X-API-Key'] = apiKey;
      }
    } catch (err) {
      if (err instanceof TypeError) throw err;
      _hmacCapable[baseUrl] = false;
      headers['X-API-Key'] = apiKey;
    }
  } else {
    headers['X-API-Key'] = apiKey;
  }

  const res = await fetch(`${baseUrl}/usage`, { method: 'POST', headers, body });
  // Re-probe on auth failure in case firmware was updated.
  if (res.status === 401) delete _hmacCapable[baseUrl];
  return res;
}

function buildDevicePayload(snapshot) {
  return {
    five_hour: snapshot.five_hour ?? 0,
    five_hour_resets_at: snapshot.five_hour_resets_at ?? undefined,
    seven_day: snapshot.seven_day ?? 0,
    seven_day_resets_at: snapshot.seven_day_resets_at ?? undefined,
    timestamp: Math.floor((snapshot.timestamp || Date.now()) / 1000),
  };
}

function normalizeEndpoint(endpoint) {
  return endpoint.startsWith('http') ? endpoint : `http://${endpoint}`;
}

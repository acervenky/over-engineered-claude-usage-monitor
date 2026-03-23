use crate::usage::DevicePayload;
use hmac::{Hmac, Mac};
use sha2::Sha256;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

type HmacSha256 = Hmac<Sha256>;

/// Cached: does the device support HMAC auth? Avoids a /ping round-trip on every push.
/// Reset to true on auth failure so the next push re-probes.
static DEVICE_SUPPORTS_HMAC: AtomicBool = AtomicBool::new(true);

#[derive(Debug)]
pub enum PushError {
    /// Device rejected the API key (401).
    AuthRejected(String),
    /// Network timeout, connection refused, or device returned an error.
    DeviceOffline(String),
    /// Non-auth HTTP error from the device.
    Other(String),
}

impl std::fmt::Display for PushError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::AuthRejected(s) => write!(f, "auth rejected: {s}"),
            Self::DeviceOffline(s) => write!(f, "device offline: {s}"),
            Self::Other(s) => write!(f, "push error: {s}"),
        }
    }
}

#[derive(Debug, serde::Deserialize)]
struct PingResponse {
    #[allow(dead_code)]
    ok: bool,
    nonce: Option<String>,
}

/// Compute HMAC-SHA256(key, nonce + body) and return hex-encoded signature.
fn compute_hmac_signature(key: &str, nonce: &str, body: &str) -> String {
    let mut mac =
        HmacSha256::new_from_slice(key.as_bytes()).expect("HMAC accepts any key length");
    mac.update(nonce.as_bytes());
    mac.update(body.as_bytes());
    hex::encode(mac.finalize().into_bytes())
}

/// Push usage data to the hardware device.
/// Uses HMAC-SHA256 challenge-response auth when the device supports it (nonce
/// in /ping response), falling back to legacy X-API-Key for older firmware.
/// Plaintext HTTP because the target is a LAN-only ESP/IoT device without TLS.
pub async fn push_to_device(
    http: &reqwest::Client,
    device_host: &str,
    device_port: u16,
    api_key: &str,
    payload: &DevicePayload,
) -> Result<(), PushError> {
    let url = format!("http://{device_host}:{device_port}/usage");
    let body = serde_json::to_string(payload)
        .map_err(|e| PushError::Other(format!("JSON serialize failed: {e}")))?;

    // Only ping for a nonce when we expect the device supports HMAC.
    let nonce = if DEVICE_SUPPORTS_HMAC.load(Ordering::Relaxed) {
        match ping_device(http, device_host, device_port).await {
            Ok(Some(n)) => {
                DEVICE_SUPPORTS_HMAC.store(true, Ordering::Relaxed);
                Some(n)
            }
            Ok(None) => {
                DEVICE_SUPPORTS_HMAC.store(false, Ordering::Relaxed);
                None
            }
            Err(e) => {
                log::debug!("Ping failed ({e}), falling back to legacy auth");
                DEVICE_SUPPORTS_HMAC.store(false, Ordering::Relaxed);
                None
            }
        }
    } else {
        None
    };

    let mut req = http
        .post(&url)
        .header("Content-Type", "application/json")
        .timeout(Duration::from_secs(5));

    if let Some(nonce) = &nonce {
        let sig = compute_hmac_signature(api_key, nonce, &body);
        req = req
            .header("X-Auth-Nonce", nonce.as_str())
            .header("X-Auth-Signature", &sig);
        log::debug!("Using HMAC auth with nonce {nonce}");
    } else {
        req = req.header("X-API-Key", api_key);
        log::debug!("Using legacy X-API-Key auth");
    }

    let resp = req
        .body(body)
        .send()
        .await
        .map_err(|e| PushError::DeviceOffline(format!("Device push failed: {e}")))?;

    let status = resp.status();
    if !status.is_success() {
        let body = resp.text().await.unwrap_or_default();
        if status.as_u16() == 401 {
            // Re-probe on next push in case firmware was updated.
            DEVICE_SUPPORTS_HMAC.store(true, Ordering::Relaxed);
            return Err(PushError::AuthRejected(
                "Device rejected auth (401). Check --api-key matches firmware API_KEY.".into(),
            ));
        }
        return Err(PushError::Other(format!(
            "Device returned {status}: {body}"
        )));
    }

    Ok(())
}

/// Lightweight health check that also retrieves the HMAC nonce.
/// Returns the nonce if the device supports HMAC auth, None otherwise.
/// Uses plaintext HTTP (LAN-only IoT device).
pub async fn ping_device(
    http: &reqwest::Client,
    device_host: &str,
    device_port: u16,
) -> Result<Option<String>, PushError> {
    let url = format!("http://{device_host}:{device_port}/ping");

    let resp = http
        .get(&url)
        .timeout(Duration::from_secs(3))
        .send()
        .await
        .map_err(|e| PushError::DeviceOffline(format!("Device unreachable: {e}")))?;

    if !resp.status().is_success() {
        return Err(PushError::Other(format!(
            "Device ping returned {}",
            resp.status()
        )));
    }

    let ping: PingResponse = resp
        .json()
        .await
        .map_err(|e| PushError::Other(format!("Bad ping response: {e}")))?;

    Ok(ping.nonce)
}

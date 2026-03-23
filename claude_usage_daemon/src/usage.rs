use serde::{Deserialize, Serialize};

const USAGE_URL: &str = "https://api.anthropic.com/api/oauth/usage";
const BETA_HEADER: &str = "oauth-2025-04-20";

/// Individual rate window from the API response.
#[derive(Debug, Clone, Deserialize)]
pub struct RateWindow {
    pub utilization: Option<f64>,
    pub resets_at: Option<String>,
}

/// Raw API response from /api/oauth/usage.
#[derive(Debug, Clone, Deserialize)]
pub struct UsageResponse {
    pub five_hour: Option<RateWindow>,
    pub seven_day: Option<RateWindow>,
    pub seven_day_opus: Option<RateWindow>,
    pub seven_day_sonnet: Option<RateWindow>,
}

/// Flattened payload pushed to the device.
#[derive(Debug, Clone, Serialize)]
pub struct DevicePayload {
    pub five_hour: f64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub five_hour_resets_at: Option<String>,
    pub seven_day: f64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub seven_day_resets_at: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub seven_day_opus: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub seven_day_sonnet: Option<f64>,
    pub timestamp: u64,
}

#[derive(Debug)]
pub enum UsageError {
    /// 401/403: token expired or revoked, caller should refresh.
    AuthExpired(String),
    /// 429, 5xx, network errors: transient, caller should backoff.
    Transient(String),
}

impl std::fmt::Display for UsageError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::AuthExpired(s) => write!(f, "auth expired: {s}"),
            Self::Transient(s) => write!(f, "transient: {s}"),
        }
    }
}

impl UsageResponse {
    /// Convert API response into the flat device payload.
    pub fn to_device_payload(&self) -> DevicePayload {
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();

        DevicePayload {
            five_hour: self
                .five_hour
                .as_ref()
                .and_then(|w| w.utilization)
                .unwrap_or(0.0),
            five_hour_resets_at: self
                .five_hour
                .as_ref()
                .and_then(|w| w.resets_at.clone()),
            seven_day: self
                .seven_day
                .as_ref()
                .and_then(|w| w.utilization)
                .unwrap_or(0.0),
            seven_day_resets_at: self
                .seven_day
                .as_ref()
                .and_then(|w| w.resets_at.clone()),
            seven_day_opus: self
                .seven_day_opus
                .as_ref()
                .and_then(|w| w.utilization),
            seven_day_sonnet: self
                .seven_day_sonnet
                .as_ref()
                .and_then(|w| w.utilization),
            timestamp: now,
        }
    }
}

/// Fetch usage data from the OAuth API.
pub async fn fetch_usage(
    http: &reqwest::Client,
    access_token: &str,
) -> Result<UsageResponse, UsageError> {
    let resp = http
        .get(USAGE_URL)
        .bearer_auth(access_token)
        .header("anthropic-beta", BETA_HEADER)
        .header("Accept", "application/json")
        .send()
        .await
        .map_err(|e| UsageError::Transient(format!("Usage request failed: {e}")))?;

    let status = resp.status();
    if !status.is_success() {
        let body = resp.text().await.unwrap_or_default();
        if status.as_u16() == 401 || status.as_u16() == 403 {
            return Err(UsageError::AuthExpired(
                format!("Usage API returned {status}: {body}")
            ));
        }
        return Err(UsageError::Transient(
            format!("Usage API returned {status}: {body}")
        ));
    }

    resp.json::<UsageResponse>()
        .await
        .map_err(|e| UsageError::Transient(format!("Failed to parse usage response: {e}")))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_to_device_payload_full() {
        let resp = UsageResponse {
            five_hour: Some(RateWindow {
                utilization: Some(45.2),
                resets_at: Some("2026-03-22T18:00:00Z".into()),
            }),
            seven_day: Some(RateWindow {
                utilization: Some(12.8),
                resets_at: Some("2026-03-25T00:00:00Z".into()),
            }),
            seven_day_opus: Some(RateWindow {
                utilization: Some(8.1),
                resets_at: None,
            }),
            seven_day_sonnet: Some(RateWindow {
                utilization: Some(15.0),
                resets_at: None,
            }),
        };

        let payload = resp.to_device_payload();
        assert!((payload.five_hour - 45.2).abs() < f64::EPSILON);
        assert_eq!(payload.five_hour_resets_at.as_deref(), Some("2026-03-22T18:00:00Z"));
        assert!((payload.seven_day - 12.8).abs() < f64::EPSILON);
        assert_eq!(payload.seven_day_resets_at.as_deref(), Some("2026-03-25T00:00:00Z"));
        assert!((payload.seven_day_opus.unwrap() - 8.1).abs() < f64::EPSILON);
        assert!((payload.seven_day_sonnet.unwrap() - 15.0).abs() < f64::EPSILON);
    }

    #[test]
    fn test_to_device_payload_partial() {
        // Missing windows should default to 0.0 / None
        let resp = UsageResponse {
            five_hour: None,
            seven_day: Some(RateWindow {
                utilization: None,
                resets_at: None,
            }),
            seven_day_opus: None,
            seven_day_sonnet: None,
        };

        let payload = resp.to_device_payload();
        assert!((payload.five_hour - 0.0).abs() < f64::EPSILON);
        assert!(payload.five_hour_resets_at.is_none());
        assert!((payload.seven_day - 0.0).abs() < f64::EPSILON);
        assert!(payload.seven_day_resets_at.is_none());
        assert!(payload.seven_day_opus.is_none());
        assert!(payload.seven_day_sonnet.is_none());
    }

    #[test]
    fn test_to_device_payload_timestamp() {
        let resp = UsageResponse {
            five_hour: None,
            seven_day: None,
            seven_day_opus: None,
            seven_day_sonnet: None,
        };

        let before = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_secs();
        let payload = resp.to_device_payload();
        let after = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_secs();

        assert!(payload.timestamp >= before);
        assert!(payload.timestamp <= after);
    }

    #[test]
    fn test_deserialize_api_response() {
        let json = r#"{
            "five_hour": { "utilization": 33.3, "resets_at": "2026-03-22T20:00:00Z" },
            "seven_day": { "utilization": 5.0 }
        }"#;
        let resp: UsageResponse = serde_json::from_str(json).unwrap();
        assert!((resp.five_hour.as_ref().unwrap().utilization.unwrap() - 33.3).abs() < f64::EPSILON);
        assert_eq!(resp.five_hour.as_ref().unwrap().resets_at.as_deref(), Some("2026-03-22T20:00:00Z"));
        assert!(resp.seven_day.as_ref().unwrap().resets_at.is_none());
        assert!(resp.seven_day_opus.is_none());
    }
}

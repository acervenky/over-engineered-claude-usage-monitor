#[cfg(target_os = "macos")]
use security_framework::passwords::{get_generic_password, set_generic_password};
use sha2::{Digest, Sha256};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

/// Token refresh threshold: refresh when less than 60 seconds remain.
const REFRESH_THRESHOLD_MS: u64 = 60_000;

const TOKEN_URL: &str = "https://platform.claude.com/v1/oauth/token";

/// OAuth public client ID. Not a secret (RFC 7636 public client flow).
const CLIENT_ID: &str = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";

/// Lock retry config matching Claude CLI behavior.
const LOCK_MAX_RETRIES: u32 = 5;
const LOCK_BACKOFF_MIN_MS: u64 = 1000;
const LOCK_BACKOFF_MAX_MS: u64 = 2000;

/// Where credentials were loaded from, so we save back to the same place.
#[derive(Clone, Debug, PartialEq)]
pub enum CredentialSource {
    Keychain,
    File,
}

#[derive(Clone, serde::Deserialize, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct OAuthCredentials {
    pub access_token: String,
    pub refresh_token: Option<String>,
    /// Milliseconds since epoch.
    pub expires_at: Option<u64>,
    pub scopes: Option<Vec<String>>,
    /// Where these credentials were loaded from (not serialized).
    #[serde(skip)]
    pub source: Option<CredentialSource>,
}

/// Redact token values to prevent accidental leaks in logs.
impl std::fmt::Debug for OAuthCredentials {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("OAuthCredentials")
            .field("access_token", &"[redacted]")
            .field("refresh_token", &self.refresh_token.as_ref().map(|_| "[redacted]"))
            .field("expires_at", &self.expires_at)
            .field("scopes", &self.scopes)
            .finish()
    }
}

/// Top-level wrapper matching the credentials file / keychain JSON format.
#[derive(Debug, serde::Deserialize, serde::Serialize)]
#[serde(rename_all = "camelCase")]
struct CredentialsFile {
    claude_ai_oauth: Option<OAuthCredentials>,
    // Flat format fallback fields.
    access_token: Option<String>,
    refresh_token: Option<String>,
    expires_at: Option<u64>,
}

/// Error types for credential operations.
#[derive(Debug)]
pub enum CredentialError {
    /// Credentials not found (file missing, keychain empty).
    NotFound(String),
    /// Token is expired and refresh failed with a permanent error (401/403).
    AuthRevoked(String),
    /// Transient error (network, I/O, lock contention exhausted).
    Transient(String),
}

impl std::fmt::Display for CredentialError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::NotFound(s) => write!(f, "credentials not found: {s}"),
            Self::AuthRevoked(s) => write!(f, "auth revoked: {s}"),
            Self::Transient(s) => write!(f, "transient error: {s}"),
        }
    }
}

impl OAuthCredentials {
    pub fn is_expired(&self) -> bool {
        let now_ms = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis() as u64;
        match self.expires_at {
            Some(exp) => now_ms + REFRESH_THRESHOLD_MS >= exp,
            None => false,
        }
    }
}

/// Parse credentials JSON, handling both nested and flat formats.
fn parse_credentials(json: &str) -> Result<OAuthCredentials, String> {
    let file: CredentialsFile =
        serde_json::from_str(json).map_err(|e| format!("JSON parse error: {e}"))?;

    if let Some(creds) = file.claude_ai_oauth {
        return Ok(creds);
    }

    if let Some(token) = file.access_token {
        return Ok(OAuthCredentials {
            access_token: token,
            refresh_token: file.refresh_token,
            expires_at: file.expires_at,
            scopes: None,
            source: None,
        });
    }

    Err("No OAuth credentials found in JSON".into())
}

/// Build the legacy keychain service name (hash-suffixed, older CLI versions).
fn keychain_service_name_legacy(config_dir: &Path) -> String {
    let dir_str = config_dir.to_string_lossy();
    let hash = Sha256::digest(dir_str.as_bytes());
    let hash8 = hex::encode(&hash[..4]);
    format!("Claude Code-credentials-{hash8}")
}

/// Current CLI uses plain service name without hash suffix.
const KEYCHAIN_SERVICE: &str = "Claude Code-credentials";

#[cfg(target_os = "macos")]
fn read_from_keychain(config_dir: &Path) -> Result<OAuthCredentials, String> {
    let account = std::env::var("USER").unwrap_or_else(|_| "claude-code-user".into());

    // Try current format first: "Claude Code-credentials" with $USER account.
    log::debug!("Reading keychain: service={KEYCHAIN_SERVICE}, account={account}");
    match get_generic_password(KEYCHAIN_SERVICE, &account) {
        Ok(bytes) => {
            let json = String::from_utf8(bytes.to_vec())
                .map_err(|e| format!("Keychain value is not UTF-8: {e}"))?;
            return parse_credentials(&json);
        }
        Err(e) => {
            log::debug!("Keychain lookup failed for {KEYCHAIN_SERVICE}: {e}");
        }
    }

    // Fall back to legacy hash-suffixed format for older CLI versions.
    let legacy_service = keychain_service_name_legacy(config_dir);
    log::debug!("Trying legacy keychain: service={legacy_service}, account={account}");
    let bytes = get_generic_password(&legacy_service, &account)
        .map_err(|e| format!("Keychain read failed (tried {KEYCHAIN_SERVICE} and {legacy_service}): {e}"))?;

    let json = String::from_utf8(bytes.to_vec())
        .map_err(|e| format!("Keychain value is not UTF-8: {e}"))?;

    parse_credentials(&json)
}

#[cfg(not(target_os = "macos"))]
fn read_from_keychain(_config_dir: &Path) -> Result<OAuthCredentials, String> {
    Err("Keychain not available on this platform".into())
}

fn read_from_file(config_dir: &Path) -> Result<OAuthCredentials, String> {
    let path = config_dir.join(".credentials.json");
    log::debug!("Reading credentials file: {}", path.display());

    let json = std::fs::read_to_string(&path)
        .map_err(|e| format!("Cannot read {}: {e}", path.display()))?;

    parse_credentials(&json)
}

/// Load credentials, trying Keychain first (macOS), then file fallback.
pub fn load_credentials(config_dir: &Path) -> Result<OAuthCredentials, CredentialError> {
    match read_from_keychain(config_dir) {
        Ok(mut creds) => {
            log::info!("Loaded credentials from Keychain");
            creds.source = Some(CredentialSource::Keychain);
            return Ok(creds);
        }
        Err(e) => {
            log::warn!("Keychain read failed: {e}");
        }
    }

    match read_from_file(config_dir) {
        Ok(mut creds) => {
            log::info!("Loaded credentials from file");
            creds.source = Some(CredentialSource::File);
            Ok(creds)
        }
        Err(e) => {
            log::warn!("Credentials file failed: {e}");
            Err(CredentialError::NotFound(format!(
                "No credentials found. Tried macOS Keychain and {}/{}. Run `claude` to log in.",
                config_dir.display(),
                ".credentials.json"
            )))
        }
    }
}

#[derive(Debug, serde::Deserialize)]
struct TokenRefreshResponse {
    access_token: String,
    refresh_token: Option<String>,
    expires_in: Option<u64>,
}

/// Refresh the OAuth token and return updated credentials.
pub async fn refresh_token(
    creds: &OAuthCredentials,
    config_dir: &Path,
    http: &reqwest::Client,
) -> Result<OAuthCredentials, CredentialError> {
    let refresh_token = creds
        .refresh_token
        .as_deref()
        .ok_or_else(|| CredentialError::AuthRevoked("No refresh token available".into()))?;

    log::info!("Refreshing OAuth token");

    let body = serde_json::json!({
        "grant_type": "refresh_token",
        "refresh_token": refresh_token,
        "client_id": CLIENT_ID,
    });

    let resp = http
        .post(TOKEN_URL)
        .json(&body)
        .send()
        .await
        .map_err(|e| CredentialError::Transient(format!("Refresh request failed: {e}")))?;

    let status = resp.status();
    if !status.is_success() {
        let body_text = resp.text().await.unwrap_or_default();
        // 401/403 = token permanently revoked. 429/5xx = transient.
        if status.as_u16() == 401 || status.as_u16() == 403 {
            return Err(CredentialError::AuthRevoked(
                format!("Token refresh returned {status}: {body_text}")
            ));
        }
        return Err(CredentialError::Transient(
            format!("Token refresh returned {status}: {body_text}")
        ));
    }

    let token_resp: TokenRefreshResponse = resp
        .json()
        .await
        .map_err(|e| CredentialError::Transient(format!("Failed to parse refresh response: {e}")))?;

    let now_ms = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64;

    let new_creds = OAuthCredentials {
        access_token: token_resp.access_token,
        refresh_token: token_resp
            .refresh_token
            .or_else(|| creds.refresh_token.clone()),
        expires_at: token_resp.expires_in.map(|secs| now_ms + secs * 1000),
        scopes: creds.scopes.clone(),
        source: creds.source.clone(),
    };

    let source = creds.source.as_ref().unwrap_or(&CredentialSource::File);
    if let Err(e) = save_credentials(&new_creds, config_dir, source).await {
        log::warn!("Failed to persist refreshed credentials: {e}");
    }

    Ok(new_creds)
}

/// Save refreshed credentials back to the source they were loaded from.
async fn save_credentials(
    creds: &OAuthCredentials,
    config_dir: &Path,
    source: &CredentialSource,
) -> Result<(), String> {
    match source {
        CredentialSource::Keychain => save_to_keychain(creds),
        CredentialSource::File => save_to_file(creds, config_dir).await,
    }
}

#[cfg(target_os = "macos")]
fn save_to_keychain(creds: &OAuthCredentials) -> Result<(), String> {
    let account = std::env::var("USER").unwrap_or_else(|_| "claude-code-user".into());
    let wrapper = serde_json::json!({ "claudeAiOauth": creds });
    let json = serde_json::to_string(&wrapper)
        .map_err(|e| format!("JSON serialize error: {e}"))?;

    set_generic_password(KEYCHAIN_SERVICE, &account, json.as_bytes())
        .map_err(|e| format!("Keychain write failed: {e}"))?;

    log::debug!("Saved refreshed credentials to Keychain");
    Ok(())
}

#[cfg(not(target_os = "macos"))]
fn save_to_keychain(_creds: &OAuthCredentials) -> Result<(), String> {
    Err("Keychain not available on this platform".into())
}

async fn save_to_file(creds: &OAuthCredentials, config_dir: &Path) -> Result<(), String> {
    use fs2::FileExt;
    use rand::Rng;

    let path = config_dir.join(".credentials.json");
    let lock_path = config_dir.join(".credentials.lock");
    let tmp_path = config_dir.join(".credentials.json.tmp");

    // Acquire exclusive lock with retry.
    let lock_file = std::fs::OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .open(&lock_path)
        .map_err(|e| format!("Cannot open lock file: {e}"))?;

    let mut locked = false;
    for attempt in 0..LOCK_MAX_RETRIES {
        match lock_file.try_lock_exclusive() {
            Ok(()) => {
                locked = true;
                break;
            }
            Err(_) if attempt < LOCK_MAX_RETRIES - 1 => {
                let backoff = rand::thread_rng()
                    .gen_range(LOCK_BACKOFF_MIN_MS..=LOCK_BACKOFF_MAX_MS);
                log::debug!("Lock contention, retry {}/{} in {backoff}ms",
                    attempt + 1, LOCK_MAX_RETRIES);
                tokio::time::sleep(std::time::Duration::from_millis(backoff)).await;
            }
            Err(e) => {
                return Err(format!("Failed to acquire lock after {LOCK_MAX_RETRIES} attempts: {e}"));
            }
        }
    }

    if !locked {
        return Err("Failed to acquire credentials lock".into());
    }

    // Serialize and write to temp file.
    let wrapper = serde_json::json!({
        "claudeAiOauth": creds,
    });
    let json = serde_json::to_string_pretty(&wrapper)
        .map_err(|e| format!("JSON serialize error: {e}"))?;

    std::fs::write(&tmp_path, &json).map_err(|e| format!("Write to temp failed: {e}"))?;

    // Set permissions before rename so the file is never world-readable.
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let perms = std::fs::Permissions::from_mode(0o600);
        std::fs::set_permissions(&tmp_path, perms)
            .map_err(|e| format!("chmod failed: {e}"))?;
    }

    // Atomic rename (same filesystem).
    std::fs::rename(&tmp_path, &path).map_err(|e| format!("Atomic rename failed: {e}"))?;

    // Lock is released when lock_file is dropped.
    drop(lock_file);

    log::debug!("Saved refreshed credentials to {}", path.display());
    Ok(())
}

/// Resolve the config directory, expanding ~ if needed.
pub fn resolve_config_dir(config_dir: Option<&str>) -> PathBuf {
    if let Some(dir) = config_dir {
        if dir.starts_with('~') {
            if let Some(home) = dirs_home() {
                if dir == "~" {
                    return home;
                } else if let Some(rest) = dir.strip_prefix("~/") {
                    return home.join(rest);
                }
            }
        }
        return PathBuf::from(dir);
    }

    std::env::var("CLAUDE_CONFIG_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| dirs_home().unwrap_or_default().join(".claude"))
}

fn dirs_home() -> Option<PathBuf> {
    #[cfg(unix)]
    {
        std::env::var("HOME").ok().map(PathBuf::from)
    }
    #[cfg(windows)]
    {
        std::env::var("USERPROFILE").ok().map(PathBuf::from)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_nested_format() {
        let json = r#"{
            "claudeAiOauth": {
                "accessToken": "tok_abc",
                "refreshToken": "ref_xyz",
                "expiresAt": 9999999999999,
                "scopes": ["read"]
            }
        }"#;
        let creds = parse_credentials(json).unwrap();
        assert_eq!(creds.access_token, "tok_abc");
        assert_eq!(creds.refresh_token.as_deref(), Some("ref_xyz"));
        assert_eq!(creds.expires_at, Some(9999999999999));
        assert_eq!(creds.scopes, Some(vec!["read".to_string()]));
    }

    #[test]
    fn test_parse_flat_format() {
        let json = r#"{
            "accessToken": "tok_flat",
            "refreshToken": "ref_flat",
            "expiresAt": 1000000000000
        }"#;
        let creds = parse_credentials(json).unwrap();
        assert_eq!(creds.access_token, "tok_flat");
        assert_eq!(creds.refresh_token.as_deref(), Some("ref_flat"));
        assert_eq!(creds.expires_at, Some(1000000000000));
    }

    #[test]
    fn test_parse_missing_fields() {
        // Empty object: no accessToken, no claudeAiOauth
        let result = parse_credentials("{}");
        assert!(result.is_err());

        // Invalid JSON
        let result = parse_credentials("not json");
        assert!(result.is_err());
    }

    #[test]
    fn test_is_expired_past() {
        let creds = OAuthCredentials {
            access_token: "tok".into(),
            refresh_token: None,
            expires_at: Some(0), // epoch = long past
            scopes: None,
            source: None,
        };
        assert!(creds.is_expired());
    }

    #[test]
    fn test_is_expired_none() {
        // Legacy credentials may omit expiresAt. Keep using the token until
        // the API rejects it, instead of refreshing every poll tick.
        let creds = OAuthCredentials {
            access_token: "tok".into(),
            refresh_token: None,
            expires_at: None,
            scopes: None,
            source: None,
        };
        assert!(!creds.is_expired());
    }

    #[test]
    fn test_is_not_expired() {
        let future_ms = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_millis() as u64
            + 3_600_000; // 1 hour from now
        let creds = OAuthCredentials {
            access_token: "tok".into(),
            refresh_token: None,
            expires_at: Some(future_ms),
            scopes: None,
            source: None,
        };
        assert!(!creds.is_expired());
    }

    /// Validate that the daemon can read credentials from the real macOS Keychain
    /// or credentials file. This test is ignored by default (requires local
    /// Claude CLI login) and will not run in CI.
    ///
    /// Run with: cargo test -- --ignored
    #[test]
    #[ignore]
    #[cfg(target_os = "macos")]
    fn test_keychain_read_macos() {
        let config_dir = resolve_config_dir(None);
        match load_credentials(&config_dir) {
            Ok(creds) => {
                // Token loaded; verify it has a non-empty access token.
                assert!(!creds.access_token.is_empty());
            }
            Err(CredentialError::NotFound(msg)) => {
                // No credentials on this machine. That's fine.
                eprintln!("Keychain test: credentials not found ({msg}). Run `claude` to log in.");
            }
            Err(e) => {
                panic!("Unexpected credential error: {e}");
            }
        }
    }
}

mod credentials;
mod push;
mod usage;

use clap::Parser;
use credentials::CredentialError;
use std::sync::Arc;
use std::time::Duration;
use tokio::time;
use usage::UsageError;

/// Max consecutive auth failures before slowing down.
const AUTH_FAIL_SLOWDOWN: u32 = 3;
/// Interval when in auth-failed slowdown mode (30 minutes).
const SLOWDOWN_INTERVAL: Duration = Duration::from_secs(30 * 60);
/// Maximum backoff for transient errors (1 hour).
const MAX_BACKOFF: Duration = Duration::from_secs(3600);

/// Validate that the input is a valid IP address or hostname.
/// Accepts IPv4, IPv6, or RFC 952/1123 hostnames.
fn parse_host(s: &str) -> Result<String, String> {
    let s = s.trim();
    if s.is_empty() {
        return Err("host must not be empty".into());
    }
    // Accept any valid IP address as-is.
    if s.parse::<std::net::IpAddr>().is_ok() {
        return Ok(s.to_string());
    }
    // Reject strings that look like IPs but failed parsing (e.g., 999.999.999.999).
    let looks_like_ip = s.chars().all(|c| c.is_ascii_digit() || c == '.');
    if looks_like_ip {
        return Err(format!("'{s}' looks like an IP address but is not valid"));
    }
    // Hostname: RFC 1123 allows labels of [a-zA-Z0-9-], max 253 chars total.
    if s.len() > 253 {
        return Err(format!("hostname too long ({} chars, max 253)", s.len()));
    }
    let valid = s.split('.').all(|label| {
        !label.is_empty()
            && label.len() <= 63
            && !label.starts_with('-')
            && !label.ends_with('-')
            && label.chars().all(|c| c.is_ascii_alphanumeric() || c == '-')
    });
    if !valid {
        return Err(format!("'{s}' is not a valid IP address or hostname"));
    }
    Ok(s.to_string())
}

#[derive(Parser)]
#[command(name = "claude-usage-daemon")]
#[command(about = "Reads Claude OAuth credentials and pushes usage data to IoT display devices")]
struct Args {
    /// Device IP address or hostname (e.g., 192.168.1.50, my-device.local)
    #[arg(short = 'H', long, value_parser = parse_host)]
    device_host: String,

    /// Port the device listens on (matches firmware HTTP_PORT)
    #[arg(short = 'p', long, default_value = "8080",
          value_parser = clap::value_parser!(u16).range(1..=65535))]
    device_port: u16,

    /// Shared API key for device authentication (must match firmware API_KEY).
    /// Override with --api-key or CLAUDE_DEVICE_API_KEY env var.
    #[arg(short = 'k', long, default_value = "sup3rs3cr3t", env = "CLAUDE_DEVICE_API_KEY")]
    api_key: String,

    /// Poll interval in seconds (minimum 10)
    #[arg(short = 'n', long, default_value = "300",
          value_parser = clap::value_parser!(u64).range(10..))]
    interval: u64,

    /// Override Claude config directory (default: ~/.claude)
    #[arg(short = 'c', long)]
    config_dir: Option<String>,

    /// Run as a background daemon (Unix only). Foreground is default.
    #[arg(short = 'd', long)]
    daemon: bool,

    /// PID file path (used with --daemon)
    #[arg(short = 'f', long)]
    pid_file: Option<String>,
}

impl Args {
    fn validate(&self) {
        if self.pid_file.is_some() && !self.daemon {
            eprintln!("warning: --pid-file has no effect without --daemon");
        }
    }
}

fn main() {
    let args = Args::parse();
    args.validate();

    if args.daemon {
        #[cfg(unix)]
        {
            daemonize(&args);
        }
        #[cfg(not(unix))]
        {
            eprintln!("error: --daemon is only supported on Unix (macOS/Linux)");
            std::process::exit(1);
        }
    }

    let default_level = if args.daemon { "warn" } else { "info" };
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or(default_level))
        .init();

    run_async(args);
}

#[cfg(unix)]
fn daemonize(args: &Args) {
    #[allow(deprecated)]
    let result = unsafe { libc::daemon(0, 0) };
    if result != 0 {
        eprintln!("error: failed to daemonize: {}", std::io::Error::last_os_error());
        std::process::exit(1);
    }

    let default_pid_path = default_pid_path();
    let pid_path = args
        .pid_file
        .as_deref()
        .unwrap_or(&default_pid_path);
    if let Err(e) = write_pid_file(pid_path) {
        eprintln!("warning: failed to write PID file {pid_path}: {e}");
    }
}

/// Prefer $TMPDIR (macOS) or $XDG_RUNTIME_DIR (Linux) over /tmp to avoid
/// writing into a world-writable directory. If forced to fall back to /tmp,
/// create a private subdirectory keyed on our UID to prevent symlink attacks.
#[cfg(unix)]
fn default_pid_path() -> String {
    for var in ["TMPDIR", "XDG_RUNTIME_DIR"] {
        if let Ok(dir) = std::env::var(var) {
            if !dir.is_empty() {
                let mut p = std::path::PathBuf::from(dir);
                p.push("claude-usage-daemon.pid");
                return p.to_string_lossy().into_owned();
            }
        }
    }

    // /tmp fallback: use a private subdirectory to avoid TOCTOU races.
    let uid = unsafe { libc::getuid() };
    let dir = format!("/tmp/claude-usage-daemon-{uid}");
    let dir_path = std::path::Path::new(&dir);

    if !dir_path.exists() {
        if let Err(e) = std::fs::create_dir(&dir) {
            if e.kind() != std::io::ErrorKind::AlreadyExists {
                eprintln!("warning: cannot create PID directory {dir}: {e}");
            }
        }
    }

    // Set mode 0700 regardless of umask.
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o700));
    }

    // Refuse to use a directory owned by another user.
    {
        use std::os::unix::fs::MetadataExt;
        if let Ok(meta) = std::fs::metadata(&dir) {
            if meta.uid() != uid {
                eprintln!(
                    "warning: PID directory {dir} owned by uid {}, expected {uid}; \
                     set $TMPDIR or $XDG_RUNTIME_DIR to a private directory",
                    meta.uid()
                );
            }
        }
    }

    format!("{dir}/claude-usage-daemon.pid")
}

/// Write PID file using O_CREAT|O_EXCL to refuse to follow symlinks.
#[cfg(unix)]
fn write_pid_file(path: &str) -> std::io::Result<()> {
    use std::io::Write;
    use std::os::unix::fs::OpenOptionsExt;

    // Remove stale PID file from a previous run (only if it is a regular file).
    let p = std::path::Path::new(path);
    if p.symlink_metadata().map(|m| m.file_type().is_file()).unwrap_or(false) {
        let _ = std::fs::remove_file(p);
    }

    let mut f = std::fs::OpenOptions::new()
        .write(true)
        .create_new(true) // O_EXCL: fail if path exists (including symlinks)
        .mode(0o644)
        .open(path)?;
    write!(f, "{}", std::process::id())
}

fn run_async(args: Args) {
    let rt = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .expect("Failed to create tokio runtime");

    rt.block_on(run(args));
}

/// Tracks backoff and failure state across poll cycles.
struct PollState {
    cached_creds: Option<Arc<credentials::OAuthCredentials>>,
    /// Consecutive auth failure count.
    auth_failures: u32,
    /// Current backoff multiplier for transient errors (resets on success).
    transient_backoff: u32,
}

impl PollState {
    fn new() -> Self {
        Self {
            cached_creds: None,
            auth_failures: 0,
            transient_backoff: 0,
        }
    }

    /// Return the effective sleep duration for the next cycle.
    fn next_interval(&self, base: Duration) -> Duration {
        if self.auth_failures >= AUTH_FAIL_SLOWDOWN {
            return SLOWDOWN_INTERVAL;
        }
        if self.transient_backoff > 0 {
            let backoff = base * 2u32.saturating_pow(self.transient_backoff - 1);
            return backoff.min(MAX_BACKOFF);
        }
        base
    }

    fn on_success(&mut self) {
        self.auth_failures = 0;
        self.transient_backoff = 0;
    }
}

async fn run(args: Args) {
    let config_dir = credentials::resolve_config_dir(args.config_dir.as_deref());
    let base_interval = Duration::from_secs(args.interval);

    log::info!(
        "Starting claude-usage-daemon: device={}:{}, interval={}s, config={}, mode={}",
        args.device_host,
        args.device_port,
        args.interval,
        config_dir.display(),
        if args.daemon { "daemon" } else { "foreground" }
    );

    let http = reqwest::Client::builder()
        .timeout(Duration::from_secs(30))
        .build()
        .expect("Failed to create HTTP client");

    // Install shutdown signal handler early so Ctrl+C works at any point.
    let shutdown = shutdown_signal();
    tokio::pin!(shutdown);

    // Startup: retry device connectivity, then push immediately.
    let mut startup_ok = false;
    for attempt in 1..=5 {
        match push::ping_device(&http, &args.device_host, args.device_port).await {
            Ok(Some(_)) => {
                log::info!(
                    "Device reachable at {}:{} (HMAC auth supported)",
                    args.device_host, args.device_port
                );
                startup_ok = true;
                break;
            }
            Ok(None) => {
                log::info!(
                    "Device reachable at {}:{} (legacy auth, no HMAC nonce)",
                    args.device_host, args.device_port
                );
                startup_ok = true;
                break;
            }
            Err(e) => {
                if attempt < 5 {
                    log::warn!(
                        "Device not reachable (attempt {attempt}/5): {e}, retrying in 3s..."
                    );
                    tokio::select! {
                        _ = time::sleep(Duration::from_secs(3)) => {}
                        _ = &mut shutdown => {
                            log::info!("Shutting down");
                            cleanup_pid(&args);
                            return;
                        }
                    }
                } else {
                    log::warn!(
                        "Device not reachable after 5 attempts: {e} (will retry on each push)"
                    );
                }
            }
        }
    }

    let mut state = PollState::new();

    // Push data immediately so device exits "waiting for daemon".
    if startup_ok {
        tokio::select! {
            _ = poll_and_push(
                &http,
                &config_dir,
                &args.device_host,
                args.device_port,
                &args.api_key,
                &mut state,
            ) => {}
            _ = &mut shutdown => {
                log::info!("Shutting down");
                cleanup_pid(&args);
                return;
            }
        }
    }

    loop {
        let sleep_dur = state.next_interval(base_interval);
        tokio::select! {
            _ = time::sleep(sleep_dur) => {
                poll_and_push(
                    &http,
                    &config_dir,
                    &args.device_host,
                    args.device_port,
                    &args.api_key,
                    &mut state,
                ).await;
            }
            _ = &mut shutdown => {
                log::info!("Shutting down");
                cleanup_pid(&args);
                break;
            }
        }
    }
}

fn cleanup_pid(args: &Args) {
    if args.daemon {
        #[cfg(unix)]
        {
            let default = default_pid_path();
            let pid_path = args.pid_file.as_deref().unwrap_or(&default);
            let p = std::path::Path::new(pid_path);
            // Only remove if it is a regular file (not a symlink to something else).
            if p.symlink_metadata().map(|m| m.file_type().is_file()).unwrap_or(false) {
                let _ = std::fs::remove_file(p);
            }
        }
    }
}

/// Load credentials from disk and cache them, returning None on failure.
fn load_and_cache(
    config_dir: &std::path::Path,
    state: &mut PollState,
) -> Option<Arc<credentials::OAuthCredentials>> {
    match credentials::load_credentials(config_dir) {
        Ok(fresh) => {
            let arc = Arc::new(fresh);
            state.cached_creds = Some(Arc::clone(&arc));
            Some(arc)
        }
        Err(e) => {
            log::error!("Cannot load credentials: {e}");
            None
        }
    }
}

async fn poll_and_push(
    http: &reqwest::Client,
    config_dir: &std::path::Path,
    device_host: &str,
    device_port: u16,
    api_key: &str,
    state: &mut PollState,
) {
    // -- Load or refresh credentials --
    let creds = match &state.cached_creds {
        Some(c) if !c.is_expired() => Arc::clone(c),
        Some(c) => {
            match credentials::refresh_token(c, config_dir, http).await {
                Ok(refreshed) => {
                    log::info!("Token refreshed successfully");
                    let arc = Arc::new(refreshed);
                    state.cached_creds = Some(Arc::clone(&arc));
                    arc
                }
                Err(CredentialError::AuthRevoked(e)) => {
                    state.auth_failures += 1;
                    if state.auth_failures >= AUTH_FAIL_SLOWDOWN {
                        log::error!(
                            "Authentication failed {} times: {e}. Re-run `claude` CLI to refresh tokens. Retrying in 30 minutes.",
                            state.auth_failures
                        );
                    } else {
                        log::warn!("Token refresh denied ({e}), reloading from storage");
                    }
                    match load_and_cache(config_dir, state) {
                        Some(arc) => arc,
                        None => return,
                    }
                }
                Err(CredentialError::Transient(e)) => {
                    state.transient_backoff = state.transient_backoff.saturating_add(1);
                    log::warn!("Token refresh transient error ({e}), will backoff");
                    match load_and_cache(config_dir, state) {
                        Some(arc) => arc,
                        None => return,
                    }
                }
                Err(CredentialError::NotFound(e)) => {
                    log::error!("Credentials not found: {e}");
                    return;
                }
            }
        }
        None => {
            match load_and_cache(config_dir, state) {
                Some(arc) => arc,
                None => return,
            }
        }
    };

    // -- Fetch usage --
    let usage_resp = match usage::fetch_usage(http, &creds.access_token).await {
        Ok(resp) => resp,
        Err(UsageError::AuthExpired(e)) => {
            log::warn!("Usage API auth expired ({e}), clearing cached credentials");
            state.cached_creds = None; // Force reload + refresh on next cycle.
            state.auth_failures += 1;
            if state.auth_failures >= AUTH_FAIL_SLOWDOWN {
                log::error!(
                    "Authentication failed {} times. Re-run `claude` CLI to refresh tokens. Retrying in 30 minutes.",
                    state.auth_failures
                );
            }
            return;
        }
        Err(UsageError::Transient(e)) => {
            state.transient_backoff = state.transient_backoff.saturating_add(1);
            log::error!("Usage fetch transient error: {e}");
            return;
        }
    };

    let payload = usage_resp.to_device_payload();

    log::info!(
        "Usage: 5h={:.1}%, 7d={:.1}%",
        payload.five_hour,
        payload.seven_day
    );

    // -- Push to device --
    match push::push_to_device(http, device_host, device_port, api_key, &payload).await {
        Ok(()) => log::info!("Pushed to device successfully"),
        Err(push::PushError::AuthRejected(e)) => {
            log::error!("{e}");
        }
        Err(push::PushError::DeviceOffline(e)) => {
            log::warn!("Device is offline: {e}");
        }
        Err(push::PushError::Other(e)) => {
            log::warn!("Device push error: {e}");
        }
    }

    // Usage fetch succeeded: reset API backoff counters.
    // Device push failures are independent (LAN issue, not API auth).
    state.on_success();
}

async fn shutdown_signal() {
    #[cfg(unix)]
    {
        use tokio::signal::unix::{signal, SignalKind};
        let ctrl_c = tokio::signal::ctrl_c();
        let mut sigterm = signal(SignalKind::terminate())
            .expect("Failed to register SIGTERM handler");
        tokio::select! {
            _ = ctrl_c => {}
            _ = sigterm.recv() => {}
        }
    }
    #[cfg(not(unix))]
    {
        tokio::signal::ctrl_c().await.ok();
    }
}

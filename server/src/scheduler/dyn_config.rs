//! Dynamic scheduling configuration: env var flags + auto-detection.

use std::env;

/// Configuration for dynamic CPU scheduling.
#[derive(Debug, Clone)]
pub struct DynSchedConfig {
    /// Force dynamic scheduling on (1) or off (0). None = auto-detect.
    pub force_on: Option<bool>,
    /// Aggressive mode: 50ms poll interval (vs 100ms default).
    pub aggressive: bool,
    /// Memory limit in MB. Workers won't grab more cores if exceeded.
    pub mem_limit_mb: u32,
    /// Custom shm path (testing). None = auto-generate.
    pub shm_path: Option<String>,
}

impl DynSchedConfig {
    /// Load from environment variables.
    ///
    /// Recognised truthy values: `"1"`, `"true"`, `"on"`.
    /// Recognised falsy values: `"0"`, `"false"`, `"off"`.
    /// For `CODESCOPE_DYNAMIC_SCHED`, any other value falls back to
    /// `Some(false)` (force off) so a typo doesn't silently enable
    /// dynamic scheduling. For `CODESCOPE_AGGRESSIVE`, any other value
    /// is treated as `false`.
    pub fn from_env() -> Self {
        let force_on = match env::var("CODESCOPE_DYNAMIC_SCHED") {
            Ok(v) => Some(matches!(v.as_str(), "1" | "true" | "on")),
            Err(_) => None,
        };
        let aggressive = env::var("CODESCOPE_AGGRESSIVE")
            .map(|v| matches!(v.as_str(), "1" | "true" | "on"))
            .unwrap_or(false);
        let mem_limit_mb = env::var("CODESCOPE_MEM_LIMIT_MB")
            .ok()
            .and_then(|v| v.parse().ok())
            .unwrap_or(4096);
        let shm_path = env::var("CODESCOPE_SHM_PATH").ok();
        Self {
            force_on,
            aggressive,
            mem_limit_mb,
            shm_path,
        }
    }

    /// Should dynamic scheduling be used for this project?
    pub fn should_enable(&self, total_modules: usize, total_files: u64) -> bool {
        match self.force_on {
            Some(b) => b,
            None => total_modules > 4 && total_files > 10000,
        }
    }

    /// Generate a shm path for this scheduler process.
    pub fn default_shm_path() -> String {
        format!("/tmp/codescope_sched_{}.shm", std::process::id())
    }
}

/// Sample the total RSS (in MB) of a list of child PIDs.
/// Returns 0 on failure (e.g., unsupported platform).
pub fn sample_total_rss_mb(pids: &[u32]) -> u32 {
    let mut total_kb: u64 = 0;
    for pid in pids {
        // Use ps -o rss= -p <pid> (macOS + Linux compatible)
        let output = std::process::Command::new("ps")
            .args(["-o", "rss=", "-p", &pid.to_string()])
            .stderr(std::process::Stdio::null())
            .output();
        if let Ok(out) = output
            && out.status.success()
        {
            let s = String::from_utf8_lossy(&out.stdout).trim().to_string();
            if let Ok(kb) = s.parse::<u64>() {
                total_kb += kb;
            }
        }
    }
    (total_kb / 1024) as u32
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_from_env_defaults() {
        // SAFETY: env mutation is unsafe in Rust 2024 because it races
        // with concurrent env readers. This test runs single-threaded
        // within the process (no other threads read these vars), and
        // nextest runs each test in its own process by default.
        unsafe {
            std::env::remove_var("CODESCOPE_DYNAMIC_SCHED");
            std::env::remove_var("CODESCOPE_AGGRESSIVE");
            std::env::remove_var("CODESCOPE_MEM_LIMIT_MB");
        }
        let cfg = DynSchedConfig::from_env();
        assert_eq!(cfg.force_on, None);
        assert!(!cfg.aggressive);
        assert_eq!(cfg.mem_limit_mb, 4096);
    }

    #[test]
    fn test_should_enable_auto() {
        let cfg = DynSchedConfig {
            force_on: None,
            aggressive: false,
            mem_limit_mb: 4096,
            shm_path: None,
        };
        assert!(!cfg.should_enable(3, 5000)); // small
        assert!(cfg.should_enable(5, 15000)); // large
    }

    #[test]
    fn test_should_enable_force() {
        let cfg = DynSchedConfig {
            force_on: Some(true),
            aggressive: false,
            mem_limit_mb: 4096,
            shm_path: None,
        };
        assert!(cfg.should_enable(1, 100)); // forced on even for tiny
    }

    #[test]
    fn test_sample_rss_returns_nonzero_for_self() {
        let pid = std::process::id();
        let rss = sample_total_rss_mb(&[pid]);
        assert!(rss > 0); // self RSS should be > 0
    }
}

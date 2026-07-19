//! Dynamic scheduling configuration: env var flags + auto-detection.
//!
//! The `DynSchedConfig` struct is wired into both `index_parallel` (for
//! the dispatch decision) and `index_parallel_chunked` (for per-run config).
//! All fields are read via the public API; the dead_code lint for fields
//! is suppressed because the struct is constructed via `from_env()` and
//! its fields are accessed through method calls, not direct field access.
#![allow(dead_code)]

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

/// Bytes per megabyte, used to convert the kernel's byte/page RSS
/// figures into the MB unit the scheduler compares against
/// `mem_limit_mb`.
const BYTES_PER_MB: u64 = 1024 * 1024;

/// Sample the total RSS (in MB) of a list of child PIDs using
/// platform-native APIs — **zero fork** per DYNAMIC_SCHED_REDESIGN.md
/// §4.6 (the previous implementation spawned one `ps` per PID every
/// poll, ~150 forks/sec under a 100ms interval).
///
/// - macOS: `proc_pidinfo(PROC_PIDTASKINFO)` → `pti_resident_size`.
/// - Linux: `/proc/<pid>/statm` field 2 (resident pages) × page size.
/// - Other platforms: returns 0 (memory monitoring degrades to no-op).
///
/// A PID that has already exited (or is otherwise unreadable)
/// contributes 0 rather than failing the whole sample.
pub fn sample_total_rss_mb(pids: &[u32]) -> u32 {
    let mut total_bytes: u64 = 0;
    for &pid in pids {
        total_bytes = total_bytes.saturating_add(rss_bytes(pid));
    }
    (total_bytes / BYTES_PER_MB) as u32
}

/// Resident set size of a single process in bytes. Returns 0 if the
/// PID cannot be queried (exited, permission denied, or unsupported
/// platform) — callers treat 0 as "unknown / excluded from the total".
#[cfg(target_os = "macos")]
fn rss_bytes(pid: u32) -> u64 {
    // SAFETY: `ti` is zero-initialised POD; `proc_pidinfo` writes up to
    // size_of::<proc_taskinfo>() bytes into it and returns the number
    // of bytes written. We only read `ti` when the return value equals
    // the full struct size, so no uninitialised field is observed.
    unsafe {
        let mut ti: libc::proc_taskinfo = std::mem::zeroed();
        let size = std::mem::size_of::<libc::proc_taskinfo>() as libc::c_int;
        let written = libc::proc_pidinfo(
            pid as libc::c_int,
            libc::PROC_PIDTASKINFO,
            0,
            &mut ti as *mut _ as *mut libc::c_void,
            size,
        );
        if written == size {
            ti.pti_resident_size
        } else {
            0
        }
    }
}

/// Linux variant: parse `/proc/<pid>/statm` (values are in pages).
#[cfg(target_os = "linux")]
fn rss_bytes(pid: u32) -> u64 {
    // Format: "size resident shared text lib data dt" (all in pages).
    // Field index 1 (resident) is the RSS we want.
    let content = match std::fs::read_to_string(format!("/proc/{}/statm", pid)) {
        Ok(c) => c,
        Err(_) => return 0, // process exited or unreadable — count as 0
    };
    let resident_pages: u64 = content
        .split_whitespace()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(0);
    // SAFETY: sysconf is a pure query with no side effects; a negative
    // return (unlikely) falls back to the conventional 4KiB page size.
    let page_size = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
    let page_size = if page_size > 0 {
        page_size as u64
    } else {
        4096
    };
    resident_pages.saturating_mul(page_size)
}

/// Fallback for unsupported platforms: memory monitoring is a no-op.
#[cfg(not(any(target_os = "macos", target_os = "linux")))]
fn rss_bytes(_pid: u32) -> u64 {
    0
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

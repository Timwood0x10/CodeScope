//! Quarantine logic for crashed modules.
//!
//! When a module worker crashes or produces zero nodes, we run a
//! binary search over its file list to localise the crashing file(s).
//! Once a crasher is found, the module is retried with that file
//! excluded via `CODESCOPE_EXCLUDE_PATHS`.
//!
//! Algorithm (mirrors `codescope-parallel.sh:find_crashing_file`):
//! 1. Get candidate file list via `discover::discover_files()`.
//! 2. Binary search: split list in half, run worker with `--file-list`
//!    on the left half. If it crashes, recurse left; else advance right.
//! 3. When `left == right`, that file is the crasher.
//! 4. Repeat up to `QUARANTINE_MAX_ITER` times to find multiple crashers.

use serde_json::Value;
use std::path::Path;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use super::{POLL_INTERVAL, QUARANTINE_MAX_ITER, worker::make_relative_glob};
use crate::discover;

/// Per-binary-search-iteration timeout. If the worker hasn't exited
/// by this deadline, we kill it and treat the left half as containing
/// a crasher (the hang is the crash symptom — recurse left to localise).
const BINARY_SEARCH_TIMEOUT: Duration = Duration::from_secs(120);

/// Binary-search a module's file list to localise a crashing file.
///
/// Returns the list of crashing file paths (relative globs suitable
/// for `CODESCOPE_EXCLUDE_PATHS`). Empty vec = no crash found (either
/// the module recovered or no individual file crashed).
pub(super) fn quarantine_module(
    exe: &str,
    project_dir: &str,
    module_name: &str,
    grammars_dir: &str,
    db_prefix: &str,
) -> Vec<String> {
    let module_dir = Path::new(project_dir).join(module_name);
    let discover_json = discover::discover_files(module_dir.to_str().unwrap_or(""));
    let discover_val: Value = match serde_json::from_str(&discover_json) {
        Ok(v) => v,
        Err(_) => return Vec::new(),
    };
    if discover_val["ok"] != true {
        return Vec::new();
    }
    let files = discover_val["files"]
        .as_array()
        .cloned()
        .unwrap_or_default();
    if files.is_empty() {
        return Vec::new();
    }

    let mut crashers: Vec<String> = Vec::new();
    let module_db = format!("{}_{}_quarantine.db", db_prefix, module_name);

    for iter in 1..=QUARANTINE_MAX_ITER {
        eprintln!(
            "scheduler: quarantine {} iter {} ({} files, {} already excluded)",
            module_name,
            iter,
            files.len(),
            crashers.len()
        );

        // Filter out already-found crashers before each iteration so
        // the binary search can find additional crashers.
        let active_files: Vec<String> = files
            .iter()
            .filter_map(|f| {
                let s = f.as_str()?;
                let basename = Path::new(s).file_name()?.to_string_lossy().to_string();
                if crashers.iter().any(|c| {
                    Path::new(c)
                        .file_name()
                        .map(|n| n == basename.as_str())
                        .unwrap_or(false)
                }) {
                    None
                } else {
                    Some(s.to_string())
                }
            })
            .collect();

        if active_files.is_empty() {
            break;
        }

        let crasher = binary_search_crasher(
            exe,
            module_dir.to_str().unwrap_or(""),
            &active_files,
            &module_db,
            grammars_dir,
        );

        match crasher {
            Some(c) => {
                eprintln!("scheduler: quarantine {} found crasher: {}", module_name, c);
                // Store as a glob relative to the module dir so the
                // worker's CODESCOPE_EXCLUDE_PATHS can match it.
                let rel = make_relative_glob(&c, module_dir.to_str().unwrap_or(""));
                crashers.push(rel);
            }
            None => {
                eprintln!(
                    "scheduler: quarantine {} — no individual file crashed, stopping",
                    module_name
                );
                break;
            }
        }
    }

    crashers
}

/// Binary search a sorted file list to find one crashing file.
///
/// Splits `[left, right]` in half, writes the left half to a temp
/// JSON file, runs the worker with `--file-list <tmp>`. If exit code
/// is non-zero and non-124 (timeout), the crasher is in the left half;
/// otherwise advance to the right half.
fn binary_search_crasher(
    exe: &str,
    module_dir: &str,
    files: &[String],
    db_path: &str,
    grammars_dir: &str,
) -> Option<String> {
    if files.is_empty() {
        return None;
    }
    let mut left = 0;
    let mut right = files.len() - 1;

    while left <= right {
        let mid = (left + right) / 2;
        let half: Vec<String> = files[left..=mid].to_vec();
        let json_list = serde_json::to_string(&half).ok()?;

        // Write JSON file list to a temp path the worker can read.
        let tmp_path = format!("{}.filelist.json", db_path);
        if std::fs::write(&tmp_path, &json_list).is_err() {
            return None;
        }

        // Clean DB before each retry so prior data doesn't interfere.
        let _ = std::fs::remove_file(db_path);
        let _ = std::fs::remove_file(format!("{}-wal", db_path));
        let _ = std::fs::remove_file(format!("{}-shm", db_path));

        let mut cmd = Command::new(exe);
        cmd.args([
            "worker",
            db_path,
            module_dir,
            "",           // no language filter
            "quarantine", // project name
            "1",          // project_id=1 — quarantine DBs are isolated and
            // never merged, so any non-zero value works; using
            // 1 avoids the worker calling create_project (saves
            // a few ms per binary-search iteration).
            "--file-list",
            &tmp_path,
        ]);
        cmd.env("GRAMMARS_DIR", grammars_dir);
        cmd.env("CODESCOPE_DB_PATH", db_path);
        cmd.env("CODESCOPE_INDEX_MODE", "fast");
        cmd.env("CODESCOPE_WORKERS", "1");
        cmd.stdout(Stdio::null()).stderr(Stdio::null());

        let t0 = Instant::now();
        let mut child = match cmd.spawn() {
            Ok(c) => c,
            Err(_) => {
                let _ = std::fs::remove_file(&tmp_path);
                return None;
            }
        };

        // Poll for exit with a hard timeout. On timeout, kill the
        // child and treat the left half as containing a crasher — a
        // hang IS a crash symptom, so we recurse left to localise it
        // (rather than advancing right, which would miss the crasher).
        let status_code: i32 = loop {
            if t0.elapsed() > BINARY_SEARCH_TIMEOUT {
                let _ = child.kill();
                let _ = child.wait(); // reap zombie
                break -1; // treat hang as crash
            }
            match child.try_wait() {
                Ok(Some(s)) => break s.code().unwrap_or(-1),
                Ok(None) => std::thread::sleep(POLL_INTERVAL),
                Err(_) => {
                    let _ = child.kill();
                    let _ = child.wait();
                    break -1;
                }
            }
        };

        // Clean up temp file regardless of outcome.
        let _ = std::fs::remove_file(&tmp_path);

        let ec = status_code;
        // 0 = success, 124 = timeout-as-success (worker's own timeout
        // exit code, meaning the worker cleanly timed out without
        // crashing). Any other code (including our -1 for hang) means
        // the left half contains a crasher — recurse left.
        if ec == 0 || ec == 124 {
            // Left half is OK — advance to right half.
            if mid + 1 >= files.len() {
                return None; // exhausted, no crasher found
            }
            left = mid + 1;
        } else {
            // Crasher is in [left, mid]. If single file, it's the crasher.
            if left == mid {
                return Some(files[left].clone());
            }
            right = mid;
        }
    }
    None
}

#[cfg(test)]
mod tests {
    // quarantine_module requires a real codescope binary and discover
    // infrastructure — covered by integration tests in tests/. The
    // binary search algorithm is exercised end-to-end by index-parallel
    // self-test runs that intentionally crash.
}

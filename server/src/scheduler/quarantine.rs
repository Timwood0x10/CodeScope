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
/// by this deadline, we kill it. Unlike the previous behaviour, a
/// timeout is NOT treated as a crash signal for bisection — see M7.
const BINARY_SEARCH_TIMEOUT: Duration = Duration::from_secs(120);

/// Number of times to run the worker on the same file subset before
/// trusting the outcome enough to recurse. Non-deterministic crashes
/// (OOM, data races, ASLR-dependent SIGSEGV) would otherwise lead
/// the bisection to the wrong file and permanently exclude a normal
/// file via `CODESCOPE_EXCLUDE_PATHS`. See M7.
const REPRODUCIBILITY_RUNS: usize = 3;

/// Outcome of a single worker run on a file subset. Distinguishes a
/// real crash (non-zero exit, non-124) from a timeout (hang) — these
/// must NOT be conflated because hangs are far less reproducible and
/// may be caused by system load rather than a specific file (M7).
#[derive(Clone, Copy, Debug)]
enum WorkerOutcome {
    /// Worker exited cleanly (0) or self-reported a clean timeout
    /// (exit 124). Either way, no crash observed on this run.
    Ok,
    /// Worker exited with a non-zero code other than 124 — a real
    /// crash (SIGSEGV, abort, etc.). Carries the exit code.
    Crash(i32),
    /// Worker exceeded `BINARY_SEARCH_TIMEOUT` and had to be killed.
    /// Not treated as a crash signal (M7).
    Timeout,
    /// Worker could not be spawned at all (env/exe issue). Abort the
    /// bisection rather than guessing.
    SpawnFailed,
}

// Reproducibility comparison: two `Crash(_)` outcomes are considered
// equal even if their exit codes differ. A race-condition crash may
// produce SIGSEGV (139) on one run and SIGABRT (134) on another; that
// is still a reproducible crash and we want to keep bisecting, not
// abort as "non-deterministic". Only the outcome *category* matters
// for the M7 reproducibility check.
impl PartialEq for WorkerOutcome {
    fn eq(&self, other: &Self) -> bool {
        matches!(
            (self, other),
            (WorkerOutcome::Ok, WorkerOutcome::Ok)
                | (WorkerOutcome::Crash(_), WorkerOutcome::Crash(_))
                | (WorkerOutcome::Timeout, WorkerOutcome::Timeout)
                | (WorkerOutcome::SpawnFailed, WorkerOutcome::SpawnFailed)
        )
    }
}
impl Eq for WorkerOutcome {}

/// Outcome of running the worker `REPRODUCIBILITY_RUNS` times on the
/// same file subset.
enum ReproducibleOutcome {
    /// Every run agreed on this outcome.
    Consistent(WorkerOutcome),
    /// Runs disagreed — the crash is non-deterministic (OOM, race,
    /// etc.). We must NOT exclude any file based on this; the bisection
    /// is aborted and the caller reports the situation (M7).
    NonDeterministic,
}

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
/// JSON file, runs the worker `REPRODUCIBILITY_RUNS` times on that
/// subset, and only trusts the outcome if every run agrees (M7):
///   - All `Ok`            → advance to the right half.
///   - All `Crash(_)`      → crasher is in the left half; recurse left.
///   - All `Timeout`       → abort bisection (hang is not a crash signal).
///   - All `SpawnFailed`   → abort bisection (cannot run worker).
///   - Mixed outcomes      → non-deterministic; abort bisection and do
///     NOT add any file to `CODESCOPE_EXCLUDE_PATHS`.
///
/// Returns `Some(file)` only when a single file reproduces a `Crash`
/// across all runs. Returns `None` on abort, exhaustion, or any
/// non-deterministic / non-crash outcome.
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

        let outcome = run_subset_reproducible(exe, module_dir, db_path, grammars_dir, &tmp_path);

        // Clean up temp file regardless of outcome.
        let _ = std::fs::remove_file(&tmp_path);

        match outcome {
            ReproducibleOutcome::Consistent(WorkerOutcome::Ok) => {
                // Left half is OK across all runs — advance to right half.
                if mid + 1 >= files.len() {
                    return None; // exhausted, no crasher found
                }
                left = mid + 1;
            }
            ReproducibleOutcome::Consistent(WorkerOutcome::Crash(code)) => {
                // Crasher is reproducibly in [left, mid]. If single
                // file, it's the crasher. Log the exit code for
                // traceability (per code_rules.md: no silent errors).
                eprintln!(
                    "scheduler: quarantine — reproducible crash (exit code {}) on subset [{}..{}] [module=scheduler, method=binary_search_crasher]",
                    code, left, mid
                );
                if left == mid {
                    return Some(files[left].clone());
                }
                right = mid;
            }
            ReproducibleOutcome::Consistent(WorkerOutcome::Timeout) => {
                // Hang is NOT a crash signal (M7): it may be caused by
                // system load, OOM, or an infinite loop, and is too
                // unreliable to blame a specific file. Abort the
                // bisection without excluding any file.
                eprintln!(
                    "scheduler: quarantine — consistent timeout on subset, aborting bisection (not a crash signal) [module=scheduler, method=binary_search_crasher]"
                );
                return None;
            }
            ReproducibleOutcome::Consistent(WorkerOutcome::SpawnFailed) => {
                // Cannot run the worker at all — abort cleanly.
                eprintln!(
                    "scheduler: quarantine — worker spawn failed consistently, aborting bisection [module=scheduler, method=binary_search_crasher]"
                );
                return None;
            }
            ReproducibleOutcome::NonDeterministic => {
                // Runs disagreed — non-deterministic crash (OOM, race,
                // ASLR-dependent SIGSEGV). We must NOT exclude any file
                // based on this: doing so would permanently skip a
                // normal file. Report and abort. See M7.
                eprintln!(
                    "scheduler: quarantine — non-deterministic outcome across {} runs, aborting bisection (will not exclude files) [module=scheduler, method=binary_search_crasher]",
                    REPRODUCIBILITY_RUNS
                );
                return None;
            }
        }
    }
    None
}

/// Run the worker once on the file subset described by `tmp_path` and
/// wait up to `BINARY_SEARCH_TIMEOUT`. Returns the `WorkerOutcome`;
/// never panics. Cleans up the DB files before spawning so prior data
/// doesn't interfere with the run. See M7.
fn run_subset_once(
    exe: &str,
    module_dir: &str,
    db_path: &str,
    grammars_dir: &str,
    tmp_path: &str,
) -> WorkerOutcome {
    // Clean DB before each run so prior data doesn't interfere.
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
        tmp_path,
    ]);
    cmd.env("GRAMMARS_DIR", grammars_dir);
    cmd.env("CODESCOPE_DB_PATH", db_path);
    cmd.env("CODESCOPE_INDEX_MODE", "fast");
    cmd.env("CODESCOPE_WORKERS", "1");
    cmd.stdout(Stdio::null()).stderr(Stdio::null());

    let t0 = Instant::now();
    let mut child = match cmd.spawn() {
        Ok(c) => c,
        Err(_) => return WorkerOutcome::SpawnFailed,
    };

    // Poll for exit with a hard timeout. Distinguish timeout from
    // crash: a hang is NOT treated as a crash signal (M7) because it
    // is far less reproducible and may be caused by system load.
    loop {
        if t0.elapsed() > BINARY_SEARCH_TIMEOUT {
            let _ = child.kill();
            let _ = child.wait(); // reap zombie
            return WorkerOutcome::Timeout;
        }
        match child.try_wait() {
            Ok(Some(s)) => {
                let code = s.code().unwrap_or(-1);
                // 0 = success, 124 = worker's own clean timeout exit.
                // Any other code is a real crash.
                return if code == 0 || code == 124 {
                    WorkerOutcome::Ok
                } else {
                    WorkerOutcome::Crash(code)
                };
            }
            Ok(None) => std::thread::sleep(POLL_INTERVAL),
            Err(_) => {
                let _ = child.kill();
                let _ = child.wait();
                return WorkerOutcome::Crash(-1);
            }
        }
    }
}

/// Run the worker `REPRODUCIBILITY_RUNS` times on the same subset and
/// return `Consistent(outcome)` only if every run agreed. Any
/// disagreement yields `NonDeterministic` (M7).
fn run_subset_reproducible(
    exe: &str,
    module_dir: &str,
    db_path: &str,
    grammars_dir: &str,
    tmp_path: &str,
) -> ReproducibleOutcome {
    let mut first: Option<WorkerOutcome> = None;
    for run in 1..=REPRODUCIBILITY_RUNS {
        let outcome = run_subset_once(exe, module_dir, db_path, grammars_dir, tmp_path);
        match first {
            None => first = Some(outcome),
            Some(prev) if prev == outcome => {
                // Still consistent with the first run.
            }
            Some(_) => {
                eprintln!(
                    "scheduler: quarantine — run {}/{} disagreed with first run (prev={:?}, this={:?}) [module=scheduler, method=run_subset_reproducible]",
                    run, REPRODUCIBILITY_RUNS, first, outcome
                );
                return ReproducibleOutcome::NonDeterministic;
            }
        }
    }
    ReproducibleOutcome::Consistent(first.expect("at least one run"))
}

#[cfg(test)]
mod tests {
    // quarantine_module requires a real codescope binary and discover
    // infrastructure — covered by integration tests in tests/. The
    // binary search algorithm is exercised end-to-end by index-parallel
    // self-test runs that intentionally crash.
}

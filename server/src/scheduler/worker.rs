//! Per-module worker subprocess spawning.
//!
//! `run_module_worker` spawns one `codescope worker` subprocess per
//! module, waits with a timeout, and parses the worker's result JSON
//! from stdout. The result is returned as a `ModuleResult` for the
//! scheduler to aggregate.
//!
//! `extract_worker_json` finds the last balanced `{...}` block in
//! stdout — this handles log noise (the worker prints log lines to
//! stderr but may emit progress JSON to stdout on some paths).
//!
//! `make_relative_glob` converts an absolute file path to a glob
//! pattern relative to the module dir, suitable for
//! `CODESCOPE_EXCLUDE_PATHS`.

use serde_json::Value;
use std::path::Path;
use std::process::{Command, Stdio};
use std::sync::mpsc;
use std::time::{Duration, Instant};

use super::{DEFAULT_WORKER_TIMEOUT_SECS, ModuleResult, POLL_INTERVAL};

/// Run one module worker subprocess. Returns a `ModuleResult` regardless
/// of success or failure — never panics.
///
/// `project_id` — unique project_id assigned by the scheduler (1, 2, 3, ...).
///   Each module worker writes rows with this project_id so the merge step
///   can disambiguate rows across modules. Must be non-zero (0 would cause
///   the worker to call create_project and auto-assign a fresh id,
///   re-introducing the project_id collision this scheme avoids).
///
/// `quarantine_exclude` — when `Some(patterns)`, sets the
/// `CODESCOPE_EXCLUDE_PATHS` env var so the worker's FilterPolicy
/// skips the listed files. Patterns are comma-separated globs.
#[allow(clippy::too_many_arguments)]
pub(super) fn run_module_worker(
    exe: &str,
    project_dir: &str,
    module_name: &str,
    files_estimate: u64,
    workers: u32,
    grammars_dir: &str,
    db_prefix: &str,
    project_id: u64,
    quarantine_exclude: Option<&str>,
    keep_db: bool,
) -> ModuleResult {
    let module_dir = Path::new(project_dir).join(module_name);
    let module_db = format!("{}_{}.db", db_prefix, module_name);

    // Normally start from a clean DB file — a stale DB would have
    // outdated graph_nodes from a previous (possibly crashed) run.
    // When keep_db is set (caller pinned CODESCOPE_DB_PREFIX for an
    // incremental run), preserve the module DB so the engine's
    // file_scan_state mtime check skips unchanged files.
    if !keep_db {
        let _ = std::fs::remove_file(&module_db);
        let _ = std::fs::remove_file(format!("{}-wal", module_db));
        let _ = std::fs::remove_file(format!("{}-shm", module_db));
    }

    let project_name = format!("parallel-{}", module_name);
    let workers_str = workers.to_string();
    // Pass the scheduler-assigned project_id to the worker. The worker
    // (main.rs) treats a non-zero value as a forced project_id and skips
    // create_project, so each module's rows carry a unique project_id
    // and the merge step can correctly disambiguate them.
    let project_id_str = if project_id == 0 {
        "1".to_string()
    } else {
        project_id.to_string()
    };

    let mut cmd = Command::new(exe);
    cmd.args([
        "worker",
        &module_db,
        module_dir.to_str().unwrap_or(""),
        "", // empty language filter — index all languages
        &project_name,
        &project_id_str,
    ]);
    cmd.env("GRAMMARS_DIR", grammars_dir);
    cmd.env("CODESCOPE_DB_PATH", &module_db);
    cmd.env("CODESCOPE_INDEX_MODE", "fast");
    cmd.env("CODESCOPE_WORKERS", &workers_str);
    // Skip the ~280ms state-builder work in per-module workers; the
    // unified DB gets its async pass once after merge (see merge::merge_module_dbs).
    cmd.env("CODESCOPE_SKIP_ASYNC", "1");
    // v0.2.6 (C2 fix): parallel workers build CSR adjacency from LOCAL
    // entity ids, which merge cannot remap inside the packed tgt_blob /
    // src_blob (binary, not a SQL remap column). Defer CSR construction to
    // the merged main.db where relation ids are already global, so CSR-based
    // graph queries don't return dangling neighbor ids.
    cmd.env("CODESCOPE_DEFER_CSR", "1");
    if let Some(exclude) = quarantine_exclude {
        cmd.env("CODESCOPE_EXCLUDE_PATHS", exclude);
    }
    cmd.stdout(Stdio::piped()).stderr(Stdio::inherit());

    let t0 = Instant::now();
    let mut child = match cmd.spawn() {
        Ok(c) => c,
        Err(e) => {
            return ModuleResult {
                name: module_name.to_string(),
                exit_code: -1,
                total_nodes: 0,
                total_edges: 0,
                files_indexed: 0,
                candidate_files: 0,
                time_parse_ms: 0,
                duration_secs: t0.elapsed().as_secs(),
                workers,
                db_path: module_db,
                project_id,
                error: Some(format!(
                    "spawn failed: {} [module=scheduler, method=run_module_worker]",
                    e
                )),
            };
        }
    };

    // Take stdout so we can drain it in a background thread.
    // stderr is inherited directly (Stdio::inherit()), so the child
    // writes to the parent's stderr — no drain needed.
    let child_stdout = child.stdout.take().expect("stdout piped");

    // Drain stdout in a dedicated thread. It exits when the pipe
    // closes (i.e., when the child exits or is killed).
    let (stdout_tx, stdout_rx) = mpsc::channel::<Vec<u8>>();
    std::thread::spawn(move || {
        use std::io::Read;
        let mut buf = Vec::new();
        let mut s = child_stdout;
        let _ = s.read_to_end(&mut buf);
        let _ = stdout_tx.send(buf);
    });

    // Poll for child exit with timeout. Parent owns the Child handle,
    // so on timeout it can kill() + wait() to reap — no thread leak.
    let timeout = Duration::from_secs(DEFAULT_WORKER_TIMEOUT_SECS);
    let status = loop {
        if t0.elapsed() > timeout {
            // Kill the child so the drain threads' pipes close and
            // they exit cleanly. Then wait() to reap the zombie.
            let _ = child.kill();
            let _ = child.wait();
            return ModuleResult {
                name: module_name.to_string(),
                exit_code: 124, // 124 = timeout, matches `timeout` shell exit code
                total_nodes: 0,
                total_edges: 0,
                files_indexed: 0,
                candidate_files: 0,
                time_parse_ms: 0,
                duration_secs: t0.elapsed().as_secs(),
                workers,
                db_path: module_db,
                project_id,
                error: Some(format!(
                    "worker timed out after {}s [module=scheduler, method=run_module_worker]",
                    timeout.as_secs()
                )),
            };
        }
        match child.try_wait() {
            Ok(Some(s)) => break s,
            Ok(None) => std::thread::sleep(POLL_INTERVAL),
            Err(e) => {
                let _ = child.kill();
                let _ = child.wait();
                return ModuleResult {
                    name: module_name.to_string(),
                    exit_code: -2,
                    total_nodes: 0,
                    total_edges: 0,
                    files_indexed: 0,
                    candidate_files: 0,
                    time_parse_ms: 0,
                    duration_secs: t0.elapsed().as_secs(),
                    workers,
                    db_path: module_db,
                    project_id,
                    error: Some(format!(
                        "try_wait failed: {} [module=scheduler, method=run_module_worker]",
                        e
                    )),
                };
            }
        }
    };

    let duration = t0.elapsed().as_secs();
    let exit_code = status.code().unwrap_or(-4);

    // Collect drained stdout. recv() blocks until the drain thread
    // sends; it always does once the child's pipes close.
    // stderr is inherited directly, so no drain needed.
    let stdout_bytes = stdout_rx.recv().unwrap_or_default();
    let stdout = String::from_utf8_lossy(&stdout_bytes).to_string();

    // The worker prints its result JSON as the last line of stdout.
    // Extract it by finding the last balanced `{...}` block.
    let parsed = extract_worker_json(&stdout);

    let (total_nodes, total_edges, files_indexed, candidate_files, time_parse_ms) = match &parsed {
        Some(v) => (
            v["total_nodes"].as_u64().unwrap_or(0),
            v["total_edges"].as_u64().unwrap_or(0),
            v["files_indexed"].as_u64().unwrap_or(0),
            v["discovery"]["candidate_files"]
                .as_u64()
                .unwrap_or(files_estimate),
            v["time_parse_ms"].as_u64().unwrap_or(0),
        ),
        None => (0, 0, 0, files_estimate, 0),
    };

    let error = if exit_code == 0 && parsed.is_some() {
        None
    } else {
        // stderr is inherited (Stdio::inherit()), so the child's
        // stderr goes directly to the parent's stderr. The caller
        // can find the full error in the parent's stderr log.
        Some(format!(
            "exit={} [module=scheduler, method=run_module_worker]",
            exit_code
        ))
    };

    ModuleResult {
        name: module_name.to_string(),
        exit_code,
        total_nodes,
        total_edges,
        files_indexed,
        candidate_files,
        time_parse_ms,
        duration_secs: duration,
        workers,
        db_path: module_db,
        project_id,
        error,
    }
}

/// Extract the worker's JSON result from stdout.
///
/// The worker prints log lines to stderr and the result JSON as the
/// last line of stdout. To be robust against log noise, we find the
/// last balanced `{...}` block in stdout and validate it parses.
///
/// We forward-scan stdout with a small state machine that tracks
/// whether we're inside a JSON string literal (handling `\` escape
/// sequences), and use a stack of `{` positions to match each `}`
/// to its opener. When a `}` pops the stack back to empty, we've
/// closed a top-level object — record it. The last such object is
/// the candidate. This handles nested objects correctly AND ignores
/// braces that appear inside string values (M9): a naive byte-level
/// brace match would miscount on output like
/// `{"path":"/some/}path","ok":true}`, fail to find the matching
/// `{`, return None, and cause the module to be misjudged as having
/// zero nodes (sending it to quarantine).
pub(super) fn extract_worker_json(stdout: &str) -> Option<Value> {
    let bytes = stdout.as_bytes();
    let mut in_string = false;
    // True if the next byte should be treated as literal (preceded by
    // a backslash inside a string). Handles `\"`, `\\`, `\n`, etc.
    let mut escape = false;
    // Stack of `{` byte positions. Popped by the matching `}`.
    let mut brace_stack: Vec<usize> = Vec::new();
    // (start, end) of the last complete top-level `{...}` object.
    let mut best: Option<(usize, usize)> = None;

    for (i, &b) in bytes.iter().enumerate() {
        if in_string {
            if escape {
                // Previous byte was a backslash; this byte is literal
                // (e.g. the `"` in `\"` does NOT end the string).
                escape = false;
            } else if b == b'\\' {
                escape = true;
            } else if b == b'"' {
                in_string = false;
            }
            continue;
        }
        match b {
            b'"' => in_string = true,
            b'{' => brace_stack.push(i),
            b'}' => {
                if let Some(start) = brace_stack.pop()
                    && brace_stack.is_empty()
                {
                    // Stack emptied — `start..=i` is a complete
                    // top-level object. Record it; later complete
                    // objects overwrite this so we keep the LAST one.
                    best = Some((start, i));
                }
                // An unbalanced `}` (empty stack) is ignored — it's
                // noise, not part of any object we care about.
            }
            _ => {}
        }
    }

    let (start, end) = best?;
    let candidate = &stdout[start..=end];
    serde_json::from_str(candidate).ok()
}

/// Convert an absolute file path to a glob pattern relative to the
/// module dir, suitable for `CODESCOPE_EXCLUDE_PATHS`.
///
/// `CODESCOPE_EXCLUDE_PATHS` accepts comma-separated glob patterns
/// matched against the relative path from the project root. We emit
/// `module_name/<rel_path>` so the worker's FilterPolicy (which sees
/// the full project-relative path) can match it.
pub(super) fn make_relative_glob(abs_path: &str, module_dir: &str) -> String {
    let abs = Path::new(abs_path);
    let modp = Path::new(module_dir);
    let rel = abs.strip_prefix(modp).unwrap_or(abs);
    let basename = rel
        .file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_default();
    // Use just the basename so the glob matches the file anywhere
    // under the module dir — this mirrors the script's behaviour
    // (`find ... -path "*/$(basename pattern) -delete`).
    if basename.is_empty() {
        abs_path.to_string()
    } else {
        format!("*/{}", basename)
    }
}

/// Run one chunk worker subprocess.
///
/// Spawns a `codescope chunk-worker` subprocess that:
/// 1. Opens the `ChunkQueue` from the shm path.
/// 2. Loops: `claim_next()` → parse the chunk's files → stream-write its OWN DB → `mark_done()`.
/// 3. Exits when all chunks are DONE/FAILED.
///
/// `worker_id` — 0-based index for CPU binding and `claim_next` identity.
/// `cpu_set` — comma-separated CPU list for `taskset`, e.g. `"0-3"` or `"0,2,4,6"`.
///   Pass empty string to skip CPU binding (e.g., on unsupported platforms).
#[allow(clippy::too_many_arguments)]
pub(super) fn run_chunk_worker(
    exe: &str,
    shm_path: &str,
    worker_id: u32,
    cpu_set: &str,
    worker_db: &str,
    files_json_path: &str,
    project_id: u64,
    grammars_dir: &str,
) -> ModuleResult {
    let t0 = Instant::now();
    let worker_id_str = worker_id.to_string();
    let project_id_str = project_id.to_string();

    // Start from a clean DB — a stale worker DB would carry outdated
    // graph nodes from a crashed previous run.
    let _ = std::fs::remove_file(worker_db);
    let _ = std::fs::remove_file(format!("{}-wal", worker_db));
    let _ = std::fs::remove_file(format!("{}-shm", worker_db));

    let mut cmd = Command::new(exe);
    cmd.args([
        "chunk-worker",
        shm_path,
        &worker_id_str,
        worker_db,
        files_json_path,
        &project_id_str,
    ]);
    cmd.env("GRAMMARS_DIR", grammars_dir);
    cmd.env("CODESCOPE_SCHED_SHM", shm_path);
    cmd.env("CODESCOPE_WORKER_ID", &worker_id_str);
    cmd.env("CODESCOPE_DB_PATH", worker_db);
    cmd.env("CODESCOPE_FILES_JSON", files_json_path);
    cmd.env("CODESCOPE_PROJECT_ID", &project_id_str);
    cmd.env("CODESCOPE_INDEX_MODE", "fast");
    cmd.env("CODESCOPE_SKIP_ASYNC", "1");
    // P3a (C2): chunk workers are parallel modules too — defer CSR so it is
    // rebuilt once on the merged DB from globally-remapped relation ids.
    cmd.env("CODESCOPE_DEFER_CSR", "1");

    // CPU binding via taskset if a cpu_set is provided.
    // `taskset` is a util-linux command and is NOT available on macOS
    // or BSDs. Callers must gate cpu_set on `#[cfg(target_os = "linux")]`
    // (see index_parallel_chunked) so non-Linux platforms pass an empty
    // string and skip the taskset wrapping.
    if !cpu_set.is_empty() {
        // Use `taskset -c <cpu_set> <command>` to bind the worker.
        // We wrap the command with taskset by prepending it.
        // taskset -c 0,1,2,3 codescope chunk-worker ...
        let mut taskset_cmd = std::process::Command::new("taskset");
        taskset_cmd.args(["-c", cpu_set, exe]);
        taskset_cmd.args([
            "chunk-worker",
            shm_path,
            &worker_id_str,
            worker_db,
            files_json_path,
            &project_id_str,
        ]);
        taskset_cmd.env("GRAMMARS_DIR", grammars_dir);
        taskset_cmd.env("CODESCOPE_SCHED_SHM", shm_path);
        taskset_cmd.env("CODESCOPE_WORKER_ID", &worker_id_str);
        taskset_cmd.env("CODESCOPE_DB_PATH", worker_db);
        taskset_cmd.env("CODESCOPE_FILES_JSON", files_json_path);
        taskset_cmd.env("CODESCOPE_PROJECT_ID", &project_id_str);
        taskset_cmd.env("CODESCOPE_INDEX_MODE", "fast");
        taskset_cmd.env("CODESCOPE_SKIP_ASYNC", "1");
        // P3a (C2): defer CSR on chunk workers too (see plain branch above).
        taskset_cmd.env("CODESCOPE_DEFER_CSR", "1");
        taskset_cmd.stdout(Stdio::piped()).stderr(Stdio::inherit());
        cmd = taskset_cmd;
    } else {
        cmd.stdout(Stdio::piped()).stderr(Stdio::inherit());
    }

    let mut child = match cmd.spawn() {
        Ok(c) => c,
        Err(e) => {
            return ModuleResult {
                name: format!("chunk-worker-{}", worker_id),
                exit_code: -1,
                total_nodes: 0,
                total_edges: 0,
                files_indexed: 0,
                candidate_files: 0,
                time_parse_ms: 0,
                duration_secs: t0.elapsed().as_secs(),
                workers: 1,
                db_path: worker_db.to_string(),
                project_id,
                error: Some(format!(
                    "spawn failed: {} [module=scheduler, method=run_chunk_worker]",
                    e
                )),
            };
        }
    };

    // Wait for the child with a timeout.
    let timeout = Duration::from_secs(DEFAULT_WORKER_TIMEOUT_SECS);
    let (status, stdout_bytes) = loop {
        match child.try_wait() {
            Ok(Some(status)) => {
                // Read stdout.
                let stdout = child
                    .stdout
                    .take()
                    .map(|mut s| {
                        let mut buf = Vec::new();
                        use std::io::Read;
                        let _ = s.read_to_end(&mut buf);
                        buf
                    })
                    .unwrap_or_default();
                break (status, stdout);
            }
            Ok(None) => {
                if t0.elapsed() > timeout {
                    let _ = child.kill();
                    let _ = child.wait();
                    return ModuleResult {
                        name: format!("chunk-worker-{}", worker_id),
                        exit_code: -4,
                        total_nodes: 0,
                        total_edges: 0,
                        files_indexed: 0,
                        candidate_files: 0,
                        time_parse_ms: 0,
                        duration_secs: t0.elapsed().as_secs(),
                        workers: 1,
                        db_path: worker_db.to_string(),
                        project_id,
                        error: Some(format!(
                            "timeout after {}s [module=scheduler, method=run_chunk_worker]",
                            DEFAULT_WORKER_TIMEOUT_SECS
                        )),
                    };
                }
                std::thread::sleep(Duration::from_millis(50));
            }
            Err(e) => {
                return ModuleResult {
                    name: format!("chunk-worker-{}", worker_id),
                    exit_code: -3,
                    total_nodes: 0,
                    total_edges: 0,
                    files_indexed: 0,
                    candidate_files: 0,
                    time_parse_ms: 0,
                    duration_secs: t0.elapsed().as_secs(),
                    workers: 1,
                    db_path: worker_db.to_string(),
                    project_id,
                    error: Some(format!(
                        "wait failed: {} [module=scheduler, method=run_chunk_worker]",
                        e
                    )),
                };
            }
        }
    };

    let duration = t0.elapsed().as_secs();
    let exit_code = status.code().unwrap_or(-4);
    let stdout = String::from_utf8_lossy(&stdout_bytes).to_string();
    let parsed = extract_worker_json(&stdout);

    let (total_nodes, total_edges, files_indexed, candidate_files, time_parse_ms) = match &parsed {
        Some(v) => (
            v["total_nodes"].as_u64().unwrap_or(0),
            v["total_edges"].as_u64().unwrap_or(0),
            v["files_indexed"].as_u64().unwrap_or(0),
            v["discovery"]["candidate_files"].as_u64().unwrap_or(0),
            v["time_parse_ms"].as_u64().unwrap_or(0),
        ),
        None => (0, 0, 0, 0, 0),
    };

    let error = if exit_code == 0 && parsed.is_some() {
        None
    } else {
        Some(format!(
            "exit={} [module=scheduler, method=run_chunk_worker]",
            exit_code
        ))
    };

    ModuleResult {
        name: format!("chunk-worker-{}", worker_id),
        exit_code,
        total_nodes,
        total_edges,
        files_indexed,
        candidate_files,
        time_parse_ms,
        duration_secs: duration,
        workers: 1,
        db_path: worker_db.to_string(),
        project_id,
        error,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_extract_worker_json_handles_trailing_noise() {
        let stdout = "log line 1\nlog line 2\n{\"ok\":true,\"total_nodes\":42}";
        let v = extract_worker_json(stdout).unwrap();
        assert_eq!(v["ok"], true);
        assert_eq!(v["total_nodes"], 42);
    }

    #[test]
    fn test_extract_worker_json_returns_none_for_no_json() {
        assert!(extract_worker_json("no json here").is_none());
    }

    #[test]
    fn test_extract_worker_json_handles_nested_objects() {
        let stdout = "{\"ok\":true,\"discovery\":{\"candidate_files\":100}}";
        let v = extract_worker_json(stdout).unwrap();
        assert_eq!(v["discovery"]["candidate_files"], 100);
    }

    #[test]
    fn test_make_relative_glob_returns_basename_glob() {
        let g = make_relative_glob("/abs/path/engine/src/parser.cpp", "/abs/path/engine");
        assert_eq!(g, "*/parser.cpp");
    }

    #[test]
    fn test_make_relative_glob_handles_empty_module_dir() {
        let g = make_relative_glob("/abs/path/file.rs", "");
        // When module_dir is "", strip_prefix fails and we fall back
        // to the absolute path. Both branches must produce a non-empty glob.
        assert!(!g.is_empty());
    }
}

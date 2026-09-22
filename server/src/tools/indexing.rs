// indexing.rs — indexing orchestration.
//
// Split out of tools/mod.rs (see plan/rules/code_rules.md 1000-line rule).
// Contains the worker subprocess runner (with timeout + orphan cleanup),
// the incremental file index entry point, the force-index file walker and
// its acceptability filter. These are kept together because they are one
// pipeline: `h_force_index_files` decides WHICH files to index and
// delegates the actual indexing to the same worker process that
// `h_index_file` uses for a single file.

use crate::ffi;
use once_cell::sync::Lazy;
use serde_json::Value;
use std::process::{Command, Stdio};
use std::sync::mpsc;
use std::time::Duration;

// ─── Worker Supervisor (timeout + retry) ───────────────────────
/// Default worker timeout in seconds when `CODESCOPE_WORKER_TIMEOUT` is unset.
const DEFAULT_WORKER_TIMEOUT_SECS: u64 = 300;
/// Interval between polls when waiting for a worker subprocess to finish.
const WORKER_POLL_INTERVAL: Duration = Duration::from_millis(100);

static WORKER_TIMEOUT: Lazy<Duration> = Lazy::new(|| {
    Duration::from_secs(
        std::env::var("CODESCOPE_WORKER_TIMEOUT")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(DEFAULT_WORKER_TIMEOUT_SECS),
    )
});
const MAX_RETRIES: usize = 3;

/// Number of attempts to re-initialise the C++ engine after a worker
/// run if the first `ffi::init` fails. The worker subprocess may hold
/// the SQLite WAL lock briefly or leave the DB in a busy state; a
/// short retry window recovers the engine instead of leaving the
/// server in an unusable "g_store == null" state where every
/// subsequent tool call returns "not initialised". See H4.
const ENGINE_INIT_MAX_ATTEMPTS: usize = 3;
/// Delay between engine re-init attempts. Chosen to be long enough
/// for the worker's WAL lock to release on a busy system but short
/// enough not to materially delay the index response.
const ENGINE_INIT_RETRY_DELAY: Duration = Duration::from_millis(500);

/// Run a worker subprocess with timeout protection.
/// Returns `Ok(output)` on success, `Err(msg)` on timeout or failure.
/// On timeout the orphaned child is killed using a platform-appropriate
/// method: `kill -9` on Unix, `taskkill /F` on Windows. On any other
/// platform the process is logged as orphaned (the `Child` handle was moved
/// into the wait thread and is no longer accessible here).
fn run_worker(
    exe: &str,
    args: &[&str],
    envs: &[(&str, &str)],
) -> Result<std::process::Output, String> {
    let mut cmd = Command::new(exe);
    cmd.args(args);
    for (k, v) in envs {
        cmd.env(k, v);
    }
    // Pipe stdout/stderr so the worker's output is captured instead of
    // leaking into the MCP server's stdout transport stream.
    cmd.stdout(Stdio::piped()).stderr(Stdio::piped());
    let child = cmd.spawn().map_err(|e| format!("spawn failed: {}", e))?;
    let pid = child.id();

    let (tx, rx) = mpsc::channel();

    // Thread: wait for child completion
    let tx_out = tx.clone();
    std::thread::spawn(move || {
        let output = child.wait_with_output();
        let _ = tx_out.send(output);
    });

    // Poll for result with timeout
    let start = std::time::Instant::now();
    loop {
        if start.elapsed() > *WORKER_TIMEOUT {
            // Kill orphaned child via a platform-appropriate method. The child
            // handle was moved into the wait thread above, so we cannot call
            // `child.kill()` here; instead we signal by PID.
            #[cfg(unix)]
            {
                let _ = Command::new("kill").args(["-9", &pid.to_string()]).output();
            }
            #[cfg(windows)]
            {
                let _ = Command::new("taskkill")
                    .args(["/F", "/PID", &pid.to_string()])
                    .output();
            }
            #[cfg(not(any(unix, windows)))]
            {
                eprintln!(
                    "warning: worker timeout on unsupported platform — process {} may be orphaned",
                    pid
                );
            }
            return Err(format!(
                "worker timed out after {}s",
                WORKER_TIMEOUT.as_secs()
            ));
        }

        match rx.try_recv() {
            Ok(Ok(output)) => return Ok(output),
            Ok(Err(e)) => return Err(format!("worker error: {}", e)),
            Err(mpsc::TryRecvError::Empty) => {
                std::thread::sleep(WORKER_POLL_INTERVAL);
            }
            Err(mpsc::TryRecvError::Disconnected) => {
                return Err("worker channel disconnected".to_string());
            }
        }
    }
}

/// Internal worker-subprocess indexer used by the MCP session auto-index
/// path. NOT registered as a public tool: index-parallel (with keep_db
/// incremental) fully replaces the serial index_project tool, so the
/// MCP tool list no longer exposes it (47→46 tools). The engine and the
/// worker subprocess entry point are shared with index-parallel and stay.
pub fn index_project_via_worker(project_id: u64, args: &Value) -> String {
    let path = args["project_path"].as_str().unwrap_or("");

    // Use worker subprocess for memory isolation
    let self_exe = std::env::current_exe().ok();
    if let Some(exe) = self_exe {
        let db_path = std::env::var("CODESCOPE_DB_PATH")
            .unwrap_or_else(|_| ".codescope/codescope.db".to_string());
        let grammars_dir = std::env::var("GRAMMARS_DIR").unwrap_or_else(|_| "grammars".to_string());
        let lang = args["language_filter"].as_str().unwrap_or("");

        // Derive a meaningful project name from the path's final component so the
        // worker records a human-readable name instead of the placeholder
        // "worker-project".
        let project_name = std::path::Path::new(path)
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("unnamed")
            .to_string();

        for attempt in 1..=MAX_RETRIES {
            // Shutdown engine before spawning worker to release SQLite lock
            crate::ffi::shutdown();

            let args_list = [
                "worker",
                &db_path,
                path,
                lang,
                &project_name,
                &project_id.to_string(),
            ];
            let envs = [
                ("GRAMMARS_DIR", &grammars_dir as &str),
                ("CODESCOPE_DB_PATH", &db_path as &str),
                ("CODESCOPE_VERBOSE", "0"),
            ];

            let result = run_worker(exe.to_str().unwrap_or("codescope"), &args_list, &envs);

            // Re-init engine after worker completes (regardless of success/failure).
            // The worker subprocess may have left the SQLite WAL lock held briefly
            // or the DB may be corrupted; checking the return value prevents the
            // server from silently running with a null g_store for the rest of its
            // lifetime (every subsequent tool call would return "not initialized").
            //
            // H4: A single failed init must NOT leave the server in an unusable
            // state. We retry with the original (pre-shutdown) db_path a few
            // times with a short delay — this recovers the engine when the
            // failure was transient (e.g. WAL lock not yet released). Only if
            // every attempt fails do we propagate the error; the caller (and
            // the operator) must restart the server in that case.
            let mut last_init_code: i32 = 0;
            let mut engine_recovered = false;
            for init_attempt in 1..=ENGINE_INIT_MAX_ATTEMPTS {
                let code = crate::ffi::init(&db_path);
                if code == 0 {
                    engine_recovered = true;
                    break;
                }
                last_init_code = code;
                if init_attempt < ENGINE_INIT_MAX_ATTEMPTS {
                    eprintln!(
                        "engine re-init attempt {}/{} failed (code={}); retrying in {:?} [module=mcp, tool=index_project, method=ffi::init]",
                        init_attempt, ENGINE_INIT_MAX_ATTEMPTS, code, ENGINE_INIT_RETRY_DELAY
                    );
                    std::thread::sleep(ENGINE_INIT_RETRY_DELAY);
                }
            }
            if !engine_recovered {
                return format!(
                    "{{\"ok\":false,\"error\":\"engine re-initialization failed after index (code={}, attempts={}). The database may be locked or corrupted; the engine is now uninitialized — restart the server before issuing further tool calls. [module=mcp, tool=index_project, method=ffi::init]\"}}",
                    last_init_code, ENGINE_INIT_MAX_ATTEMPTS
                );
            }

            match result {
                Ok(out) => {
                    if out.status.success() {
                        let stdout = String::from_utf8_lossy(&out.stdout);
                        // Trigger background FTS build after a successful index.
                        // Uses spawn_fts_build to deduplicate concurrent builds
                        // and avoid a data race on the global C++ g_store.
                        if let Some(json_start) = stdout.find('{')
                            && let Some(json_end) = stdout[json_start..].rfind('}')
                        {
                            let candidate = &stdout[json_start..=json_start + json_end];
                            // Validate the slice is well-formed JSON before returning
                            // it, so malformed worker output does not produce invalid
                            // JSON that would confuse the MCP client.
                            if serde_json::from_str::<serde_json::Value>(candidate).is_ok() {
                                crate::ffi::spawn_fts_build(project_id);
                                return candidate.to_string();
                            }
                        }
                        crate::ffi::spawn_fts_build(project_id);
                        return stdout.to_string();
                    }
                    let stderr = String::from_utf8_lossy(&out.stderr);
                    let err_msg = stderr.lines().last().unwrap_or("unknown");
                    if attempt < MAX_RETRIES {
                        eprintln!(
                            "worker attempt {} failed ({}), retrying...",
                            attempt, err_msg
                        );
                        std::thread::sleep(Duration::from_secs(1));
                        continue;
                    }
                    return format!(
                        "{{\"ok\":false,\"error\":\"worker failed after {} attempts: {}\"}}",
                        MAX_RETRIES, err_msg
                    );
                }
                Err(msg) => {
                    if attempt < MAX_RETRIES {
                        eprintln!("worker attempt {}: {} — retrying...", attempt, msg);
                        std::thread::sleep(Duration::from_secs(1));
                        continue;
                    }
                    return format!(
                        "{{\"ok\":false,\"error\":\"worker {} after {} attempts\"}}",
                        msg, MAX_RETRIES
                    );
                }
            }
        }
    }

    // Fallback: in-process indexing with correct null handling for missing lang filter
    // Fix: Use CString to ensure null-termination for FFI compatibility
    let lang = args["language_filter"].as_str();
    let lang_cstring = lang.map(|s| std::ffi::CString::new(s).unwrap_or_default());
    let lang_ptr = lang_cstring
        .as_ref()
        .map_or(std::ptr::null(), |cs| cs.as_ptr() as *const _);
    ffi::index_project(project_id, path, lang_ptr)
}

pub(super) fn h_index_file(project_id: u64, args: &Value) -> String {
    let path = args["file_path"].as_str().unwrap_or("");
    ffi::index_file(project_id, path)
}

/// Force-index specific files or directories, bypassing FilterPolicy's
/// default skip rules (test/, docs/, vendored/, node_modules/, etc.).
///
/// Use case: user says "go index xxx/yyy for me" — the AI calls this
/// tool with paths=[...]. Files under the given paths are indexed
/// regardless of the default skip list, so the user can pull in
/// test fixtures, vendored deps, or generated code on demand.
///
/// Args:
///   paths: array of absolute file/dir paths to force-index.
///   language_filter: optional comma-separated language whitelist
///     (e.g. "java,python"). When empty, all detectable languages
///     are indexed.
///
/// Returns the engine_index_files JSON result
/// (files_indexed/nodes/edges/errors).
pub(super) fn h_force_index_files(project_id: u64, args: &Value) -> String {
    // Collect paths: accept either `paths: [...]` or legacy
    // `path: "..."` for single-path convenience.
    let mut paths: Vec<String> = Vec::new();
    if let Some(arr) = args["paths"].as_array() {
        for v in arr {
            if let Some(s) = v.as_str()
                && !s.is_empty()
            {
                paths.push(s.to_string());
            }
        }
    }
    if let Some(s) = args["path"].as_str()
        && !s.is_empty()
    {
        paths.push(s.to_string());
    }
    if paths.is_empty() {
        return "{\"ok\":false,\"error\":\"paths is required (array of file/dir paths)\"}"
            .to_string();
    }

    let lang_filter = args["language_filter"].as_str().unwrap_or("");

    // Expand directories into individual file paths, bypassing
    // FilterPolicy's shouldSkipEntry. We DO still respect:
    //   - file size limit (CODESCOPE_MAX_FILE_SIZE, default 5MB)
    //   - language detectability (detectLanguage must return non-null)
    //   - optional language_filter whitelist
    // We DO NOT respect:
    //   - normal_skip_dirs_ / top_only_skip_dirs_ (test/, docs/, ...)
    //   - skip_suffixes_ for source extensions
    //   - .gitignore / .codescopeignore
    // This is the "user override" path.
    let max_size: u64 = std::env::var("CODESCOPE_MAX_FILE_SIZE")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(5 * 1024 * 1024);

    let mut lang_whitelist: Option<std::collections::HashSet<String>> = None;
    if !lang_filter.is_empty() {
        let mut set = std::collections::HashSet::new();
        for part in lang_filter.split(',') {
            let p = part.trim().to_lowercase();
            if !p.is_empty() {
                set.insert(p);
            }
        }
        lang_whitelist = Some(set);
    }

    let mut all_files: Vec<String> = Vec::new();
    let mut skipped_files = 0u64;
    let mut skipped_dirs = 0u64;

    for p in &paths {
        let path = std::path::Path::new(p);
        if !path.exists() {
            skipped_files += 1;
            continue;
        }
        if path.is_file() {
            // Single file — index directly if detectable.
            if let Some(fp) = filter_acceptable_file(path, max_size, lang_whitelist.as_ref()) {
                all_files.push(fp);
            } else {
                skipped_files += 1;
            }
            continue;
        }
        // Directory — walk it, bypassing skip-dir rules but still
        // respecting file-level detectability + size. Start at depth 0.
        walk_force_index(
            path,
            max_size,
            lang_whitelist.as_ref(),
            &mut all_files,
            &mut skipped_files,
            &mut skipped_dirs,
            0,
        );
    }

    if all_files.is_empty() {
        return format!(
            "{{\"ok\":true,\"files_indexed\":0,\"nodes\":0,\"edges\":0,\"errors\":0,\"skipped_files\":{},\"skipped_dirs\":{}}}",
            skipped_files, skipped_dirs
        );
    }

    // Build JSON file list and call engine_index_files.
    let json_list = serde_json::Value::Array(
        all_files
            .iter()
            .map(|s| serde_json::Value::String(s.clone()))
            .collect(),
    )
    .to_string();

    let result = ffi::index_files(project_id, &json_list);

    // Annotate result with skip stats for transparency.
    if let Ok(mut v) = serde_json::from_str::<serde_json::Value>(&result) {
        if let Some(obj) = v.as_object_mut() {
            obj.insert("skipped_files".into(), skipped_files.into());
            obj.insert("skipped_dirs".into(), skipped_dirs.into());
            obj.insert("paths_requested".into(), paths.len().into());
        }
        return v.to_string();
    }
    result
}

/// Maximum recursion depth for `walk_force_index`. A deeply nested directory
/// tree (e.g. a chain of node_modules) or a crafted path would otherwise
/// consume the call stack until it overflows, panicking and crashing the
/// MCP server (local DoS, see H-B). 256 levels is far deeper than any
/// legitimate project tree; once reached we stop descending and log a
/// warning so the walk always terminates.
const MAX_WALK_DEPTH: u32 = 256;

/// Source extensions recognised by the C++ FilterPolicy::detectLanguage.
/// Mirrored here so the force-index walk can decide which files to
/// accept without crossing the FFI boundary for every entry.
const SOURCE_EXTENSIONS: &[&str] = &[
    ".py", ".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hxx", ".hh", ".rs", ".swift", ".js",
    ".mjs", ".cjs", ".ts", ".tsx", ".go", ".java", ".kt", ".kts", ".rb", ".scala",
];

/// Check a single file path against force-index rules:
///   - must exist and be a regular file
///   - must be within max_size
///   - extension must be a recognised source extension
///   - must pass the optional language whitelist
///
/// Returns the absolute path string if acceptable, None otherwise.
fn filter_acceptable_file(
    path: &std::path::Path,
    max_size: u64,
    lang_whitelist: Option<&std::collections::HashSet<String>>,
) -> Option<String> {
    let meta = match std::fs::metadata(path) {
        Ok(m) => m,
        Err(_) => return None,
    };
    if !meta.is_file() {
        return None;
    }
    if meta.len() > max_size {
        return None;
    }

    // Extension check (case-insensitive)
    let ext = format!(
        ".{}",
        path.extension().and_then(|e| e.to_str())?.to_lowercase()
    );
    if !SOURCE_EXTENSIONS.iter().any(|&s| s == ext) {
        return None;
    }

    // Language whitelist (maps extension -> language label)
    if let Some(wl) = lang_whitelist {
        let lang = match ext.as_str() {
            ".py" => "python",
            ".cpp" | ".cc" | ".cxx" | ".h" | ".hpp" | ".hxx" | ".hh" => "cpp",
            ".c" => "c",
            ".rs" => "rust",
            ".swift" => "swift",
            ".js" | ".mjs" | ".cjs" => "javascript",
            ".ts" => "typescript",
            ".tsx" => "tsx",
            ".go" => "go",
            ".java" => "java",
            ".kt" | ".kts" => "kotlin",
            ".rb" => "ruby",
            ".scala" => "scala",
            _ => "",
        };
        if !lang.is_empty() && !wl.contains(lang) {
            return None;
        }
    }

    // Return absolute path
    let canon = std::fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf());
    Some(canon.to_string_lossy().to_string())
}

/// Recursively walk `root` for force-index. Bypasses all skip-dir
/// rules (test/, docs/, vendored/, node_modules/, etc.) — this is
/// the whole point of the force-index tool. Still respects:
///   - file size limit
///   - extension detectability
///   - optional language whitelist
///
/// `depth` is the current recursion depth (call with 0 at the top).
/// Recursion stops once `depth >= MAX_WALK_DEPTH` to guarantee the walk
/// always terminates and cannot exhaust the stack (H-B). When the limit
/// is hit, a warning is logged and descent into that subtree is skipped.
fn walk_force_index(
    root: &std::path::Path,
    max_size: u64,
    lang_whitelist: Option<&std::collections::HashSet<String>>,
    out_files: &mut Vec<String>,
    skipped_files: &mut u64,
    skipped_dirs: &mut u64,
    depth: u32,
) {
    let entries = match std::fs::read_dir(root) {
        Ok(e) => e,
        Err(_) => {
            *skipped_dirs += 1;
            return;
        }
    };
    for entry in entries.flatten() {
        let path = entry.path();
        // Use symlink_metadata (NOT following symlinks) to detect the
        // entry's own type. Path::is_dir() follows symlinks, so a symlink
        // loop (a -> b -> a) would make is_dir() always return true and
        // the recursion would never terminate → stack overflow panic
        // crashing the MCP server (local DoS). symlink_metadata gives us
        // the link's own metadata without dereferencing, so we only recurse
        // into real directories.
        let is_real_dir = std::fs::symlink_metadata(&path)
            .map(|m| m.is_dir())
            .unwrap_or(false);
        if is_real_dir {
            // Guard against unbounded recursion: a pathologically deep
            // directory tree would otherwise overflow the stack (H-B).
            if depth >= MAX_WALK_DEPTH {
                eprintln!(
                    "warning: walk_force_index reached max depth {} at {:?}; skipping subtree to avoid stack overflow [module=mcp, tool=force_index_files, method=walk_force_index]",
                    MAX_WALK_DEPTH, path
                );
                *skipped_dirs += 1;
                continue;
            }
            // Recurse unconditionally — bypass skip-dir rules.
            walk_force_index(
                &path,
                max_size,
                lang_whitelist,
                out_files,
                skipped_files,
                skipped_dirs,
                depth + 1,
            );
            continue;
        }
        if path.is_file() {
            match filter_acceptable_file(&path, max_size, lang_whitelist) {
                Some(fp) => out_files.push(fp),
                None => *skipped_files += 1,
            }
        }
    }
}

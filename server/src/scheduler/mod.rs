//! Built-in parallel scheduler — replaces `codescope-parallel.sh`.
//!
//! Design principle (see `builtin-scheduler-design.md` §4.1):
//! > The scheduler only manages CPU core allocation; it does NOT
//! > participate in file discovery, parsing, or graph building.
//!
//! CPU core reclaim strategy:
//! The scheduler uses a bounded-concurrency worker pool where each
//! worker thread pulls the next pending module from a shared queue.
//! When a worker finishes its module, it immediately picks up the
//! next one — no kill/restart churn (which would cause SQLite WAL
//! contention per the script's notes). This achieves core reclaim at
//! module granularity: small modules free their cores for the next
//! pending module, while large modules retain their proportional
//! allocation until completion. Within a single module, the engine's
//! internal thread pool handles parse parallelism (CODESCOPE_WORKERS).
//!
//! Concretely, the scheduler:
//! 1. Calls `discover::discover_modules()` to get a list of top-level
//!    modules and their (approximate) source-file counts.
//! 2. Allocates parse-worker cores proportionally to each module's
//!    file count — `ceil(files * total_workers / total_files)`, min 1.
//! 3. Spawns one `codescope worker` subprocess per module, bounded by
//!    `--parallel M` concurrent modules. Each worker gets its own DB
//!    file so concurrent SQLite writers never contend.
//!    Workers run with CODESCOPE_SKIP_ASYNC=1 to skip the ~280ms
//!    state-builder work during the parallel phase; the unified DB
//!    is built by the final merge step.
//! 4. For modules that crash or produce zero nodes, runs a binary
//!    search via `discover::discover_files()` + `worker --file-list`
//!    to localise the crashing file, then retries the module with the
//!    crashing file excluded via `CODESCOPE_EXCLUDE_PATHS`.
//! 5. Merges per-module DBs into a single main DB via sqlite3 CLI
//!    (`ATTACH` + `INSERT OR IGNORE`) so the caller gets a unified DB.
//! 6. Returns a single JSON summary aggregating per-module stats.
//!
//! The scheduler never duplicates file-discovery logic: it leans on
//! `discover::*` for module/file listing and the worker's C++
//! `FilterPolicy` for the authoritative skip rules.

// dyn_config is wired into the dispatch path via DynSchedConfig.
// See index_parallel() -> DynSchedConfig::should_enable() and
// index_parallel_dynamic() -> DynSchedConfig::from_env() usage.
mod chunk_plan;
pub mod chunk_queue;
mod dyn_config;
mod merge;
mod quarantine;
mod shm;
mod worker;

use dyn_config::{DynSchedConfig, sample_total_rss_mb};
use serde_json::{Value, json};
use shm::SchedShm;
use std::collections::VecDeque;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, mpsc};
use std::time::{Duration, Instant};

use crate::discover;
use merge::MergeResult;
use worker::run_module_worker;

/// Default total worker cores when `--workers` is not specified.
const DEFAULT_TOTAL_WORKERS: u32 = 8;

/// Default max concurrent modules when `--parallel` is not specified.
const DEFAULT_PARALLEL: u32 = 4;

/// Poll interval when waiting for a free concurrency slot or worker exit.
/// 50ms keeps idle CPU low while bounding latency on module completion.
const POLL_INTERVAL: Duration = Duration::from_millis(50);

/// Per-worker subprocess timeout. Workers normally finish in seconds;
/// 600s matches the script's hard cap and guards against stuck parses.
const DEFAULT_WORKER_TIMEOUT_SECS: u64 = 600;

/// Maximum quarantine iterations per module. Each iteration should
/// localise one crashing file; 10 is more than enough for any real
/// project (the script also uses 10).
const QUARANTINE_MAX_ITER: u32 = 10;

/// Result of running one module worker.
#[derive(Clone, Debug)]
pub(super) struct ModuleResult {
    pub name: String,
    pub exit_code: i32,
    pub total_nodes: u64,
    pub total_edges: u64,
    pub files_indexed: u64,
    pub candidate_files: u64,
    pub time_parse_ms: u64,
    pub duration_secs: u64,
    pub workers: u32,
    pub db_path: String,
    /// Scheduler-assigned project_id (1, 2, 3, ...). Stored on the
    /// result so the Phase 4 retry can reuse the SAME project_id the
    /// original worker used — without this, retry would recompute
    /// project_id from the completion-order index and collide with
    /// another module's id during merge.
    pub project_id: u64,
    pub error: Option<String>,
}

/// Whether the LadybugDB graph rebuild can be skipped for an incremental
/// no-change run.
///
/// v0.6 (perf): when `keep_db` (incremental mode) is active and no file was
/// re-indexed this run (`total_files_indexed == 0`), the module DBs were not
/// modified, so the freshly merged entity/relation tables are byte-identical
/// to the prior run and the existing `.lbug` (if present) is still valid.
/// Rebuilding it would be pure wasted work (rust: 23.8s). The existing `.lbug`
/// fingerprint check in initLadybugDB drops+rebuilds on the next open if the
/// stored fingerprint ever mismatches the live SQLite totals, so no precision
/// can be lost even if we skip here.
///
/// @param keep_db              True if this is an incremental (prefix) run.
/// @param total_files_indexed  Total files re-indexed across all modules.
/// @param lbug_exists          True if the .lbug file already exists on disk.
fn should_skip_graph_rebuild(keep_db: bool, total_files_indexed: u64, lbug_exists: bool) -> bool {
    keep_db && total_files_indexed == 0 && lbug_exists
}

/// Rebuild each project's LadybugDB (.lbug) graph on the merged SQLite DB.
///
/// v0.6 (perf): see `should_skip_graph_rebuild` — in incremental no-change
/// runs the rebuild is skipped and the existing `.lbug` (validated by its
/// fingerprint on next open) is reused.
///
/// @param main_db              Path to the merged main.db.
/// @param keep_db              True if this is an incremental (prefix) run.
/// @param total_files_indexed  Total files re-indexed across all modules.
/// @return Number of projects whose graph was rebuilt (0 if skipped).
fn rebuild_ladybug_graphs_if_needed(
    main_db: &str,
    keep_db: bool,
    total_files_indexed: u64,
) -> usize {
    // Derive the .lbug path exactly as the engine does (store_ladybug_core.cpp
    // initLadybugDB): replace everything after the last '.' with ".lbug",
    // or append ".lbug" if there is no '.'. Must match or the existence
    // check below won't reflect the real .lbug on disk.
    let lbug_path = match main_db.rfind('.') {
        Some(dot) => format!("{}.lbug", &main_db[..dot]),
        None => format!("{}.lbug", main_db),
    };
    let skip_graph_rebuild = should_skip_graph_rebuild(
        keep_db,
        total_files_indexed,
        std::path::Path::new(&lbug_path).exists(),
    );
    if skip_graph_rebuild {
        eprintln!(
            "[scheduler] keep_db incremental with no file changes; skipping \
             LadybugDB graph rebuild for {} (existing .lbug stays valid) \
             [module=scheduler, method=rebuild_ladybug_graphs_if_needed]",
            main_db
        );
        return 0;
    }
    match list_project_ids(main_db) {
        Ok(ids) => {
            let mut rebuilt = 0;
            for pid in &ids {
                match crate::ffi::rebuild_ladybug_graph(main_db, *pid) {
                    Ok(()) => rebuilt += 1,
                    Err(e) => eprintln!(
                        "[scheduler] rebuild graph failed for project {}: {}",
                        pid, e
                    ),
                }
            }
            eprintln!(
                "[scheduler] rebuilt LadybugDB graphs for {}/{} projects",
                rebuilt,
                ids.len()
            );
            rebuilt
        }
        Err(e) => {
            eprintln!(
                "[scheduler] could not enumerate projects in {}: {}",
                main_db, e
            );
            0
        }
    }
}

/// Entry point: discover modules, dispatch workers, quarantine failures,
/// return aggregated JSON summary.
///
/// `project_dir` — root directory to index (must exist).
/// `total_workers` — total parse-worker cores to distribute across modules.
///                   0 → use `DEFAULT_TOTAL_WORKERS`.
/// `parallel` — max concurrent module workers. 0 → use `DEFAULT_PARALLEL`.
///
/// Output JSON schema:
/// ```json
/// {"ok":true,"project_path":"...","total_workers":N,"parallel":N,
///  "duration_ms":N,"success":N,"fail":N,"total_nodes":N,"total_edges":N,
///  "total_files_indexed":N,"modules":[{...per-module...}]}
/// ```
///
/// Enumerate the project ids present in a merged parallel-index DB so the
/// caller can rebuild each project's LadybugDB graph after the SQLite merge.
/// Uses the sqlite3 CLI (consistent with `merge::merge_module_dbs` — the
/// scheduler deliberately avoids a rusqlite dependency). Returns the ids in
/// ascending order; on any failure returns `Err` with a descriptive message.
///
/// NOTE: the merged main.db deliberately omits bookkeeping tables — it only
/// carries the data tables (entity/relation/reference/semantic_records/...),
/// so `projects` does not exist there. Project ids are therefore enumerated
/// from `entity.project_id` (every module writes entities; a project with
/// zero entities contributes nothing to the graph anyway). Fall back to
/// `semantic_records` if entity is somehow empty.
fn list_project_ids(db_path: &str) -> Result<Vec<u64>, String> {
    let query = "SELECT DISTINCT project_id FROM entity ORDER BY project_id;";
    let output = std::process::Command::new("sqlite3")
        .arg(db_path)
        .arg(query)
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .output();
    let output = output.map_err(|e| format!("failed to run sqlite3: {}", e))?;
    let mut ids = Vec::new();
    if output.status.success() {
        let stdout = String::from_utf8_lossy(&output.stdout);
        for line in stdout.lines() {
            let line = line.trim();
            if line.is_empty() {
                continue;
            }
            match line.parse::<u64>() {
                Ok(id) => ids.push(id),
                Err(e) => eprintln!(
                    "[scheduler] list_project_ids: non-numeric project id '{}': {}",
                    line, e
                ),
            }
        }
    } else {
        // Fallback: entity table may be empty in unusual cases; try
        // semantic_records before giving up.
        let fallback = "SELECT DISTINCT project_id FROM semantic_records ORDER BY project_id;";
        let out2 = std::process::Command::new("sqlite3")
            .arg(db_path)
            .arg(fallback)
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped())
            .output();
        let out2 = out2.map_err(|e| format!("failed to run sqlite3: {}", e))?;
        if !out2.status.success() {
            return Err(format!(
                "sqlite3 failed ({}): {}",
                out2.status,
                String::from_utf8_lossy(&out2.stderr)
            ));
        }
        let stdout = String::from_utf8_lossy(&out2.stdout);
        for line in stdout.lines() {
            let line = line.trim();
            if line.is_empty() {
                continue;
            }
            match line.parse::<u64>() {
                Ok(id) => ids.push(id),
                Err(e) => eprintln!(
                    "[scheduler] list_project_ids: non-numeric project id '{}': {}",
                    line, e
                ),
            }
        }
    }
    Ok(ids)
}

pub fn index_parallel(project_dir: &str, total_workers: u32, parallel: u32) -> String {
    let start = Instant::now();

    let total_workers = if total_workers == 0 {
        DEFAULT_TOTAL_WORKERS
    } else {
        total_workers
    };
    let parallel = if parallel == 0 {
        DEFAULT_PARALLEL
    } else {
        parallel
    };

    // Resolve project_dir to an absolute path so workers inherit a
    // consistent working directory regardless of how the caller invoked us.
    let project_path_buf = match canonicalize_project_dir(project_dir) {
        Ok(p) => p,
        Err(e) => return error_json(&e, "scheduler", "index_parallel"),
    };
    let project_path = project_path_buf.to_string_lossy().to_string();

    // Locate the codescope binary: prefer $CODESCOPE_BIN, then
    // std::env::current_exe() so a release build can spawn itself.
    let exe_path = match resolve_self_exe() {
        Ok(p) => p,
        Err(e) => return error_json(&e, "scheduler", "index_parallel"),
    };
    let exe_str = exe_path.to_string_lossy().to_string();

    let grammars_dir =
        std::env::var("GRAMMARS_DIR").unwrap_or_else(|_| "engine/grammars".to_string());

    // Generate a per-run DB prefix in /tmp so each invocation starts
    // fresh and parallel runs never collide on DB files.
    let run_id = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    // When CODESCOPE_DB_PREFIX is explicitly pinned, treat this as an
    // incremental run: keep module/main DBs so the engine's
    // file_scan_state mtime check skips unchanged files on the next pass.
    // Otherwise (auto run_id prefix) always start clean.
    let prefix_env = std::env::var("CODESCOPE_DB_PREFIX");
    let keep_db = prefix_env.is_ok();
    let db_prefix = prefix_env.unwrap_or_else(|_| format!("/tmp/codescope_parallel_{}", run_id));

    eprintln!(
        "scheduler: project={} workers={} parallel={} db_prefix={} keep_db={}",
        project_path, total_workers, parallel, db_prefix, keep_db
    );

    // ── Phase 1: discover modules ──────────────────────────────
    let discover_json = discover::discover_modules(&project_path);
    let discover_val: Value = match serde_json::from_str(&discover_json) {
        Ok(v) => v,
        Err(e) => {
            return error_json(
                &format!(
                    "discover parse failed: {} [module=scheduler, method=index_parallel]",
                    e
                ),
                "scheduler",
                "index_parallel",
            );
        }
    };
    if discover_val["ok"] != true {
        return discover_json;
    }

    let modules = match discover_val["modules"].as_array() {
        Some(a) if !a.is_empty() => a,
        _ => {
            return json!({
                "ok": true,
                "project_path": project_path,
                "duration_ms": start.elapsed().as_millis() as u64,
                "success": 0,
                "fail": 0,
                "total_nodes": 0,
                "total_edges": 0,
                "total_files_indexed": 0,
                "modules": [],
                "note": "no source modules found"
            })
            .to_string();
        }
    };

    let total_files_sum: u64 = modules
        .iter()
        .filter_map(|m| m["files"].as_u64())
        .sum::<u64>()
        .max(1);

    // ── Dispatch: STATIC by default ──────────────────────────
    // The project's scheduling principle is "static analysis by default;
    // CPU-dynamic scheduling only when an explicit flag is set". We
    // therefore NEVER auto-enable dynamic mode for large projects — the
    // flag (`CODESCOPE_CPU_DYNAMIC` / `CODESCOPE_DYNAMIC_SCHED` = 1/true/
    // on) is the ONLY trigger. Without it we stay on the static
    // proportional allocator below (no shm overhead, no work-stealing).
    let dyn_config = dyn_config::DynSchedConfig::from_env();
    let cpu_dynamic = dyn_config.force_on == Some(true);
    if cpu_dynamic {
        eprintln!(
            "scheduler: CPU-dynamic (chunked work-stealing) enabled via flag [module=scheduler, method=index_parallel]"
        );
        // The chunk-level scheduler (chunk queue + work-stealing) is the
        // stable, current implementation of CPU-dynamic scheduling. It
        // replaces the old shm-core-pool `index_parallel_dynamic`, which
        // is retained only as dead-code reference (DS-1 deadlock risk).
        return index_parallel_chunked(project_dir, total_workers, parallel);
    }

    // ── Phase 2: proportional worker allocation ────────────────
    // ceil(files * total_workers / total_files_sum), min 1. Modules
    // are already sorted by size descending by discover_modules(),
    // so the largest module dispatches first — better CPU utilisation
    // when --parallel is smaller than the module count.
    let allocations: Vec<(String, u64, u32)> = modules
        .iter()
        .filter_map(|m| {
            let name = m["name"].as_str()?.to_string();
            let files = m["files"].as_u64().unwrap_or(0);
            let alloc = if files == 0 {
                1
            } else {
                let raw = (files as u128 * total_workers as u128).div_ceil(total_files_sum as u128);
                let a = u32::try_from(raw as u64).unwrap_or(total_workers);
                std::cmp::max(1, std::cmp::min(a, total_workers))
            };
            Some((name, files, alloc))
        })
        .collect();

    eprintln!(
        "scheduler: {} modules, total_files={}, allocations={:?}",
        allocations.len(),
        total_files_sum,
        allocations
            .iter()
            .map(|(n, f, w)| format!("{}:{}/{}", n, f, w))
            .collect::<Vec<_>>()
    );

    // ── Phase 3: dispatch workers with bounded concurrency ─────
    // Each module gets a unique project_id (1, 2, 3, ...) so the merge
    // step can disambiguate rows across modules even when their auto-
    // incremented ids collide (each module DB starts graph_nodes.id
    // from 1). The merge step remaps ids per module to avoid collisions.
    let (tx, rx) = mpsc::channel::<ModuleResult>();
    let active = Arc::new(AtomicU32::new(0));
    let mut handles = Vec::new();

    for (idx, (name, files, alloc)) in allocations.iter().enumerate() {
        // Wait for a free slot if at concurrency cap. The poll loop
        // matches the script's simple token-gate pattern; `wait -n`
        // in bash is replaced by channel + atomic counter here.
        while active.load(Ordering::SeqCst) >= parallel {
            std::thread::sleep(POLL_INTERVAL);
        }
        active.fetch_add(1, Ordering::SeqCst);

        // project_id is 1-indexed (project_id=0 would cause the worker
        // to call create_project and auto-assign a fresh id, defeating
        // the unique-project_id scheme).
        let project_id = (idx as u64) + 1;

        // Clone the Arc for THIS closure so `active` is not moved out of
        // the loop. Each spawned thread gets its own strong reference.
        let active_clone = Arc::clone(&active);
        let tx = tx.clone();
        let exe_str = exe_str.clone();
        let project_path = project_path.clone();
        let grammars_dir = grammars_dir.clone();
        let db_prefix = db_prefix.clone();
        let name = name.clone();
        let files = *files;
        let alloc = *alloc;

        let handle = std::thread::spawn(move || {
            let result = run_module_worker(
                &exe_str,
                &project_path,
                &name,
                files,
                alloc,
                &grammars_dir,
                &db_prefix,
                project_id,
                None, // no quarantine initially
                keep_db,
            );
            let _ = tx.send(result);
            active_clone.fetch_sub(1, Ordering::SeqCst);
        });
        handles.push(handle);
    }

    drop(tx); // close sender so rx.recv() returns Err on completion

    let mut results: Vec<ModuleResult> = Vec::new();
    while let Ok(r) = rx.recv() {
        results.push(r);
    }
    for h in handles {
        let _ = h.join();
    }

    // ── Phase 4: quarantine failed modules ────────────────────
    // A module is "failed" if its exit code is non-zero OR it produced
    // zero nodes. For each failure, run binary search via discover_files
    // + worker --file-list to localise the crashing file, then retry
    // the module with that file excluded.
    //
    // The retry re-uses the module's original project_id so the merge
    // step still sees a contiguous 1..N project_id range. Modules that
    // never succeed keep their project_id slot (the merge just sees no
    // data for that slot).
    let mut final_results: Vec<ModuleResult> = Vec::new();
    for r in results {
        if r.exit_code == 0 && (r.total_nodes > 0 || r.files_indexed == 0) {
            final_results.push(r);
            continue;
        }
        eprintln!(
            "scheduler: module {} failed (exit={}, nodes={}) — starting quarantine",
            r.name, r.exit_code, r.total_nodes
        );
        let quarantined = quarantine::quarantine_module(
            &exe_str,
            &project_path,
            &r.name,
            &grammars_dir,
            &db_prefix,
        );
        if quarantined.is_empty() {
            final_results.push(r);
            continue;
        }
        // Retry the full module with the quarantine list applied via
        // CODESCOPE_EXCLUDE_PATHS (the worker's FilterPolicy already
        // honours this env var — see filter_policy_ignore.cpp:184).
        // Reuse the original worker's project_id so the merge step
        // still sees a contiguous 1..N project_id range — recomputing
        // it from the completion-order `idx` would collide with another
        // module's id (Phase 3 collects results in completion order, not
        // allocation order).
        let excluded_env = quarantined.join(",");
        let retry = run_module_worker(
            &exe_str,
            &project_path,
            &r.name,
            r.files_indexed,
            1, // single worker for retry — matches script's `index_module "0" "1"`
            &grammars_dir,
            &db_prefix,
            r.project_id,
            Some(&excluded_env),
            keep_db,
        );
        final_results.push(retry);
    }

    // ── Phase 5: aggregate summary ────────────────────────────
    let success = final_results
        .iter()
        .filter(|r| r.exit_code == 0 && (r.total_nodes > 0 || r.files_indexed == 0))
        .count();
    let fail = final_results.len() - success;
    let total_nodes: u64 = final_results.iter().map(|r| r.total_nodes).sum();
    let total_edges: u64 = final_results.iter().map(|r| r.total_edges).sum();
    let total_files_indexed: u64 = final_results.iter().map(|r| r.files_indexed).sum();

    // ── Phase 6: merge per-module DBs into unified main DB ────
    // Each worker wrote to its own DB (CODESCOPE_SKIP_ASYNC=1, no
    // async state-builder work). We now ATTACH each module DB to a
    // fresh main DB and INSERT OR IGNORE the rows so the caller gets
    // a single unified DB. The merge uses sqlite3 CLI (no new Rust
    // deps). Per-module project_ids are preserved so cross-module
    // queries can disambiguate via project_id.
    let main_db = format!("{}_main.db", db_prefix);
    // main.db is ALWAYS rebuilt from the module DBs below — keep_db only
    // preserves the per-module DBs so workers can skip unchanged files.
    // Keeping main.db too would double-count rows on INSERT OR IGNORE.
    let _ = std::fs::remove_file(&main_db);
    let _ = std::fs::remove_file(format!("{}-wal", main_db));
    let _ = std::fs::remove_file(format!("{}-shm", main_db));

    let module_db_paths: Vec<String> = final_results
        .iter()
        .filter(|r| r.exit_code == 0 && (r.total_nodes > 0 || r.files_indexed == 0))
        .map(|r| r.db_path.clone())
        .collect();

    let merge_result = if module_db_paths.is_empty() {
        MergeResult {
            merged: false,
            main_db_path: main_db.clone(),
            tables_merged: 0,
            rows_merged: 0,
            duration_ms: 0,
            error: Some("no successful module DBs to merge".to_string()),
        }
    } else {
        merge::merge_module_dbs(&main_db, &module_db_paths)
    };

    // ── Rebuild LadybugDB graphs on the merged DB ─────────────────
    // Parallel workers run with CODESCOPE_SKIP_ASYNC=1, so they never build
    // the LadybugDB graph (.lbug); the merge above only stitches SQLite
    // tables. Without this step the merged main.db has no graph-native data,
    // so find_callers/graph_query etc. would need a separate serial re-index.
    // v0.6 (perf): helper skips the rebuild in incremental no-change runs,
    // relying on the .lbug fingerprint check to stay consistent with SQLite.
    if merge_result.merged {
        rebuild_ladybug_graphs_if_needed(&main_db, keep_db, total_files_indexed);
    }

    let modules_json: Vec<Value> = final_results
        .iter()
        .map(|r| {
            json!({
                "name": r.name,
                "exit_code": r.exit_code,
                "files_indexed": r.files_indexed,
                "candidate_files": r.candidate_files,
                "total_nodes": r.total_nodes,
                "total_edges": r.total_edges,
                "time_parse_ms": r.time_parse_ms,
                "duration_secs": r.duration_secs,
                "workers": r.workers,
                "db_path": r.db_path,
                "error": r.error,
            })
        })
        .collect();

    json!({
        "ok": success > 0,
        "project_path": project_path,
        "db_prefix": db_prefix,
        "main_db": merge_result.main_db_path,
        "merge": {
            "merged": merge_result.merged,
            "tables_merged": merge_result.tables_merged,
            "rows_merged": merge_result.rows_merged,
            "duration_ms": merge_result.duration_ms,
            "error": merge_result.error,
        },
        "total_workers": total_workers,
        "parallel": parallel,
        "duration_ms": start.elapsed().as_millis() as u64,
        "success": success,
        "fail": fail,
        "total_nodes": total_nodes,
        "total_edges": total_edges,
        "total_files_indexed": total_files_indexed,
        "modules": modules_json
    })
    .to_string()
}

/// Dynamic-mode parallel indexer with worker queue + core reclaim.
///
/// Phases:
/// 1. `discover_modules` — same as static (file walk, no parsing).
/// 2. Create `SchedShm` — shared memory for cross-process core
///    accounting. Falls back to static mode on creation failure.
/// 3. Sort modules by file count desc — largest first so big modules
///    start early and benefit from cores freed by small modules.
/// 4. Dispatch workers from a queue, claiming cores from the shm pool
///    before each spawn and releasing them on completion. This is the
///    core reclaim: small modules finish and free their cores for the
///    next pending module while large modules retain their proportional
///    allocation until done.
/// 5. Aggregate summary (same as static).
/// 6. Merge per-module DBs (same as static).
///
/// Memory monitoring: child PIDs are sampled each poll tick via
/// `pgrep -P $$` + `ps -o rss=`; the total RSS is pushed to
/// `SchedShm::update_mem_usage`. If RSS exceeds `mem_limit_mb`, new
/// worker spawns are paused (existing workers keep running — we don't
/// kill them).
// Kept for reference; the chunk-level scheduler (index_parallel_chunked)
// replaces this path. Remove after migration is complete.
#[allow(dead_code)]
fn index_parallel_dynamic(project_dir: &str, total_workers: u32, parallel: u32) -> String {
    let start = Instant::now();
    let total_workers = if total_workers == 0 {
        DEFAULT_TOTAL_WORKERS
    } else {
        total_workers
    };
    let parallel = if parallel == 0 {
        DEFAULT_PARALLEL
    } else {
        parallel
    };
    // Cap parallel at total_workers to avoid spawning more OS threads
    // than cores available. The shm pool caps cores, not thread count,
    // so without this cap a user could pass --parallel 100 and spawn
    // 100 idle threads all waiting for 0-core claims.
    let parallel = parallel.min(total_workers).max(1);

    let project_path_buf = match canonicalize_project_dir(project_dir) {
        Ok(p) => p,
        Err(e) => return error_json(&e, "scheduler", "index_parallel_dynamic"),
    };
    let project_path = project_path_buf.to_string_lossy().to_string();

    let exe_path = match resolve_self_exe() {
        Ok(p) => p,
        Err(e) => return error_json(&e, "scheduler", "index_parallel_dynamic"),
    };
    let exe_str = exe_path.to_string_lossy().to_string();

    let grammars_dir =
        std::env::var("GRAMMARS_DIR").unwrap_or_else(|_| "engine/grammars".to_string());

    let run_id = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let prefix_env = std::env::var("CODESCOPE_DB_PREFIX");
    let keep_db = prefix_env.is_ok();
    let db_prefix = prefix_env.unwrap_or_else(|_| format!("/tmp/codescope_parallel_{}", run_id));
    let _ = keep_db; // keep_db consumed in run_module_worker calls below

    eprintln!(
        "scheduler: [dynamic] project={} workers={} parallel={} db_prefix={}",
        project_path, total_workers, parallel, db_prefix
    );

    // ── Phase 1: discover modules ──────────────────────────────
    let discover_json = discover::discover_modules(&project_path);
    let discover_val: Value = match serde_json::from_str(&discover_json) {
        Ok(v) => v,
        Err(e) => {
            return error_json(
                &format!(
                    "discover parse failed: {} [module=scheduler, method=index_parallel_dynamic]",
                    e
                ),
                "scheduler",
                "index_parallel_dynamic",
            );
        }
    };
    if discover_val["ok"] != true {
        return discover_json;
    }

    let modules = match discover_val["modules"].as_array() {
        Some(a) if !a.is_empty() => a,
        _ => {
            return json!({
                "ok": true,
                "project_path": project_path,
                "duration_ms": start.elapsed().as_millis() as u64,
                "success": 0,
                "fail": 0,
                "total_nodes": 0,
                "total_edges": 0,
                "total_files_indexed": 0,
                "modules": [],
                "note": "no source modules found"
            })
            .to_string();
        }
    };

    let total_files_sum: u64 = modules
        .iter()
        .filter_map(|m| m["files"].as_u64())
        .sum::<u64>()
        .max(1);

    // ── Phase 2: create SchedShm ───────────────────────────────
    // total_cores caps at total_workers so the dynamic scheduler
    // never claims more than the user asked for. available_parallelism
    // gives us the host CPU count, which we then min with total_workers.
    let dyn_config = DynSchedConfig::from_env();
    let host_cores: u32 = std::thread::available_parallelism()
        .map(|n| n.get() as u32)
        .unwrap_or(total_workers.max(1));
    let total_cores = host_cores.min(total_workers.max(1));
    let mem_limit = dyn_config.mem_limit_mb;
    let shm_path = dyn_config
        .shm_path
        .clone()
        .unwrap_or_else(DynSchedConfig::default_shm_path);

    eprintln!(
        "scheduler: [dynamic] shm_path={} total_cores={} mem_limit_mb={}",
        shm_path, total_cores, mem_limit
    );

    let shm = match SchedShm::create(&shm_path, total_cores, mem_limit) {
        Ok(s) => {
            // Set the aggressive poll flag from config (1 = 50ms, 0 = 100ms).
            s.set_aggressive(dyn_config.aggressive);
            s
        }
        Err(e) => {
            // Graceful fallback: disable dynamic sched and recurse into
            // the static path. The env override is process-local and
            // only affects this scheduler run.
            eprintln!(
                "scheduler: [dynamic] SchedShm create failed ({}), falling back to static mode [module=scheduler, method=index_parallel_dynamic]",
                e
            );
            // SAFETY: Rust 2024 marks set_var unsafe because it can race
            // with concurrent getenv readers. This code path runs in the
            // single-threaded scheduler init before any worker threads
            // spawn, so there are no concurrent readers.
            unsafe {
                std::env::set_var("CODESCOPE_DYNAMIC_SCHED", "0");
            }
            return index_parallel(project_dir, total_workers, parallel);
        }
    };

    // RAII guard: remove the shm filesystem path when the scheduler
    // exits, even on panic. Constructed IMMEDIATELY after create so
    // nothing between create and guard can leak the shm file.
    // SchedShm::drop also unlinks (via shm_unlink), so the second
    // unlink is a no-op (ENOENT) — harmless redundancy.
    struct ShmGuard {
        path: String,
    }
    impl Drop for ShmGuard {
        fn drop(&mut self) {
            let _ = std::fs::remove_file(&self.path);
        }
    }
    let _shm_guard = ShmGuard {
        path: shm_path.clone(),
    };

    // Propagate shm path via env so worker subprocesses can attach.
    // Command inherits env vars by default, so setting this once on
    // the scheduler process is enough.
    // SAFETY: Rust 2024 marks set_var unsafe because it can race with
    // concurrent getenv readers. This runs in the single-threaded
    // scheduler init before any worker threads spawn, so no races.
    unsafe {
        std::env::set_var("CODESCOPE_SCHED_SHM", &shm_path);
    }

    // ── Phase 3: sort modules by file count desc ─────────────
    // Largest first so big modules start early — they benefit most
    // from core reclaim since small modules finish and free cores
    // while the big module is still running.
    let mut queue: VecDeque<(String, u64, u64)> = modules
        .iter()
        .enumerate()
        .filter_map(|(i, m)| {
            let name = m["name"].as_str()?.to_string();
            let files = m["files"].as_u64().unwrap_or(0);
            // project_id is 1-indexed (see index_parallel for rationale).
            Some((name, files, (i as u64) + 1))
        })
        .collect();
    // VecDeque::from(Vec) preserves order; then sort in place.
    // We convert to Vec, sort, then convert back since VecDeque
    // has no built-in sort_by_key.
    {
        let mut v: Vec<_> = queue.drain(..).collect();
        v.sort_by_key(|b| std::cmp::Reverse(b.1));
        queue.extend(v);
    }

    // ── Phase 4: dynamic worker dispatch with core reclaim ────
    let (tx, rx) = mpsc::channel::<ModuleResult>();
    let active = Arc::new(AtomicU32::new(0));
    let shm = Arc::new(shm);
    let mut handles = Vec::new();
    let mut results: Vec<ModuleResult> = Vec::new();

    loop {
        // Drain completed workers (non-blocking). The worker thread
        // already released its claimed cores via shm.release_cores
        // before sending the result, so we just collect results here.
        while let Ok(r) = rx.try_recv() {
            results.push(r);
        }

        // Sample total RSS of child processes. Pauses new spawns if
        // over mem_limit (existing workers keep running — we don't
        // kill them, just don't start new ones).
        let child_pids = get_child_pids();
        let total_rss_mb = sample_total_rss_mb(&child_pids);
        shm.update_mem_usage(total_rss_mb);
        let mem_ok = total_rss_mb <= mem_limit;
        if !mem_ok {
            eprintln!(
                "scheduler: [dynamic] memory limit exceeded ({} > {}), pausing new spawns [module=scheduler, method=index_parallel_dynamic]",
                total_rss_mb, mem_limit
            );
        }

        // Try to start new workers while we have capacity, pending
        // tasks, available cores, and memory headroom.
        while active.load(Ordering::SeqCst) < parallel && !queue.is_empty() && mem_ok {
            if !shm.has_available() {
                // No cores to claim right now — wait for an active
                // worker to release some. Don't pop the task yet.
                break;
            }

            let (name, files, project_id) =
                queue.pop_front().expect("queue non-empty in dispatch loop");

            // Desired core count: proportional to file count, capped
            // at total_workers. Matches the static allocation formula.
            let want = if files == 0 {
                1
            } else {
                let raw = (files as u128 * total_workers as u128).div_ceil(total_files_sum as u128);
                let a = u32::try_from(raw as u64).unwrap_or(total_workers);
                std::cmp::max(1, std::cmp::min(a, total_workers))
            };

            // Claim cores atomically. If 0 returned, another worker
            // beat us to it — put the task back at the front and wait.
            let claimed = shm.claim_cores(want);
            if claimed == 0 {
                queue.push_front((name, files, project_id));
                break;
            }

            active.fetch_add(1, Ordering::SeqCst);

            let active_clone = Arc::clone(&active);
            let tx = tx.clone();
            let exe_str = exe_str.clone();
            let project_path = project_path.clone();
            let grammars_dir = grammars_dir.clone();
            let db_prefix = db_prefix.clone();
            let shm_clone = Arc::clone(&shm);

            let handle = std::thread::spawn(move || {
                let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    run_module_worker(
                        &exe_str,
                        &project_path,
                        &name,
                        files,
                        claimed,
                        &grammars_dir,
                        &db_prefix,
                        project_id,
                        None,
                        keep_db,
                    )
                }));
                // Release the cores we claimed so the next pending
                // worker can grab them. This is the core reclaim: a
                // small module finishing frees its cores for the next
                // big module still in the queue.
                // catch_unwind guarantees this runs even if the worker
                // panicked — without it, core leak + active underflow
                // would cause a permanent hang (H-C).
                shm_clone.release_cores(claimed);
                let result = match result {
                    Ok(r) => r,
                    Err(_) => ModuleResult {
                        name: name.clone(),
                        exit_code: -5,
                        total_nodes: 0,
                        total_edges: 0,
                        files_indexed: 0,
                        candidate_files: files,
                        time_parse_ms: 0,
                        duration_secs: 0,
                        workers: claimed,
                        db_path: String::new(),
                        project_id,
                        error: Some(
                            "worker panicked [module=scheduler, method=index_parallel_dynamic]"
                                .to_string(),
                        ),
                    },
                };
                let _ = tx.send(result);
                active_clone.fetch_sub(1, Ordering::SeqCst);
            });
            handles.push(handle);
        }

        // Exit when queue is drained AND no active workers remain.
        if active.load(Ordering::SeqCst) == 0 && queue.is_empty() {
            break;
        }

        // Sleep longer when memory-paused: existing workers need time to
        // finish and release RSS. 50ms polling burns CPU on pgrep+ps for
        // no benefit — 1s is a reasonable backoff.
        if !mem_ok {
            std::thread::sleep(Duration::from_secs(1));
        } else {
            std::thread::sleep(POLL_INTERVAL);
        }
    }

    drop(tx);

    // Drain any results that arrived between the last try_recv and
    // the channel closure.
    while let Ok(r) = rx.try_recv() {
        results.push(r);
    }

    for h in handles {
        let _ = h.join();
    }

    // ── Phase 4b: quarantine failed modules ───────────────────
    // Retry failed modules with crashing files excluded. Tries to claim
    // cores from the shm pool so the retry benefits from available cores
    // (e.g., after small modules finished and freed their cores).
    // Falls back to 1 worker if the pool is empty or shm is unavailable.
    let mut final_results: Vec<ModuleResult> = Vec::new();
    for r in results {
        if r.exit_code == 0 && (r.total_nodes > 0 || r.files_indexed == 0) {
            final_results.push(r);
            continue;
        }
        eprintln!(
            "scheduler: [dynamic] module {} failed (exit={}, nodes={}) — starting quarantine",
            r.name, r.exit_code, r.total_nodes
        );
        let quarantined = quarantine::quarantine_module(
            &exe_str,
            &project_path,
            &r.name,
            &grammars_dir,
            &db_prefix,
        );
        if quarantined.is_empty() {
            final_results.push(r);
            continue;
        }
        let excluded_env = quarantined.join(",");
        // Try to claim up to 4 cores, or fall back to 1.
        let retry_workers = shm.claim_cores(4);
        let retry_workers = if retry_workers == 0 { 1 } else { retry_workers };
        let retry = run_module_worker(
            &exe_str,
            &project_path,
            &r.name,
            r.files_indexed,
            retry_workers,
            &grammars_dir,
            &db_prefix,
            r.project_id,
            Some(&excluded_env),
            keep_db,
        );
        // Release the claimed cores back to the pool.
        shm.release_cores(retry_workers);
        final_results.push(retry);
    }

    // ── Phase 5: aggregate summary ────────────────────────────
    let success = final_results
        .iter()
        .filter(|r| r.exit_code == 0 && (r.total_nodes > 0 || r.files_indexed == 0))
        .count();
    let fail = final_results.len() - success;
    let total_nodes: u64 = final_results.iter().map(|r| r.total_nodes).sum();
    let total_edges: u64 = final_results.iter().map(|r| r.total_edges).sum();
    let total_files_indexed: u64 = final_results.iter().map(|r| r.files_indexed).sum();

    // ── Phase 6: merge per-module DBs into unified main DB ────
    let main_db = format!("{}_main.db", db_prefix);
    // main.db is ALWAYS rebuilt from the module DBs below — keep_db only
    // preserves the per-module DBs so workers can skip unchanged files.
    // Keeping main.db too would double-count rows on INSERT OR IGNORE.
    let _ = std::fs::remove_file(&main_db);
    let _ = std::fs::remove_file(format!("{}-wal", main_db));
    let _ = std::fs::remove_file(format!("{}-shm", main_db));

    let module_db_paths: Vec<String> = final_results
        .iter()
        .filter(|r| r.exit_code == 0 && (r.total_nodes > 0 || r.files_indexed == 0))
        .map(|r| r.db_path.clone())
        .collect();

    let merge_result = if module_db_paths.is_empty() {
        MergeResult {
            merged: false,
            main_db_path: main_db.clone(),
            tables_merged: 0,
            rows_merged: 0,
            duration_ms: 0,
            error: Some("no successful module DBs to merge".to_string()),
        }
    } else {
        merge::merge_module_dbs(&main_db, &module_db_paths)
    };

    // ── Rebuild LadybugDB graphs on the merged DB ─────────────────
    // Parallel workers run with CODESCOPE_SKIP_ASYNC=1, so they never build
    // the LadybugDB graph (.lbug); the merge above only stitches SQLite
    // tables. Without this step the merged main.db has no graph-native data,
    // so find_callers/graph_query etc. would need a separate serial re-index.
    // v0.6 (perf): helper skips the rebuild in incremental no-change runs,
    // relying on the .lbug fingerprint check to stay consistent with SQLite.
    if merge_result.merged {
        rebuild_ladybug_graphs_if_needed(&main_db, keep_db, total_files_indexed);
    }

    let modules_json: Vec<Value> = final_results
        .iter()
        .map(|r| {
            json!({
                "name": r.name,
                "exit_code": r.exit_code,
                "files_indexed": r.files_indexed,
                "candidate_files": r.candidate_files,
                "total_nodes": r.total_nodes,
                "total_edges": r.total_edges,
                "time_parse_ms": r.time_parse_ms,
                "duration_secs": r.duration_secs,
                "workers": r.workers,
                "db_path": r.db_path,
                "error": r.error,
            })
        })
        .collect();

    json!({
        "ok": success > 0,
        "project_path": project_path,
        "db_prefix": db_prefix,
        "main_db": merge_result.main_db_path,
        "merge": {
            "merged": merge_result.merged,
            "tables_merged": merge_result.tables_merged,
            "rows_merged": merge_result.rows_merged,
            "duration_ms": merge_result.duration_ms,
            "error": merge_result.error,
        },
        "total_workers": total_workers,
        "parallel": parallel,
        "sched_mode": "dynamic",
        "duration_ms": start.elapsed().as_millis() as u64,
        "success": success,
        "fail": fail,
        "total_nodes": total_nodes,
        "total_edges": total_edges,
        "total_files_indexed": total_files_indexed,
        "modules": modules_json
    })
    .to_string()
}

/// Get child PIDs of the current process via `pgrep -P $$`.
/// Returns an empty vec if pgrep is unavailable (e.g., unsupported
/// platform) — memory monitoring just degrades to no-op in that case.
/// Kept for reference by the old index_parallel_dynamic path.
#[allow(dead_code)]
fn get_child_pids() -> Vec<u32> {
    let parent_pid = std::process::id();
    let output = std::process::Command::new("pgrep")
        .args(["-P", &parent_pid.to_string()])
        .stderr(std::process::Stdio::null())
        .output();
    match output {
        Ok(out) if out.status.success() => String::from_utf8_lossy(&out.stdout)
            .lines()
            .filter_map(|l| l.trim().parse::<u32>().ok())
            .collect(),
        _ => Vec::new(),
    }
}

/// Canonicalise the project directory to an absolute path.
/// Returns an error string suitable for inclusion in JSON output.
fn canonicalize_project_dir(dir: &str) -> Result<PathBuf, String> {
    let p = Path::new(dir);
    if !p.is_dir() {
        return Err(format!(
            "directory not found: {} [module=scheduler, method=canonicalize_project_dir]",
            dir
        ));
    }
    let canon = std::fs::canonicalize(p).map_err(|e| {
        format!(
            "canonicalize failed: {} [module=scheduler, method=canonicalize_project_dir]",
            e
        )
    })?;
    Ok(canon)
}

/// Resolve the codescope binary path. Prefer $CODESCOPE_BIN (so tests
/// can override), then std::env::current_exe() so a release build can
/// spawn itself.
fn resolve_self_exe() -> Result<PathBuf, String> {
    if let Ok(p) = std::env::var("CODESCOPE_BIN")
        && Path::new(&p).is_file()
    {
        return Ok(PathBuf::from(p));
    }
    std::env::current_exe().map_err(|e| {
        format!(
            "current_exe failed: {} [module=scheduler, method=resolve_self_exe]",
            e
        )
    })
}

/// Build a tagged error JSON string. `module` and `method` populate
/// the bracketed tag so failures can be traced back to the source
/// location per `plan/rules/code_rules.md` (no silent error handling).
fn error_json(msg: &str, module: &str, method: &str) -> String {
    json!({
        "ok": false,
        "error": msg,
        "module": module,
        "method": method
    })
    .to_string()
}

/// Chunk-level parallel indexer with work-stealing (CPU-dynamic scheduling).
///
/// OPT-IN path: entered only when `CODESCOPE_CPU_DYNAMIC` /
/// `CODESCOPE_DYNAMIC_SCHED` is explicitly set. The DEFAULT (no flag) is the
/// static proportional allocator in `index_parallel` — see the project's
/// "static by default" scheduling principle (DYNAMIC_SCHED_REDESIGN.md).
///
/// Stability: each worker writes its OWN DB (no concurrent WAL writers),
/// uses a unique 1-based project_id, and the final merge reuses the proven
/// per-unit-DB + merge_module_dbs machinery from the static path.
fn index_parallel_chunked(project_dir: &str, total_workers: u32, parallel: u32) -> String {
    let start = Instant::now();
    let total_workers = if total_workers == 0 {
        DEFAULT_TOTAL_WORKERS
    } else {
        total_workers
    };
    let parallel = if parallel == 0 {
        DEFAULT_PARALLEL
    } else {
        parallel
    };
    // Cap parallel at total_workers (see Bug 9 fix).
    let parallel = parallel.min(total_workers).max(1);

    let project_path_buf = match canonicalize_project_dir(project_dir) {
        Ok(p) => p,
        Err(e) => return error_json(&e, "scheduler", "index_parallel_chunked"),
    };
    let project_path = project_path_buf.to_string_lossy().to_string();

    let exe_path = match resolve_self_exe() {
        Ok(p) => p,
        Err(e) => return error_json(&e, "scheduler", "index_parallel_chunked"),
    };
    let exe_str = exe_path.to_string_lossy().to_string();

    let grammars_dir =
        std::env::var("GRAMMARS_DIR").unwrap_or_else(|_| "engine/grammars".to_string());

    let run_id = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let prefix_env = std::env::var("CODESCOPE_DB_PREFIX");
    let keep_db = prefix_env.is_ok();
    let db_prefix = prefix_env.unwrap_or_else(|_| format!("/tmp/codescope_chunked_{}", run_id));
    let _ = keep_db; // keep_db consumed in run_module_worker calls below

    // ── Phase 1: discover the GLOBAL file list ───────────────
    // One walk of the whole project yields every candidate source file.
    // Unlike the static path (which plans per-module), the chunked path
    // plans over the flat global list so chunks can span directory
    // boundaries and balance by byte weight.
    let discover_json = discover::discover_files(&project_path);
    let discover_val: Value = match serde_json::from_str(&discover_json) {
        Ok(v) => v,
        Err(e) => {
            return error_json(
                &format!(
                    "discover_files parse failed: {} [module=scheduler, method=index_parallel_chunked]",
                    e
                ),
                "scheduler",
                "index_parallel_chunked",
            );
        }
    };
    if discover_val["ok"] != true {
        return discover_json;
    }
    let all_paths: Vec<String> = discover_val["files"]
        .as_array()
        .map(|a| {
            a.iter()
                .filter_map(|v| v.as_str().map(|s| s.to_string()))
                .collect()
        })
        .unwrap_or_default();
    if all_paths.is_empty() {
        return json!({
            "ok": true,
            "project_path": project_path,
            "duration_ms": start.elapsed().as_millis() as u64,
            "success": 0,
            "fail": 0,
            "total_nodes": 0,
            "total_edges": 0,
            "total_files_indexed": 0,
            "modules": [],
            "note": "no source files found"
        })
        .to_string();
    }

    // Build FileEntry list (path + size), sorted by path — plan_chunks
    // REQUIRES the input sorted by path (see chunk_plan.rs Invariant).
    let mut files: Vec<chunk_plan::FileEntry> = all_paths
        .iter()
        .map(|p| {
            let size = std::fs::metadata(p).map(|m| m.len()).unwrap_or(0);
            chunk_plan::FileEntry {
                path: p.clone(),
                size,
            }
        })
        .collect();
    files.sort_by(|a, b| a.path.cmp(&b.path));

    // ── Phase 2: plan chunks ────────────────────────────────
    let chunks = chunk_plan::plan_chunks(&files, chunk_plan::TARGET_BYTES, chunk_plan::MAX_BYTES);
    if chunks.len() > chunk_queue::MAX_CHUNKS {
        eprintln!(
            "scheduler: warning: plan_chunks produced {} chunks, truncating to MAX_CHUNKS={}; some files will not be indexed [module=scheduler, method=index_parallel_chunked]",
            chunks.len(),
            chunk_queue::MAX_CHUNKS
        );
    }
    let chunk_count = (chunks.len() as u32).min(chunk_queue::MAX_CHUNKS as u32);
    if chunk_count == 0 {
        return error_json(
            "plan_chunks returned zero chunks [module=scheduler, method=index_parallel_chunked]",
            "scheduler",
            "index_parallel_chunked",
        );
    }

    // Share the GLOBAL file list with workers via a temp JSON file.
    // Workers slice it by each chunk's (file_start, file_count).
    let files_json_path = format!("{}_chunk_files.json", db_prefix);
    let files_json = serde_json::to_string(&all_paths).unwrap_or_default();
    if let Err(e) = std::fs::write(&files_json_path, &files_json) {
        return error_json(
            &format!(
                "write chunk file list failed: {} [module=scheduler, method=index_parallel_chunked]",
                e
            ),
            "scheduler",
            "index_parallel_chunked",
        );
    }

    // ── Phase 3: create + fill the chunk queue ──────────────
    let shm_path = format!("/tmp/codescope_chunked_sched_{}.shm", std::process::id());
    let _ = std::fs::remove_file(&shm_path);
    let queue = match chunk_queue::ChunkQueue::create(&shm_path, chunk_count) {
        Ok(q) => q,
        Err(e) => return error_json(&e, "scheduler", "index_parallel_chunked"),
    };
    for (idx, ch) in chunks.iter().take(chunk_count as usize).enumerate() {
        if let Err(e) = queue.write_chunk(
            idx as u32,
            ch.module_id,
            ch.file_start as u32,
            ch.file_count as u32,
            ch.total_bytes,
        ) {
            return error_json(
                &format!(
                    "write_chunk {} failed: {} [module=scheduler, method=index_parallel_chunked]",
                    idx, e
                ),
                "scheduler",
                "index_parallel_chunked",
            );
        }
    }

    eprintln!(
        "scheduler: [chunked] project={} files={} chunks={} workers={} parallel={}",
        project_path,
        all_paths.len(),
        chunk_count,
        total_workers,
        parallel
    );

    // ── Phase 3: spawn workers, each with its OWN DB ──────────
    // Every chunk-worker writes to a unique per-worker DB
    // (`{db_prefix}_chunk_{worker_id}.db`) with a unique project_id
    // (`worker_id + 1`), so there are NO concurrent WAL writers on a
    // shared DB — the original shared_db design corrupted the index
    // under concurrency. After all workers exit, Phase 4 merges the
    // per-worker DBs into the unified main DB via the SAME
    // merge_module_dbs machinery as the static path.
    let mut handles = Vec::new();
    let mut results: Vec<ModuleResult> = Vec::new();
    let active = Arc::new(AtomicU32::new(0));
    let (tx, rx) = mpsc::channel::<ModuleResult>();

    for worker_id in 0..total_workers {
        // Wait for a free slot if at concurrency cap.
        while active.load(Ordering::SeqCst) >= parallel {
            std::thread::sleep(POLL_INTERVAL);
        }
        active.fetch_add(1, Ordering::SeqCst);

        // CPU set for this worker (static binding, Linux only; empty
        // elsewhere so run_chunk_worker skips taskset wrapping).
        #[cfg(target_os = "linux")]
        let cpu_set = {
            let host_cores = std::thread::available_parallelism()
                .map(|n| n.get() as u32)
                .unwrap_or(1)
                .max(1);
            let cores_per_worker = (host_cores / total_workers.max(1)).max(1);
            let cpu_idx = (worker_id * cores_per_worker) % host_cores;
            format!("{}", cpu_idx)
        };
        #[cfg(not(target_os = "linux"))]
        let cpu_set: String = String::new();

        // Per-worker DB + unique project_id. Mirrors the static path's
        // (idx + 1) project_id scheme so merge's id-remap and project
        // disambiguation behave identically.
        let worker_db = format!("{}_chunk_{}.db", db_prefix, worker_id);
        let project_id: u64 = (worker_id as u64) + 1;

        let active_clone = Arc::clone(&active);
        let tx = tx.clone();
        let exe_str = exe_str.clone();
        let grammars_dir = grammars_dir.clone();
        let shm_path = shm_path.clone();
        let worker_db = worker_db.clone();
        let files_json_path = files_json_path.clone();

        let handle = std::thread::spawn(move || {
            let result = worker::run_chunk_worker(
                &exe_str,
                &shm_path,
                worker_id,
                &cpu_set,
                &worker_db,
                &files_json_path,
                project_id,
                &grammars_dir,
            );
            let _ = tx.send(result);
            active_clone.fetch_sub(1, Ordering::SeqCst);
        });
        handles.push(handle);
    }

    drop(tx);

    // Collect results.
    while let Ok(r) = rx.recv() {
        results.push(r);
    }

    // Wait for all threads to finish.
    for h in handles {
        let _ = h.join();
    }

    // ── Phase 4: merge per-worker DBs into the unified main DB ──
    // Mirror index_parallel Phase 6: ATTACH each worker DB to a fresh
    // main DB and INSERT OR IGNORE. Per-worker project_ids are unique
    // (worker_id + 1) and merge_module_dbs remaps ids to avoid
    // cross-worker collisions (same as the static path).
    let main_db = format!("{}_main.db", db_prefix);
    // main.db is ALWAYS rebuilt from the module DBs below — keep_db only
    // preserves the per-module DBs so workers can skip unchanged files.
    // Keeping main.db too would double-count rows on INSERT OR IGNORE.
    let _ = std::fs::remove_file(&main_db);
    let _ = std::fs::remove_file(format!("{}-wal", main_db));
    let _ = std::fs::remove_file(format!("{}-shm", main_db));

    let worker_db_paths: Vec<String> = results
        .iter()
        .filter(|r| r.exit_code == 0 && (r.total_nodes > 0 || r.files_indexed == 0))
        .map(|r| r.db_path.clone())
        .collect();

    let merge_result = if worker_db_paths.is_empty() {
        MergeResult {
            merged: false,
            main_db_path: main_db.clone(),
            tables_merged: 0,
            rows_merged: 0,
            duration_ms: 0,
            error: Some(
                "no successful chunk-worker DBs to merge [module=scheduler, method=index_parallel_chunked]"
                    .to_string(),
            ),
        }
    } else {
        merge::merge_module_dbs(&main_db, &worker_db_paths)
    };

    // ── Phase 5: aggregate summary ────────────────────────────
    let success = results
        .iter()
        .filter(|r| r.exit_code == 0 && (r.total_nodes > 0 || r.files_indexed == 0))
        .count();
    let fail = results.len() - success;
    let total_nodes: u64 = results.iter().map(|r| r.total_nodes).sum();
    let total_edges: u64 = results.iter().map(|r| r.total_edges).sum();
    let total_files_indexed: u64 = results.iter().map(|r| r.files_indexed).sum();

    let modules_json: Vec<Value> = results
        .iter()
        .map(|r| {
            json!({
                "name": r.name,
                "exit_code": r.exit_code,
                "files_indexed": r.files_indexed,
                "candidate_files": r.candidate_files,
                "total_nodes": r.total_nodes,
                "total_edges": r.total_edges,
                "time_parse_ms": r.time_parse_ms,
                "duration_secs": r.duration_secs,
                "workers": r.workers,
                "db_path": r.db_path,
                "error": r.error,
            })
        })
        .collect();

    json!({
        "ok": success > 0,
        "project_path": project_path,
        "db_prefix": db_prefix,
        "main_db": merge_result.main_db_path,
        "merge": {
            "merged": merge_result.merged,
            "tables_merged": merge_result.tables_merged,
            "rows_merged": merge_result.rows_merged,
            "duration_ms": merge_result.duration_ms,
            "error": merge_result.error,
        },
        "total_workers": total_workers,
        "parallel": parallel,
        "sched_mode": "chunked",
        "duration_ms": start.elapsed().as_millis() as u64,
        "success": success,
        "fail": fail,
        "total_nodes": total_nodes,
        "total_edges": total_edges,
        "total_files_indexed": total_files_indexed,
        "modules": modules_json
    })
    .to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_error_json_includes_module_and_method_tags() {
        let s = error_json("boom", "scheduler", "test_method");
        let v: Value = serde_json::from_str(&s).unwrap();
        assert_eq!(v["ok"], false);
        assert_eq!(v["module"], "scheduler");
        assert_eq!(v["method"], "test_method");
        assert!(v["error"].as_str().unwrap().contains("boom"));
    }

    #[test]
    fn test_should_skip_graph_rebuild_gates_on_all_conditions() {
        // Skip only when: keep_db AND no files changed AND .lbug already
        // exists. Any other combination MUST rebuild to avoid losing graph
        // data — these are the precision guarantees for incremental runs.
        // 1. Incremental no-change with an existing .lbug → skip.
        assert!(should_skip_graph_rebuild(true, 0, true));
        // 2. Fresh run (not keep_db) → never skip, even if .lbug exists.
        assert!(!should_skip_graph_rebuild(false, 0, true));
        // 3. Files changed → must rebuild (stale graph risk).
        assert!(!should_skip_graph_rebuild(true, 1, true));
        // 4. No .lbug on disk → must rebuild (nothing to reuse).
        assert!(!should_skip_graph_rebuild(true, 0, false));
        // 5. Everything false → rebuild.
        assert!(!should_skip_graph_rebuild(false, 5, false));
    }

    #[test]
    fn test_canonicalize_project_dir_rejects_missing_dir() {
        let r = canonicalize_project_dir("/nonexistent/does/not/exist/xyz");
        assert!(r.is_err());
        let err = r.unwrap_err();
        assert!(err.contains("directory not found"));
        assert!(err.contains("module=scheduler"));
    }

    #[test]
    fn test_canonicalize_project_dir_accepts_existing_dir() {
        let dir = std::env::temp_dir();
        let r = canonicalize_project_dir(dir.to_str().unwrap());
        assert!(r.is_ok());
        assert!(r.unwrap().is_absolute());
    }
}

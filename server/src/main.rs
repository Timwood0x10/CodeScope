mod discover;
mod ffi;
mod mcp;
mod scheduler;
mod tools;

use crate::scheduler::chunk_queue;
use serde_json::{Value, json};

use std::env;
use std::fs;
use std::path::Path;

/// Resolve the database path used by the CLI-style entry points (`cli` and
/// `index-parallel`).
///
/// `CODESCOPE_DB_PATH` wins; otherwise the cwd-relative `.codescope/` directory
/// is created and used — the same rule server mode applies when it is not given
/// a `--rootPath`. Extracted so `index-parallel` installs its finished database
/// at exactly the path the next command opens: the scheduler assembles its
/// output in a temp directory and only reported it as `main_db`, so the next
/// `get_graph_stats` answered 0 nodes for a project that had just been indexed
/// (measured twice on 2026-10-06).
fn resolve_cli_db_path() -> String {
    let default_dir = ".codescope";
    if !Path::new(default_dir).exists() {
        let _ = fs::create_dir_all(default_dir);
    }
    env::var("CODESCOPE_DB_PATH").unwrap_or_else(|_| format!("{}/codescope.db", default_dir))
}

fn main() {
    // ── --version / -V: print version and exit ─────────────────────
    // Handled before any other argument processing so it works regardless
    // of mode (server/worker/cli/discover) and never touches the engine.
    if std::env::args().any(|a| a == "--version" || a == "-V") {
        println!("CodeScope {}", crate::ffi::version());
        std::process::exit(0);
    }

    let args: Vec<String> = env::args().collect();

    // ── Discover mode: codescope discover <dir_path> ──────────────
    // Quick scan: count candidate source files per top-level directory.
    // No parsing, no SQLite — just filesystem walk with FilterPolicy rules.
    // Outputs JSON with per-directory file counts for parallel module planning.
    if args.len() >= 2 && args[1] == "discover" {
        let dir_path = args.get(2).map(|s| s.as_str()).unwrap_or(".");
        let result = tools::discover(dir_path);
        println!("{}", result);
        return;
    }

    // ── discover-modules: codescope discover-modules <dir_path> ───
    // Built-in scheduler entry: list top-level modules + their source-file
    // counts. Output schema is focused on scheduler use (no skipped_dirs/
    // skipped_files fields).
    // Exit code: 0 on success, 1 on missing dir.
    if args.len() >= 2 && args[1] == "discover-modules" {
        let dir_path = args.get(2).map(|s| s.as_str()).unwrap_or(".");
        let result = discover::discover_modules(dir_path);
        println!("{}", result);
        let ok = serde_json::from_str::<Value>(&result)
            .map(|v| v["ok"] == true)
            .unwrap_or(false);
        if !ok {
            std::process::exit(1);
        }
        return;
    }

    // ── discover-files: codescope discover-files <dir_path> ───────
    // List all candidate source files under a directory, using the same
    // skip rules as the worker's FilterPolicy. Used by the scheduler's
    // quarantine binary search. The worker re-filters via C++ FilterPolicy,
    // so any over-inclusion here is silently dropped by the worker.
    // Exit code: 0 on success, 1 on missing dir.
    if args.len() >= 2 && args[1] == "discover-files" {
        let dir_path = args.get(2).map(|s| s.as_str()).unwrap_or(".");
        let result = discover::discover_files(dir_path);
        println!("{}", result);
        let ok = serde_json::from_str::<Value>(&result)
            .map(|v| v["ok"] == true)
            .unwrap_or(false);
        if !ok {
            std::process::exit(1);
        }
        return;
    }

    // ── index-parallel: codescope index-parallel <dir> [--workers N] [--parallel M] ──
    // Built-in CPU-dynamic parallel indexer. Replaces the legacy
    // scripts/legacy/codescope-parallel.sh.
    // Dispatches one worker subprocess
    // per top-level module with proportional parse-worker allocation; failed
    // modules are quarantined via binary search.
    // Exit code: 0 on success (>=1 module indexed with nodes), 1 on failure.
    // NOTE: the queue segment and the per-module DBs live in the platform
    // temp directory and are shared through src/scheduler/mapped_file.rs,
    // so this path runs on Windows as well as POSIX.
    if args.len() >= 2 && args[1] == "index-parallel" {
        let mut dir_path = ".".to_string();
        let mut total_workers: u32 = 0;
        let mut parallel: u32 = 0;

        let mut i = 2;
        while i < args.len() {
            match args[i].as_str() {
                "--workers" | "-w" => match args.get(i + 1) {
                    Some(v) => {
                        total_workers = v.parse().unwrap_or(0);
                        i += 2;
                        continue;
                    }
                    None => {
                        eprintln!("error: --workers requires a value");
                        std::process::exit(2);
                    }
                },
                "--parallel" | "-p" => match args.get(i + 1) {
                    Some(v) => {
                        parallel = v.parse().unwrap_or(0);
                        i += 2;
                        continue;
                    }
                    None => {
                        eprintln!("error: --parallel requires a value");
                        std::process::exit(2);
                    }
                },
                "--help" | "-h" => {
                    println!("Usage: codescope index-parallel <dir> [--workers N] [--parallel M]");
                    println!("  Built-in CPU-dynamic parallel indexer.");
                    println!("  --workers N   total parse-worker cores (default 8)");
                    println!("  --parallel M  max concurrent module workers (default 4)");
                    return;
                }
                p => {
                    if !p.starts_with("--") {
                        dir_path = p.to_string();
                    }
                    i += 1;
                }
            }
        }

        let result = scheduler::index_parallel(&dir_path, total_workers, parallel);

        // A scheduler-built DB is assembled by the merger from per-worker
        // DBs, so it never receives the engine's post-index pass (knowledge
        // layer, metrics, readiness flags, search index). Without this the
        // CLI's own output looks complete while `search` silently degrades
        // to its graph fallback — it cannot find functions at all — and
        // `project_readiness` stays empty so `project_overview` reports no
        // feature as ready. Measured: 0 rows in project_readiness and in
        // code_fts/name_trgm before, 1 and 1793 after. The merge unifies
        // the DB onto project id 1 (see merge_driver::unify_project), so
        // the pass runs on that id here, while this process owns the DB.
        let merged_db = serde_json::from_str::<serde_json::Value>(&result)
            .ok()
            .filter(|v| v["ok"] == serde_json::Value::Bool(true))
            .and_then(|v| v["main_db"].as_str().map(|s| s.to_string()));
        // Counts as they stand AFTER the pass below; the summary was built
        // before it and would otherwise report a graph the caller cannot see.
        let mut final_totals: Option<(u64, u64)> = None;
        if let Some(main_db) = merged_db.as_deref() {
            if ffi::init(main_db) == 0 {
                let enhanced = ffi::enhance_project(1);
                eprintln!(
                    "codescope: post-index pass on {}: {}",
                    main_db,
                    enhanced.chars().take(120).collect::<String>()
                );
                // Read the finished numbers while the engine still has the DB
                // open (no second sqlite3 round trip).
                if let Ok(v) = serde_json::from_str::<serde_json::Value>(&ffi::get_graph_stats(1)) {
                    final_totals = v["total_nodes"].as_u64().zip(v["total_edges"].as_u64());
                }
                ffi::shutdown();
            } else {
                eprintln!(
                    "codescope: could not open {} for the post-index pass",
                    main_db
                );
            }
        }

        // Install the finished database at the CLI path.
        //
        // The scheduler assembles its output in a temp directory and reports it
        // as `main_db`; nothing copied it back, so `index-parallel` followed by
        // any other command silently read an EMPTY database. The copy happens
        // after the post-index pass released the database, so it is a plain
        // file copy of a closed SQLite file. A stale WAL sidecar of the previous
        // database must be removed first — SQLite would replay it against the
        // new file and corrupt it.
        if let Some(main_db) = merged_db.as_deref() {
            let target = resolve_cli_db_path();
            if main_db != target {
                for suffix in ["-wal", "-shm"] {
                    let sidecar = format!("{}{}", target, suffix);
                    if Path::new(&sidecar).exists()
                        && let Err(e) = fs::remove_file(&sidecar)
                    {
                        eprintln!("codescope: could not remove {}: {}", sidecar, e);
                    }
                }
                match fs::copy(main_db, &target) {
                    Ok(bytes) => eprintln!(
                        "codescope: index-parallel installed its database at {} ({} bytes)",
                        target, bytes
                    ),
                    Err(e) => eprintln!(
                        "codescope: FAILED to install the index at {}: {} — the result is only in {}",
                        target, e, main_db
                    ),
                }
            }
        }

        // Report the graph the run actually produced. The scheduler's summary
        // counts what the workers wrote; the post-index pass above then adds
        // the knowledge layer's edges — a 185-file run summed 1427 edges while
        // the DB it handed back answered 1850, so `index-parallel` disagreed
        // with the next `get_graph_stats` call on its own output. Per-module
        // numbers in `modules[]` still describe the workers.
        let result = match (
            serde_json::from_str::<serde_json::Value>(&result),
            final_totals,
        ) {
            (Ok(mut v), Some((nodes, edges))) => {
                v["total_nodes"] = nodes.into();
                v["total_edges"] = edges.into();
                v.to_string()
            }
            _ => result,
        };

        println!("{}", result);
        return;
    }

    // ── Parse-failure maintenance ────────────────────────────────
    //   codescope parse-failures [--db <path>] [--limit <n>]
    //   codescope reset-failures [--db <path>]
    // parse_failures lists the files the indexer could not parse, with the
    // reason and the retry count that drives the fail-fast skip. These two
    // subcommands are the only supported way to read and clear it — they are
    // what the store comments and the README refer to.
    if args.len() >= 2 && (args[1] == "parse-failures" || args[1] == "reset-failures") {
        let reset = args[1] == "reset-failures";
        let mut db_path = std::env::var("CODESCOPE_DB_PATH")
            .unwrap_or_else(|_| ".codescope/codescope.db".to_string());
        let mut limit: i32 = 100;
        let mut i = 2;
        while i < args.len() {
            match args[i].as_str() {
                "--db" => match args.get(i + 1) {
                    Some(v) => {
                        db_path = v.clone();
                        i += 2;
                        continue;
                    }
                    None => {
                        eprintln!("error: --db requires a value");
                        std::process::exit(2);
                    }
                },
                "--limit" => match args.get(i + 1).map(|v| v.parse::<i32>()) {
                    Some(Ok(v)) => {
                        limit = v;
                        i += 2;
                        continue;
                    }
                    Some(Err(_)) => {
                        eprintln!("error: --limit requires an integer");
                        std::process::exit(2);
                    }
                    None => {
                        eprintln!("error: --limit requires a value");
                        std::process::exit(2);
                    }
                },
                "--help" | "-h" => {
                    println!("Usage: codescope parse-failures [--db <path>] [--limit <n>]");
                    println!("       codescope reset-failures [--db <path>]");
                    println!(
                        "  Read or clear parse_failures: the files the indexer could not parse,"
                    );
                    println!("  with their reason and retry count. Clearing them re-arms the");
                    println!("  fail-fast skip for those files.");
                    return;
                }
                other => {
                    eprintln!("error: unexpected argument '{}' (try --help)", other);
                    std::process::exit(2);
                }
            }
        }
        if ffi::init(&db_path) != 0 {
            eprintln!("parse-failures: engine init failed (db={})", db_path);
            std::process::exit(1);
        }
        let pid = ffi::get_latest_project_id();
        if pid == 0 {
            eprintln!(
                "parse-failures: no project in {} — index something first",
                db_path
            );
            ffi::shutdown();
            std::process::exit(1);
        }
        let result = if reset {
            ffi::reset_parse_failures(pid)
        } else {
            ffi::get_parse_failures(pid, limit)
        };
        ffi::shutdown();
        println!("{}", result);
        // An ok:false envelope must fail the process, or a scripted caller
        // treats a broken read as an empty result.
        let ok = serde_json::from_str::<Value>(&result)
            .map(|v| v["ok"] == true)
            .unwrap_or(false);
        if !ok {
            std::process::exit(1);
        }
        return;
    }

    // ── Force-index mode: codescope force-index <path> [<path>...] ─
    // Index specific files/dirs, BYPASSING the default skip rules
    // (test/, docs/, vendored/, node_modules/, .gitignore, ...).
    // Use case: user says "go index xxx/yyy for me" — the AI runs
    //   codescope force-index /path/to/xxx/yyy
    // and CodeScope pulls in those files regardless of the default
    // skip list.
    //
    // Flags:
    //   --lang <filter>   comma-separated language whitelist
    //   --db <path>       SQLite DB path (default .codescope/codescope.db)
    if args.len() >= 2 && args[1] == "force-index" {
        let mut paths: Vec<String> = Vec::new();
        let mut lang_filter = String::new();
        let mut db_path = std::env::var("CODESCOPE_DB_PATH")
            .unwrap_or_else(|_| ".codescope/codescope.db".to_string());
        let mut i = 2;
        while i < args.len() {
            match args[i].as_str() {
                "--lang" => {
                    if let Some(v) = args.get(i + 1) {
                        lang_filter = v.clone();
                        i += 2;
                        continue;
                    }
                }
                "--db" => {
                    if let Some(v) = args.get(i + 1) {
                        db_path = v.clone();
                        i += 2;
                        continue;
                    }
                }
                "--help" | "-h" => {
                    println!(
                        "Usage: codescope force-index [--lang <filter>] [--db <path>] <path> [<path>...]"
                    );
                    println!("  Force-index specific files/dirs, bypassing default skip rules.");
                    return;
                }
                p => {
                    if !p.starts_with("--") {
                        paths.push(p.to_string());
                    }
                    i += 1;
                }
            }
        }
        if paths.is_empty() {
            eprintln!("force-index: no paths given (try --help)");
            std::process::exit(1);
        }

        if ffi::init(&db_path) != 0 {
            eprintln!("force-index: engine init failed (db={})", db_path);
            std::process::exit(1);
        }

        // Restore latest project_id; create one if DB is fresh.
        let mut pid = ffi::get_latest_project_id();
        if pid == 0 {
            pid = ffi::create_project(&db_path, "force-index");
            eprintln!("force-index: created fresh project_id={}", pid);
        }

        // Build JSON args for tools::execute. We go through the same
        // h_force_index_files handler the MCP server uses, so the
        // semantics are identical.
        let tool_args = serde_json::json!({
            "paths": paths,
            "language_filter": lang_filter,
        });
        let result = tools::execute(pid, "force_index_files", &tool_args);
        println!("{}", result);
        // Both an explicit ok:false AND an "ok:true, files_indexed:0"
        // empty run must fail the process: exit 0 here makes shell/CI
        // callers treat it as an empty success — the same hazard the
        // worker --file-list path guards against below. The engine
        // reports ok:true for an empty run (engine_index_files.cpp:144
        // jobs.empty() early return, and the writer path when every file
        // fails to parse — empty read, unavailable grammar), so the ok flag
        // alone is not a sufficient success predicate. Re-runs of a healthy
        // index always report files_indexed > 0 (unchanged files are
        // re-parsed and counted), so requiring it is safe. Note that this
        // path applies NO fail-fast skip: a forced file is always
        // re-attempted, so a previously failed file still counts as a parse
        // attempt rather than being dropped silently.
        let ok = serde_json::from_str::<Value>(&result)
            .map(|v| v["ok"] == true && v["files_indexed"].as_u64().unwrap_or(0) > 0)
            .unwrap_or(false);

        ffi::shutdown();
        if !ok {
            std::process::exit(1);
        }
        return;
    }

    // ── Worker mode: codescope worker <db_path> <dir_path> <lang_filter> <project_name> <project_id> [--file-list <json>] ─
    // Runs index_project in a subprocess, then exits. RSS is 100% returned to OS on exit.
    // Called by the MCP server to isolate indexing memory from the long-running server.
    // With --file-list, indexes only the specified files (JSON array of paths).
    if args.len() >= 2 && args[1] == "worker" {
        let db_path = args
            .get(2)
            .map(|s| s.as_str())
            .unwrap_or(".codescope/codescope.db");
        let dir_path = args.get(3).map(|s| s.as_str()).unwrap_or(".");
        let lang_filter = args.get(4).map(|s| s.as_str()).unwrap_or("");
        let project_name = args.get(5).map(|s| s.as_str()).unwrap_or("worker-project");
        let project_id_arg = args.get(6).map(|s| s.as_str()).unwrap_or("0");

        // Check for --file-list argument (path to JSON file containing file list)
        let file_list: Option<String> = if args.len() >= 8 && args[7] == "--file-list" {
            match args.get(8) {
                Some(s) => {
                    // Read file list from the specified file. A read failure
                    // must fail the worker: substituting "" makes the engine
                    // return ok:false while we exit 0, which the scheduler
                    // counts as an empty success — and quarantine bisection
                    // treats exit 0 as healthy, so it can skip past a crasher.
                    let path = s.as_str();
                    match std::fs::read_to_string(path) {
                        Ok(content) => Some(content),
                        Err(e) => {
                            eprintln!(
                                "codescope worker: failed to read file-list from {}: {} [module=scheduler, method=worker]",
                                path, e
                            );
                            ffi::shutdown();
                            std::process::exit(1);
                        }
                    }
                }
                None => {
                    eprintln!(
                        "codescope worker: --file-list requires a path [module=scheduler, method=worker]"
                    );
                    ffi::shutdown();
                    std::process::exit(1);
                }
            }
        } else {
            None
        };

        if ffi::init(db_path) != 0 {
            eprintln!("codescope worker: engine init failed");
            std::process::exit(1);
        }

        let pid = if project_id_arg.chars().all(|c| c.is_ascii_digit()) {
            let parsed = project_id_arg.parse::<u64>().unwrap_or(0);
            if parsed == 0 {
                ffi::create_project(dir_path, project_name)
            } else {
                parsed
            }
        } else {
            ffi::create_project(dir_path, project_name)
        };

        eprintln!(
            "worker: project={} starting index_project dir={} lang={}",
            pid, dir_path, lang_filter
        );

        let result = if let Some(files_json) = file_list {
            // Scheduler-driven worker: honour the fail-fast skip, exactly like
            // ffi::index_project does for the whole-directory path.
            ffi::index_files(pid, &files_json, false)
        } else if lang_filter.is_empty() {
            ffi::index_project(pid, dir_path, std::ptr::null())
        } else {
            let c_lang = std::ffi::CString::new(lang_filter).unwrap_or_default();
            ffi::index_project(pid, dir_path, c_lang.as_ptr() as *const _)
        };
        // Write result JSON to stdout for the server to read
        println!("{}", result);

        ffi::shutdown();
        eprintln!("worker: done, RSS will be returned to OS on exit");
        return;
    }

    // ── Chunk-worker mode: codescope chunk-worker <shm_path> <worker_id> <worker_db> <files_json> <project_id> ─
    // Spawned by the chunk-level scheduler (run_chunk_worker in worker.rs).
    // Opens the ChunkQueue from shm, then loops:
    //   claim_next → slice the GLOBAL file list by (file_start,file_count)
    //   → ffi::index_files → mark_done. Reclaims stale chunks (a crashed
    //   peer) via reset_all_stale, and exits when every chunk is
    //   DONE/FAILED. Each worker owns its OWN DB — no shared-DB corruption.
    // Requires the `chunk_queue` module (see scheduler/chunk_queue.rs).
    if args.len() >= 7 && args[1] == "chunk-worker" {
        let shm_path = args[2].as_str();
        let worker_id: u32 = args[3].parse().unwrap_or(0);
        let worker_db = args[4].as_str();
        let files_json_path = args[5].as_str();
        let project_id: u64 = args[6].parse().unwrap_or(0);

        if ffi::init(worker_db) != 0 {
            eprintln!("chunk-worker: engine init failed for {}", worker_db);
            std::process::exit(1);
        }

        let queue = match chunk_queue::ChunkQueue::open(shm_path) {
            Ok(q) => q,
            Err(e) => {
                eprintln!("chunk-worker: open queue {} failed: {}", shm_path, e);
                ffi::shutdown();
                std::process::exit(1);
            }
        };

        // GLOBAL file list (absolute paths) shared by all workers; each
        // chunk is a slice [file_start .. file_start+file_count].
        let files_content = match std::fs::read_to_string(files_json_path) {
            Ok(c) => c,
            Err(e) => {
                eprintln!(
                    "chunk-worker: failed to read file list from {}: {} [module=scheduler, method=chunk_worker]",
                    files_json_path, e
                );
                ffi::shutdown();
                std::process::exit(1);
            }
        };
        let all_paths: Vec<String> = match serde_json::from_str(&files_content) {
            Ok(p) => p,
            Err(e) => {
                eprintln!(
                    "chunk-worker: failed to parse file list JSON: {} [module=scheduler, method=chunk_worker]",
                    e
                );
                ffi::shutdown();
                std::process::exit(1);
            }
        };

        // The scheduler always assigns a unique non-zero project_id per
        // worker ((worker_id + 1)) and passes it on argv. A zero here means
        // the invocation is broken; silently minting a fresh project would
        // create a ghost project whose id the merge phase cannot map, so
        // fail loudly instead of indexing into an orphan.
        if project_id == 0 {
            eprintln!(
                "chunk-worker: project_id is 0 (must be assigned by the scheduler) [module=scheduler, method=chunk_worker]"
            );
            ffi::shutdown();
            std::process::exit(1);
        }
        let pid = project_id;

        // Watchdog window for reclaiming orphaned chunks (crashed peer).
        // Set to match the scheduler's per-worker timeout so a chunk is
        // only reclaimed once its owner has been killed — never while the
        // owner is still alive (which would duplicate rows at merge time).
        let stale_timeout_ms: u64 = std::env::var("CODESCOPE_STALE_TIMEOUT_MS")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(600_000);

        let mut total_nodes: u64 = 0;
        let mut total_edges: u64 = 0;
        let mut files_indexed: u64 = 0;
        let mut chunks_done: u32 = 0;
        let mut chunks_failed: u32 = 0;

        loop {
            match queue.claim_next(worker_id) {
                Some(idx) => {
                    let snap = match queue.chunk_state(idx) {
                        Some(s) => s,
                        None => {
                            queue.mark_failed(idx);
                            chunks_done += 1;
                            chunks_failed += 1;
                            continue;
                        }
                    };
                    let start = snap.file_start as usize;
                    let count = snap.file_count as usize;
                    if count == 0 || start >= all_paths.len() {
                        queue.mark_done(idx);
                        chunks_done += 1;
                        continue;
                    }
                    let end = (start + count).min(all_paths.len());
                    let chunk_files: Vec<String> = all_paths[start..end].to_vec();
                    let files_json = match serde_json::to_string(&chunk_files) {
                        Ok(j) => j,
                        Err(e) => {
                            eprintln!("chunk-worker: serialize chunk {} failed: {}", idx, e);
                            queue.mark_failed(idx);
                            chunks_done += 1;
                            chunks_failed += 1;
                            continue;
                        }
                    };
                    // Chunk worker: same policy as the other scheduler paths —
                    // a chunk whose files keep failing must not be retried
                    // forever by every claimant.
                    let result = ffi::index_files(pid, &files_json, false);
                    if let Ok(v) = serde_json::from_str::<Value>(&result) {
                        if v["ok"] == true {
                            total_nodes += v["total_nodes"].as_u64().unwrap_or(0);
                            total_edges += v["total_edges"].as_u64().unwrap_or(0);
                            files_indexed += v["files_indexed"].as_u64().unwrap_or(0);
                            queue.mark_done(idx);
                        } else {
                            let err = v["error"].as_str().unwrap_or("unknown");
                            eprintln!(
                                "chunk-worker: chunk {} failed: {} [module=scheduler, method=chunk_worker]",
                                idx, err
                            );
                            queue.mark_failed(idx);
                            chunks_failed += 1;
                        }
                    } else {
                        eprintln!(
                            "chunk-worker: chunk {} index_files returned invalid JSON [module=scheduler, method=chunk_worker]",
                            idx
                        );
                        queue.mark_failed(idx);
                        chunks_failed += 1;
                    }
                    chunks_done += 1;
                }
                None => {
                    // No PENDING chunk. Reclaim any stale CLAIMED chunk so a
                    // crashed peer's files aren't stranded, then check
                    // completion. Sleep briefly to avoid a hot spin while
                    // live peers finish their chunks.
                    queue.reset_all_stale(stale_timeout_ms);
                    if queue.is_complete() {
                        break;
                    }
                    std::thread::sleep(std::time::Duration::from_millis(50));
                }
            }
        }

        let result_json = json!({
            "ok": chunks_failed == 0,
            "worker_id": worker_id,
            "total_nodes": total_nodes,
            "total_edges": total_edges,
            "files_indexed": files_indexed,
            "chunks_done": chunks_done,
            "chunks_failed": chunks_failed,
        });
        println!("{}", result_json);

        ffi::shutdown();
        eprintln!(
            "chunk-worker {}: done (nodes={} edges={} files={} chunks={} failed={})",
            worker_id, total_nodes, total_edges, files_indexed, chunks_done, chunks_failed
        );
        return;
    }

    // ── CLI mode: codescope cli <tool_name> [json_args] ─────
    if args.len() >= 3 && args[1] == "cli" {
        let tool_name = &args[2];
        let tool_args: serde_json::Value = if args.len() >= 4 {
            serde_json::from_str(&args[3]).unwrap_or(serde_json::Value::Null)
        } else {
            serde_json::Value::Null
        };

        // Same persistent DB path as server mode.
        let db_path = resolve_cli_db_path();

        if ffi::init(&db_path) != 0 {
            eprintln!("codescope: engine init failed");
            std::process::exit(1);
        }

        // Restore latest project_id so CLI queries work on existing DB
        let mut pid = ffi::get_latest_project_id();
        if pid == 0 {
            // No existing project — create a fresh one
            pid = ffi::create_project(".", "cli-project");
            eprintln!("codescope cli: created fresh project_id={}", pid);
        }
        let result = tools::execute(pid, tool_name, &tool_args);
        println!("{}", result);

        ffi::shutdown();
        return;
    }

    // ── Server mode (default) ─────────────────────────────────
    // Parse --rootPath / --root-path from CLI args. When provided, the
    // server opens <rootPath>/.codescope/codescope.db instead of the
    // cwd-relative default. This lets MCP clients point at an existing
    // project DB without cd-ing into the project directory.
    //
    // Without this, `codescope mcp --rootPath /path/to/project` silently
    // ignores --rootPath, opens cwd/.codescope/codescope.db (wrong DB),
    // and handle_initialize creates an empty project shell because the
    // rootPath doesn't match any project in the wrong DB.
    let mut root_path: Option<String> = None;
    {
        let mut iter = args.iter().skip(1);
        while let Some(arg) = iter.next() {
            if (arg == "--rootPath" || arg == "--root-path")
                && let Some(val) = iter.next()
            {
                root_path = Some(val.clone());
            }
        }
    }

    let db_path = if let Some(ref rp) = root_path {
        let codescope_dir = format!("{}/.codescope", rp);
        if !Path::new(&codescope_dir).exists() {
            fs::create_dir_all(&codescope_dir).unwrap_or_else(|e| {
                eprintln!("codescope: failed to create {}: {}", codescope_dir, e);
            });
        }
        format!("{}/codescope.db", codescope_dir)
    } else {
        let default_dir = ".codescope";
        let default_db = format!("{}/codescope.db", default_dir);
        if !Path::new(default_dir).exists() {
            fs::create_dir_all(default_dir).expect("failed to create .codescope/ directory");
        }
        env::var("CODESCOPE_DB_PATH").unwrap_or(default_db)
    };

    // Publish the resolved path once, before any thread is spawned. Worker
    // subprocesses receive it explicitly via `Command::env`, so there is no
    // need to mutate the process environment (which is `unsafe` in Rust
    // 2024). Tool handlers read it via `tools::db_path()`.
    tools::set_db_path(db_path.clone());

    eprintln!("codescope: initializing with db={}", db_path);

    let rc = ffi::init(&db_path);
    if rc != 0 {
        eprintln!("codescope: failed to initialize engine");
        std::process::exit(1);
    }

    eprintln!("codescope: ready");

    let mut server = mcp::server::Server::new();

    if let Err(e) = server.run() {
        eprintln!("codescope: server error: {}", e);
    }

    ffi::shutdown();
}

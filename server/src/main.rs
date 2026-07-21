mod discover;
mod ffi;
mod mcp;
#[cfg(not(windows))]
mod scheduler;
mod tools;

#[cfg(not(windows))]
use crate::scheduler::chunk_queue;
use serde_json::{Value, json};

use std::env;
use std::fs;
use std::path::Path;

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
    // skipped_files fields). See builtin-scheduler-design.md §4.2.
    // Exit code: 0 on success, 1 on missing dir.
    if args.len() >= 2 && args[1] == "discover-modules" {
        let dir_path = args.get(2).map(|s| s.as_str()).unwrap_or(".");
        let result = discover::discover_modules(dir_path);
        println!("{}", result);
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
        return;
    }

    // ── index-parallel: codescope index-parallel <dir> [--workers N] [--parallel M] ──
    // Built-in CPU-dynamic parallel indexer. Replaces codescope-parallel.sh.
    // See builtin-scheduler-design.md §4. Dispatches one worker subprocess
    // per top-level module with proportional parse-worker allocation; failed
    // modules are quarantined via binary search.
    // Exit code: 0 on success (>=1 module indexed with nodes), 1 on failure.
    // NOTE: This command uses Unix-specific shared memory (mmap) and is not
    // available on Windows. Use `codescope index` instead on Windows.
    if args.len() >= 2 && args[1] == "index-parallel" {
        #[cfg(not(windows))]
        {
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
                        println!(
                            "Usage: codescope index-parallel <dir> [--workers N] [--parallel M]"
                        );
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
            println!("{}", result);
            return;
        }
        #[cfg(windows)]
        {
            eprintln!(
                "error: index-parallel is not available on Windows. Use `codescope index` instead."
            );
            std::process::exit(1);
        }
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

        ffi::shutdown();
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
        let file_list = if args.len() >= 8 && args[7] == "--file-list" {
            args.get(8).map(|s| {
                // Read file list from the specified file
                let path = s.as_str();
                match std::fs::read_to_string(path) {
                    Ok(content) => content,
                    Err(e) => {
                        eprintln!(
                            "codescope worker: failed to read file-list from {}: {}",
                            path, e
                        );
                        String::new()
                    }
                }
            })
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
            ffi::index_files(pid, &files_json)
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
    #[cfg(not(windows))]
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

        // Forced project_id (mirrors the static path); falls back to a
        // fresh project if 0. Each worker's DB is independent, so a single
        // project per worker keeps ids consistent within the DB.
        let pid = if project_id > 0 {
            project_id
        } else {
            let new_pid = ffi::create_project(".", &format!("chunk-worker-{}", worker_id));
            if new_pid == 0 {
                eprintln!(
                    "chunk-worker: failed to create project [module=scheduler, method=chunk_worker]"
                );
                ffi::shutdown();
                std::process::exit(1);
            }
            new_pid
        };

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

        loop {
            match queue.claim_next(worker_id) {
                Some(idx) => {
                    let snap = match queue.chunk_state(idx) {
                        Some(s) => s,
                        None => {
                            queue.mark_failed(idx);
                            chunks_done += 1;
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
                            continue;
                        }
                    };
                    let result = ffi::index_files(pid, &files_json);
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
                        }
                    } else {
                        eprintln!(
                            "chunk-worker: chunk {} index_files returned invalid JSON [module=scheduler, method=chunk_worker]",
                            idx
                        );
                        queue.mark_failed(idx);
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
            "ok": true,
            "worker_id": worker_id,
            "total_nodes": total_nodes,
            "total_edges": total_edges,
            "files_indexed": files_indexed,
            "chunks_done": chunks_done,
        });
        println!("{}", result_json);

        ffi::shutdown();
        eprintln!(
            "chunk-worker {}: done (nodes={} edges={} files={} chunks={})",
            worker_id, total_nodes, total_edges, files_indexed, chunks_done
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

        // Use same persistent DB path as server mode
        let default_dir = ".codescope";
        if !Path::new(default_dir).exists() {
            let _ = fs::create_dir_all(default_dir);
        }
        let default_db = format!("{}/codescope.db", default_dir);
        let db_path = env::var("CODESCOPE_DB_PATH").unwrap_or(default_db);

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
        let db = format!("{}/codescope.db", codescope_dir);
        // Set env var so worker subprocesses (spawned by tools::execute)
        // inherit the correct DB path.
        // Safety: this runs before any threads are spawned (server.run()
        // starts later), so there is no data race on the environment.
        unsafe {
            env::set_var("CODESCOPE_DB_PATH", &db);
        }
        db
    } else {
        let default_dir = ".codescope";
        let default_db = format!("{}/codescope.db", default_dir);
        if !Path::new(default_dir).exists() {
            fs::create_dir_all(default_dir).expect("failed to create .codescope/ directory");
        }
        env::var("CODESCOPE_DB_PATH").unwrap_or(default_db)
    };

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

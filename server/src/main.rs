mod ffi;
mod mcp;
mod tools;

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
    let default_dir = ".codescope";
    let default_db = format!("{}/codescope.db", default_dir);

    // Auto-create .codescope/ if it doesn't exist
    if !Path::new(default_dir).exists() {
        fs::create_dir_all(default_dir).expect("failed to create .codescope/ directory");
    }

    let db_path = env::var("CODESCOPE_DB_PATH").unwrap_or(default_db);

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

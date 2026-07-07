mod ffi;
mod mcp;
mod tools;

use std::env;
use std::fs;
use std::path::Path;

fn main() {
    let args: Vec<String> = env::args().collect();

    // ── Worker mode: codescope worker <db_path> <dir_path> <lang_filter> <project_name> <project_id> ─
    // Runs index_project in a subprocess, then exits. RSS is 100% returned to OS on exit.
    // Called by the MCP server to isolate indexing memory from the long-running server.
    if args.len() >= 2 && args[1] == "worker" {
        let db_path = args
            .get(2)
            .map(|s| s.as_str())
            .unwrap_or(".codescope/codescope.db");
        let dir_path = args.get(3).map(|s| s.as_str()).unwrap_or(".");
        let lang_filter = args.get(4).map(|s| s.as_str()).unwrap_or("");
        let project_name = args.get(5).map(|s| s.as_str()).unwrap_or("worker-project");
        // Fix: project_id_arg should be the 6th argument (args[6]), not the 5th
        let project_id_arg = args.get(6).map(|s| s.as_str()).unwrap_or("0");
        // project_id is the 6th arg if it's numeric, otherwise project_name is

        // Safety: set_var is safe in single-threaded startup context,
        // but Rust 2024 marks it unsafe for multi-threaded use.
        unsafe { env::set_var("CODESCOPE_DB_PATH", db_path); }

        if ffi::init(db_path) != 0 {
            eprintln!("codescope worker: engine init failed");
            std::process::exit(1);
        }

        // Use provided project_id if numeric, otherwise create a new project
        let pid = if project_id_arg.chars().all(|c| c.is_ascii_digit()) {
            project_id_arg
                .parse::<u64>()
                .unwrap_or_else(|_| ffi::create_project(dir_path, project_name))
        } else {
            ffi::create_project(dir_path, project_name)
        };

        eprintln!(
            "worker: project={} starting index_project dir={} lang={}",
            pid, dir_path, lang_filter
        );

        let result = if lang_filter.is_empty() {
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

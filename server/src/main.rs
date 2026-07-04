mod ffi;
mod mcp;
mod tools;

use std::env;
use std::fs;
use std::path::Path;

fn main() {
    let args: Vec<String> = env::args().collect();

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

        let pid = ffi::create_project(".", "cli-project");
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

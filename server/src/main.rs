mod ffi;
mod mcp;
mod tools;

use std::env;
use std::fs;
use std::path::Path;

fn main() {
    // Default to .codescope/ directory in current working directory
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

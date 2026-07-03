mod mcp;
mod ffi;
mod tools;

use std::env;

fn main() {
    let db_path = env::var("CODESCOPE_DB_PATH")
        .unwrap_or_else(|_| "/tmp/codescope.db".to_string());

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

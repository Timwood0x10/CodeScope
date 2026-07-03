mod mcp;
mod ffi;
mod tools;

use std::env;

fn main() {
    let db_path = env::var("ASTGRAPH_DB_PATH")
        .unwrap_or_else(|_| "/tmp/astgraph.db".to_string());

    eprintln!("ast-graph-mcp: initializing with db={}", db_path);

    let rc = ffi::init(&db_path);
    if rc != 0 {
        eprintln!("ast-graph-mcp: failed to initialize engine");
        std::process::exit(1);
    }

    eprintln!("ast-graph-mcp: ready");

    let mut server = mcp::server::Server::new();

    if let Err(e) = server.run() {
        eprintln!("ast-graph-mcp: server error: {}", e);
    }

    ffi::shutdown();
}

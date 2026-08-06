use std::io;

use super::protocol::*;
use super::transport;

use crate::ffi;
use crate::tools;

pub struct Server {
    project_id: u64,
}

impl Server {
    pub fn new() -> Self {
        // Restore the project_id that has the most indexed data from the
        // database, so queries work immediately without re-indexing.
        // get_latest_project_id prefers projects with graph_nodes over
        // empty shells, preventing project_id misalignment.
        let pid = ffi::get_latest_project_id();
        if pid > 0 {
            eprintln!("codescope: restored project_id={}", pid);
        }
        Server { project_id: pid }
    }

    pub fn run(&mut self) -> io::Result<()> {
        loop {
            let req = match transport::read_message()? {
                transport::ReadResult::Msg(r) => r,
                transport::ReadResult::ParseError => continue, // log and skip bad input
                transport::ReadResult::Eof => return Ok(()),   // clean disconnect
            };

            // JSON-RPC notifications must not receive a response, so only
            // write to the transport when `handle_request` returns `Some`.
            if let Some(response) = self.handle_request(req) {
                transport::write_message(&response)?;
            }
        }
    }

    fn handle_request(&mut self, req: Request) -> Option<serde_json::Value> {
        match req {
            Request::Standard {
                id, method, params, ..
            } => {
                let result = self.dispatch(&method, params);
                Some(json_response(id, result))
            }
            Request::Notification { method, params, .. } => {
                // Per JSON-RPC 2.0, notifications (requests without an `id`)
                // must not receive a response. Dispatch the method for its
                // side effects, then return `None` so `run()` skips writing.
                let _ = self.dispatch(&method, params);
                None
            }
        }
    }

    fn dispatch(
        &mut self,
        method: &str,
        params: Option<serde_json::Value>,
    ) -> Result<serde_json::Value, JsonRpcError> {
        match method {
            "initialize" => self.handle_initialize(params),
            "initialized" => Ok(serde_json::Value::Null),
            "notifications/initialized" => Ok(serde_json::Value::Null),
            "tools/list" => self.handle_list_tools(),
            "tools/call" => self.handle_call_tool(params),
            _ => Err(JsonRpcError {
                code: -32601,
                message: format!("Method not found: {}", method),
                data: None,
            }),
        }
    }

    // ── Initialize ──────────────────────────────────────────────

    fn handle_initialize(
        &mut self,
        params: Option<serde_json::Value>,
    ) -> Result<serde_json::Value, JsonRpcError> {
        // Extract project path from initialization params
        // MCP clients may pass rootPath, root_uri, or workspaceFolders
        let root_path = params.as_ref().and_then(|p| {
            // Try rootPath (old Claude Desktop convention)
            if let Some(path) = p.get("rootPath").and_then(|v| v.as_str()) {
                return Some(path.to_string());
            }
            // Try rootUri (MCP spec)
            if let Some(uri) = p.get("rootUri").and_then(|v| v.as_str()) {
                let path = uri.trim_start_matches("file://");
                return Some(path.to_string());
            }
            // Try workspaceFolders (newer MCP clients)
            if let Some(folders) = p.get("workspaceFolders").and_then(|v| v.as_array())
                && let Some(first) = folders.first()
                && let Some(uri) = first.get("uri").and_then(|v| v.as_str())
            {
                let path = uri.trim_start_matches("file://");
                return Some(path.to_string());
            }
            None
        });

        if let Some(ref path) = root_path {
            let name = std::path::Path::new(path)
                .file_name()
                .and_then(|n| n.to_str())
                .unwrap_or("unnamed");

            // Prefer matching an existing project by rootPath before
            // creating a new one. If a project with this rootPath already
            // exists AND has indexed data (graph_nodes > 0), reuse it and
            // skip re-indexing — this is the "MCP reuse" path that avoids
            // project_id misalignment (e.g. CLI indexed id=1, MCP must not
            // create an empty id=2 and read stale data).
            //
            // Bug3 fix: when reuse fails (path mismatch or empty project),
            // we must NOT call ffi::index_project directly — that runs in
            // the server process which already holds g_store (SQLite WAL),
            // causing write contention and silently failing the index.
            // The freshly created project_id would be an empty shell and
            // every subsequent tool call returns 0 nodes. Instead route
            // through tools::execute("index_project") which uses the worker
            // subprocess path (shutdown → spawn worker → re-init) for
            // memory + SQLite isolation.
            let existing_pid = ffi::get_project_id_by_path(path);
            if existing_pid > 0 && ffi::get_project_node_count(existing_pid) > 0 {
                self.project_id = existing_pid;
                eprintln!(
                    "Reusing existing project {} (id={}, has data)",
                    name, existing_pid
                );
            } else {
                // Either no project matches this rootPath, or the matching
                // project has no data yet. Create the project row first
                // (cheap INSERT), then index via the worker subprocess path
                // so we don't race with the server's own g_store.
                self.project_id = ffi::create_project(path, name);
                if self.project_id > 0 {
                    eprintln!(
                        "Created project {} (id={}), indexing via worker...",
                        name, self.project_id
                    );
                    let tool_args = serde_json::json!({
                        "project_path": path,
                        "language_filter": "",
                    });
                    // index_project is no longer a registered MCP tool —
                    // index-parallel + keep_db incremental replaced it. The
                    // session auto-index still needs a worker-subprocess
                    // index, so it calls the internal helper directly.
                    let result = tools::index_project_via_worker(self.project_id, &tool_args);
                    if let Ok(json) = serde_json::from_str::<serde_json::Value>(&result) {
                        if let Some(error) = json.get("error").and_then(|e| e.as_str()) {
                            if !error.is_empty() {
                                eprintln!("Warning: index_project failed: {}", error);
                            }
                        } else {
                            eprintln!("Successfully indexed project {}", name);
                        }
                    } else {
                        eprintln!("Warning: index_project returned invalid JSON: {}", result);
                    }
                }
            }
        }

        let result = InitializeResult {
            protocol_version: "2024-11-05".to_string(),
            capabilities: ServerCapabilities {
                tools: ToolCapability { list_changed: true },
            },
            server_info: ServerInfo {
                name: "codescope".to_string(),
                version: env!("CARGO_PKG_VERSION").to_string(),
            },
        };

        match serde_json::to_value(result) {
            Ok(v) => Ok(v),
            Err(e) => Err(JsonRpcError {
                code: -32603,
                message: "Internal error: failed to serialize initialize result".into(),
                data: Some(serde_json::Value::String(e.to_string())),
            }),
        }
    }

    // ── List Tools ──────────────────────────────────────────────

    fn handle_list_tools(&self) -> Result<serde_json::Value, JsonRpcError> {
        let result = ListToolsResult {
            tools: tools::all_tools(),
        };
        match serde_json::to_value(result) {
            Ok(v) => Ok(v),
            Err(e) => Err(JsonRpcError {
                code: -32603,
                message: "Internal error: failed to serialize tools list".into(),
                data: Some(serde_json::Value::String(e.to_string())),
            }),
        }
    }

    // ── Call Tool ───────────────────────────────────────────────

    fn handle_call_tool(
        &self,
        params: Option<serde_json::Value>,
    ) -> Result<serde_json::Value, JsonRpcError> {
        let params = params.ok_or_else(|| JsonRpcError {
            code: -32602,
            message: "Missing params".into(),
            data: None,
        })?;

        let tool_name = params["name"].as_str().ok_or_else(|| JsonRpcError {
            code: -32602,
            message: "Missing tool name".into(),
            data: None,
        })?;

        let tool_args = params
            .get("arguments")
            .cloned()
            .unwrap_or(serde_json::Value::Null);

        let result = tools::execute(self.project_id, tool_name, &tool_args);

        // Note: background enhancement after `scan_project` is triggered inside
        // `tools::execute`, which is the natural owner for tool-specific logic.

        // Determine if the result indicates an error (JSON with non-null "error" key)
        let is_error = serde_json::from_str::<serde_json::Value>(&result)
            .ok()
            .and_then(|v| v.get("error").cloned())
            .and_then(|e| if e.is_null() { None } else { Some(true) });

        let content = vec![TextContent {
            content_type: "text",
            text: result,
        }];

        let result = CallToolResult { content, is_error };

        match serde_json::to_value(result) {
            Ok(v) => Ok(v),
            Err(e) => Err(JsonRpcError {
                code: -32603,
                message: "Internal error: failed to serialize call tool result".into(),
                data: Some(serde_json::Value::String(e.to_string())),
            }),
        }
    }
}

fn json_response(
    id: serde_json::Value,
    result: Result<serde_json::Value, JsonRpcError>,
) -> serde_json::Value {
    let response = match result {
        Ok(val) => Response {
            jsonrpc: "2.0",
            id: Some(id.clone()),
            result: Some(val),
            error: None,
        },
        Err(err) => Response {
            jsonrpc: "2.0",
            id: Some(id.clone()),
            result: None,
            error: Some(err),
        },
    };
    // Never panic on serialization failure — a JSON-RPC server must return a
    // -32603 internal error response, not crash the process. Construct a raw
    // error response with the jsonrpc version and id for spec compliance.
    match serde_json::to_value(response) {
        Ok(v) => v,
        Err(e) => serde_json::json!({
            "jsonrpc": "2.0",
            "id": id,
            "error": {
                "code": -32603,
                "message": "Internal error: failed to serialize JSON-RPC response",
                "data": e.to_string()
            }
        }),
    }
}

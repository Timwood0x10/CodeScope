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
        Server { project_id: 0 }
    }

    pub fn run(&mut self) -> io::Result<()> {
        loop {
            let req = match transport::read_message()? {
                Some(r) => r,
                None => continue, // empty line or parse error
            };

            let response = self.handle_request(req);
            transport::write_message(&response)?;
        }
    }

    fn handle_request(&mut self, req: Request) -> serde_json::Value {
        match req {
            Request::Standard {
                id, method, params, ..
            } => {
                let result = self.dispatch(&method, params);
                json_response(id, result)
            }
            Request::Notification { method, params, .. } => {
                let _ = self.dispatch(&method, params);
                // Notifications don't get a response
                serde_json::json!({})
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
            if let Some(folders) = p.get("workspaceFolders").and_then(|v| v.as_array()) {
                if let Some(first) = folders.first() {
                    if let Some(uri) = first.get("uri").and_then(|v| v.as_str()) {
                        let path = uri.trim_start_matches("file://");
                        return Some(path.to_string());
                    }
                }
            }
            None
        });

        if let Some(ref path) = root_path {
            let name = path.split('/').last().unwrap_or("unnamed");
            self.project_id = ffi::create_project(path, name);
            if self.project_id > 0 {
                eprintln!("Created project {} (id={})", name, self.project_id);
                let _ = ffi::index_project(self.project_id, path, std::ptr::null());
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

        Ok(serde_json::to_value(result).unwrap())
    }

    // ── List Tools ──────────────────────────────────────────────

    fn handle_list_tools(&self) -> Result<serde_json::Value, JsonRpcError> {
        let result = ListToolsResult {
            tools: tools::all_tools(),
        };
        Ok(serde_json::to_value(result).unwrap())
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

        // ── Auto-trigger background enhancement after scan_project ──
        if tool_name == "scan_project" && self.project_id > 0 {
            crate::ffi::spawn_enhancement(self.project_id);
        }

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

        Ok(serde_json::to_value(result).unwrap())
    }
}

fn json_response(
    id: serde_json::Value,
    result: Result<serde_json::Value, JsonRpcError>,
) -> serde_json::Value {
    let response = match result {
        Ok(val) => Response {
            jsonrpc: "2.0",
            id: Some(id),
            result: Some(val),
            error: None,
        },
        Err(err) => Response {
            jsonrpc: "2.0",
            id: Some(id),
            result: None,
            error: Some(err),
        },
    };
    serde_json::to_value(response).unwrap()
}

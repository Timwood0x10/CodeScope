use serde::{Deserialize, Serialize};

// ── Protocol version ───────────────────────────────────────────

/// The MCP revision this server speaks, declared once.
///
/// The lifecycle requires the server to answer with the client's requested
/// version when it supports it, and with a version it does support otherwise.
/// This server supports exactly one revision, so "always answer this one" IS
/// that rule — the constant exists so the version is stated in one place, and
/// `handle_initialize` reads what the client asked for and logs a mismatch so
/// the compatibility decision is visible rather than implicit.
pub const SUPPORTED_PROTOCOL_VERSION: &str = "2024-11-05";

// ── JSON-RPC 2.0 message types ─────────────────────────────────

#[derive(Debug, Deserialize)]
#[serde(untagged)]
pub enum Request {
    Standard {
        #[allow(dead_code)]
        jsonrpc: String,
        id: serde_json::Value,
        method: String,
        #[serde(default)]
        params: Option<serde_json::Value>,
    },
    Notification {
        #[allow(dead_code)]
        jsonrpc: String,
        method: String,
        #[serde(default)]
        params: Option<serde_json::Value>,
    },
}

#[derive(Debug, Serialize)]
pub struct Response {
    pub jsonrpc: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub id: Option<serde_json::Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub result: Option<serde_json::Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<JsonRpcError>,
}

#[derive(Debug, Serialize)]
pub struct JsonRpcError {
    pub code: i32,
    pub message: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub data: Option<serde_json::Value>,
}

// ── MCP specific types ─────────────────────────────────────────
// All structs in this section use #[serde(rename_all = "camelCase")]
// to conform to the MCP protocol spec, which requires camelCase
// field names in JSON-RPC messages (e.g. "protocolVersion" not
// "protocol_version", "serverInfo" not "server_info").

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InitializeResult {
    pub protocol_version: String,
    pub capabilities: ServerCapabilities,
    pub server_info: ServerInfo,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ServerCapabilities {
    pub tools: ToolCapability,
}

#[derive(Debug, Serialize)]
pub struct ToolCapability {
    #[serde(rename = "listChanged")]
    pub list_changed: bool,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ServerInfo {
    pub name: String,
    pub version: String,
}

#[derive(Debug, Serialize)]
pub struct Tool {
    pub name: String,
    pub description: String,
    #[serde(rename = "inputSchema")]
    pub input_schema: serde_json::Value,
}

#[derive(Debug, Serialize)]
pub struct ListToolsResult {
    pub tools: Vec<Tool>,
}

#[derive(Debug, Serialize)]
// The tool-call result is the one response type that carried the protocol's
// naming rule by hand and got it wrong: `is_error` went out as snake_case,
// while MCP requires `isError` — so a spec-compliant client, which reads
// `isError`, saw NO failure flag at all, not even for results that were
// already being flagged. Every other multi-word field in this file is renamed
// explicitly (`inputSchema`, `listChanged`, `type`); this struct now states the
// rule where the field lives.
#[serde(rename_all = "camelCase")]
pub struct CallToolResult {
    pub content: Vec<TextContent>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub is_error: Option<bool>,
}

#[derive(Debug, Serialize)]
pub struct TextContent {
    #[serde(rename = "type")]
    pub content_type: &'static str,
    pub text: String,
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn test_call_tool_result_uses_the_wire_name_is_error() {
        // MCP's CallToolResult field is `isError`. It was serialized as
        // `is_error`, so a spec-compliant client reading `isError` saw no
        // failure flag at all — including for results that were already
        // flagged as failures. Pinned here because the payload is what clients
        // actually parse, not the Rust field name.
        let err = CallToolResult {
            content: vec![TextContent {
                content_type: "text",
                text: "{\"error\":\"boom\"}".into(),
            }],
            is_error: Some(true),
        };
        let v = serde_json::to_value(&err).expect("serialize");
        assert_eq!(v["isError"], serde_json::json!(true));
        assert!(v.get("is_error").is_none(), "snake_case must not appear");

        // A successful result omits the field entirely (the client's default
        // is "not an error"), so the wire shape is unchanged for successes.
        let ok = CallToolResult {
            content: vec![TextContent {
                content_type: "text",
                text: "{}".into(),
            }],
            is_error: None,
        };
        let v = serde_json::to_value(&ok).expect("serialize");
        assert!(v.get("isError").is_none());
        assert!(v.get("is_error").is_none());
    }

    #[test]
    fn test_initialize_result_serde() {
        let r = InitializeResult {
            protocol_version: SUPPORTED_PROTOCOL_VERSION.into(),
            capabilities: ServerCapabilities {
                tools: ToolCapability { list_changed: true },
            },
            server_info: ServerInfo {
                name: "codescope".into(),
                version: "0.3.0".into(),
            },
        };
        let json = serde_json::to_value(&r).unwrap();
        // After rename_all = "camelCase", field names use camelCase
        assert_eq!(json["protocolVersion"], "2024-11-05");
        assert_eq!(json["serverInfo"]["name"], "codescope");
        assert!(
            json["capabilities"]["tools"]["listChanged"]
                .as_bool()
                .unwrap()
        );
    }

    #[test]
    fn test_tool_serde() {
        let t = Tool {
            name: "test_tool".into(),
            description: "A test tool".into(),
            input_schema: json!({"type": "object", "properties": {}}),
        };
        let json = serde_json::to_value(&t).unwrap();
        assert_eq!(json["name"], "test_tool");
        assert_eq!(json["description"], "A test tool");
        assert!(json["inputSchema"].is_object());
    }

    #[test]
    fn test_call_tool_result() {
        let r = CallToolResult {
            content: vec![TextContent {
                content_type: "text",
                text: "hello".into(),
            }],
            is_error: None,
        };
        let json = serde_json::to_value(&r).unwrap();
        assert_eq!(json["content"][0]["type"], "text");
        assert_eq!(json["content"][0]["text"], "hello");
    }
}

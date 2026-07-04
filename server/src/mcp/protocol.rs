use serde::{Deserialize, Serialize};

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

#[derive(Debug, Serialize)]
pub struct InitializeResult {
    pub protocol_version: String,
    pub capabilities: ServerCapabilities,
    pub server_info: ServerInfo,
}

#[derive(Debug, Serialize)]
pub struct ServerCapabilities {
    pub tools: ToolCapability,
}

#[derive(Debug, Serialize)]
pub struct ToolCapability {
    #[serde(rename = "listChanged")]
    pub list_changed: bool,
}

#[derive(Debug, Serialize)]
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
    fn test_initialize_result_serde() {
        let r = InitializeResult {
            protocol_version: "2024-11-05".into(),
            capabilities: ServerCapabilities {
                tools: ToolCapability { list_changed: true },
            },
            server_info: ServerInfo {
                name: "codescope".into(),
                version: "0.3.0".into(),
            },
        };
        let json = serde_json::to_value(&r).unwrap();
        assert_eq!(json["protocol_version"], "2024-11-05");
        assert_eq!(json["server_info"]["name"], "codescope");
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

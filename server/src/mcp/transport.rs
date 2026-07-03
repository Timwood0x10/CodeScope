use std::io::{self, BufRead, Write};

use super::protocol::Request;

/// Reads a single JSON-RPC message (one line) from stdin.
/// Returns the parsed Request, or None on EOF.
/// On parse error, writes a JSON-RPC error response to stdout before returning None.
pub fn read_message() -> io::Result<Option<Request>> {
    let stdin = io::stdin();
    let mut line = String::new();
    match stdin.lock().read_line(&mut line) {
        Ok(0) => Ok(None), // EOF
        Ok(_) => {
            let trimmed = line.trim();
            if trimmed.is_empty() {
                return Ok(None);
            }
            match serde_json::from_str::<Request>(trimmed) {
                Ok(req) => Ok(Some(req)),
                Err(e) => {
                    // JSON-RPC requires a Parse Error response (-32700)
                    let error_resp = serde_json::json!({
                        "jsonrpc": "2.0",
                        "id": null,
                        "error": {
                            "code": -32700,
                            "message": "Parse error",
                            "data": {
                                "detail": format!("{}", e),
                                "raw": trimmed
                            }
                        }
                    });
                    let json = serde_json::to_string(&error_resp).unwrap_or_default();
                    let stdout = io::stdout();
                    let mut handle = stdout.lock();
                    let _ = writeln!(handle, "{}", json);
                    let _ = handle.flush();
                    Ok(None)
                }
            }
        }
        Err(e) => Err(e),
    }
}

/// Writes a JSON-RPC response to stdout (one line per message).
pub fn write_message(msg: &serde_json::Value) -> io::Result<()> {
    let stdout = io::stdout();
    let mut handle = stdout.lock();
    let json = serde_json::to_string(msg)?;
    writeln!(handle, "{}", json)?;
    handle.flush()?;
    Ok(())
}

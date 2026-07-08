use std::io::{self, BufRead, Read, Write};

use super::protocol::Request;

/// Result of reading a single message.
pub enum ReadResult {
    /// A valid Request was parsed.
    Msg(Request),
    /// EOF — client disconnected cleanly.
    Eof,
    /// Parse error — malformed JSON was received; a -32700 error response
    /// has already been written to stdout. The caller should continue
    /// reading rather than treating this as EOF.
    ParseError,
}

/// Maximum line length to accept (1MB). Longer lines are rejected as
/// a parse error to prevent memory exhaustion attacks (L8).
const MAX_LINE_LEN: usize = 1 << 20; // 1 MiB

/// Reads a single JSON-RPC message (one line) from stdin.
/// Returns the parsed Request, Eof on clean EOF, or ParseError on bad JSON.
pub fn read_message() -> io::Result<ReadResult> {
    let stdin = io::stdin();
    let mut line = String::new();
    // Limit line length to avoid unbounded memory consumption (DoS).
    // take() ensures at most MAX_LINE_LEN bytes are read into the buffer.
    let mut reader = stdin.lock().take((MAX_LINE_LEN + 1) as u64);
    match reader.read_line(&mut line) {
        Ok(0) => Ok(ReadResult::Eof), // EOF
        Ok(_) => {
            let trimmed = line.trim();
            if trimmed.is_empty() {
                return Ok(ReadResult::Eof);
            }
            match serde_json::from_str::<Request>(trimmed) {
                Ok(req) => Ok(ReadResult::Msg(req)),
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
                    Ok(ReadResult::ParseError)
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

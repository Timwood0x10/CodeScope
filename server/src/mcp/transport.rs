use std::io::{self, BufRead, Write};

use super::protocol::Request;

/// Reads a single JSON-RPC message (one line) from stdin.
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
                    eprintln!("Failed to parse request: {}", e);
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

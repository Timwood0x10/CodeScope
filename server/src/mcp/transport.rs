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

/// Maximum line length to accept (1 MiB). Longer lines are rejected as
/// a parse error to prevent memory exhaustion attacks (L8). The +1 in
/// `take((MAX_LINE_LEN + 1) as u64)` below lets us detect overflow:
/// if `read_line` returns exactly `MAX_LINE_LEN + 1` bytes without a
/// trailing `'\n'`, the line was longer than the limit (M8).
const MAX_LINE_LEN: usize = 1 << 20; // 1 MiB

/// Upper bound on the number of bytes discarded while draining the
/// remainder of an over-long line (M8). If the remainder exceeds this,
/// the protocol is unsalvageable; we drain what we can and let
/// subsequent `read_message` calls re-trigger the drain, making
/// progress toward the next newline. 16 MiB is generous for any
/// legitimate JSON-RPC message and bounds memory use under attack.
const MAX_DRAIN_BYTES: u64 = 16 << 20; // 16 MiB

/// Reads a single JSON-RPC message (one line) from stdin.
/// Returns the parsed Request, Eof on clean EOF, or ParseError on bad JSON.
pub fn read_message() -> io::Result<ReadResult> {
    let stdin = io::stdin();
    let mut line = String::new();
    // Limit line length to avoid unbounded memory consumption (DoS).
    // take() ensures at most MAX_LINE_LEN+1 bytes are read into the
    // buffer: MAX_LINE_LEN bytes of payload + 1 byte to detect
    // overflow. If the line is longer, read_line returns without a
    // trailing '\n' and we drain the remainder below (M8).
    let mut reader = stdin.lock().take((MAX_LINE_LEN + 1) as u64);
    let n = reader.read_line(&mut line)?;
    if n == 0 {
        return Ok(ReadResult::Eof); // clean EOF
    }
    // M8: If we hit the take limit without a trailing '\n', the line
    // was longer than MAX_LINE_LEN. The take() reader is exhausted but
    // the underlying stdin cursor still sits in the MIDDLE of that
    // line — without draining, the next read_message() would resume
    // from there and produce a cascade of ParseErrors, putting the
    // JSON-RPC protocol permanently out of sync. Drop the take
    // wrapper (releasing the stdin lock), drain stdin until the next
    // '\n' or EOF, then report a parse error so the caller continues
    // at a clean protocol boundary.
    if !line.ends_with('\n') && n > MAX_LINE_LEN {
        drop(reader);
        drain_stdin_until_newline()?;
        let error_resp = serde_json::json!({
            "jsonrpc": "2.0",
            "id": null,
            "error": {
                "code": -32700,
                "message": "Parse error",
                "data": {
                    "detail": format!("line exceeds {} byte limit", MAX_LINE_LEN),
                }
            }
        });
        let json = serde_json::to_string(&error_resp).unwrap_or_default();
        let stdout = io::stdout();
        let mut handle = stdout.lock();
        let _ = writeln!(handle, "{}", json);
        let _ = handle.flush();
        return Ok(ReadResult::ParseError);
    }
    drop(reader);
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

/// Drain stdin until the next `'\n'` (inclusive) or EOF, discarding
/// all bytes. Used to resync the JSON-RPC protocol after an over-long
/// line (M8). Caps the drain at `MAX_DRAIN_BYTES` to avoid unbounded
/// allocation; if the remainder exceeds the cap, subsequent reads
/// will re-trigger the drain and make progress toward the next
/// newline.
fn drain_stdin_until_newline() -> io::Result<()> {
    let stdin = io::stdin();
    let handle = stdin.lock();
    // read_until stops at the first '\n' (inclusive) or EOF, so it
    // never over-consumes past the line boundary — critical for
    // keeping the protocol in sync. The sink is discarded.
    let mut sink = Vec::new();
    handle.take(MAX_DRAIN_BYTES).read_until(b'\n', &mut sink)?;
    Ok(())
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

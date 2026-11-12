// briefing.rs — M1 Project Briefing tool handler.
//
// Split out of tools/mod.rs (see plan/rules/code_rules.md 1000-line rule).
// This module registers the `get_project_briefing` MCP tool: a read-only
// navigation card that the AI Agent calls first to understand a project's
// structure, verification state, and unknowns (development_plan §18.7.1).
//
// The handler is a thin pass-through to the C++ engine via FFI. The engine
// assembles the briefing JSON from existing database state; the Rust side
// does not index, run rules, or execute target-project scripts.

use serde_json::{Value, json};

use crate::ffi;

/// Handler for the `get_project_briefing` MCP tool.
///
/// Returns the engine's briefing JSON verbatim. The briefing includes:
/// - freshness (run_id, status, source)
/// - scale (files, loc, symbols)
/// - languages (capped, with total/returned/has_more)
/// - modules (capped, with role=inferred)
/// - entry_points (capped)
/// - verification (run_id, status, evidence_count, findings)
/// - unknowns (explicit list of unverified aspects)
/// - next_queries (navigation suggestions)
///
/// The tool takes no arguments; `project_id` comes from the session
/// context, matching every other tool in the catalog.
pub fn h_get_project_briefing(project_id: u64, _args: &Value) -> String {
    ffi::get_project_briefing(project_id)
}

/// Build the JSON error for a missing required argument.
///
/// Centralised so the tagged error format stays consistent with the rest
/// of the server: `[module=mcp, tool=<name>]`.
#[allow(dead_code)]
fn missing_arg(tool: &str, field: &str) -> String {
    json!({
        "error": format!(
            "{field} field is required [module=mcp, tool={tool}]"
        )
    })
    .to_string()
}

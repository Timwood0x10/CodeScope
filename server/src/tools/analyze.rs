// analyze.rs — M1 `analyze_project` handler.
//
// Split out of tools/mod.rs (1000-line rule). Implements the single analysis
// entry point the plan defines: `profile` selects the work, `view` projects the
// result (development_plan §11.1/§11.2).
//
//   profile=quick        read-only projection of existing facts (never indexes,
//                        never runs rules, never starts a run)
//   profile=conformance  executes the rule set once through the FFI and
//                        persists the run, its evidence and its findings
//   architecture / full  rejected until the structure graph (M2) and the
//                        risk observers (M3) exist
//
// The handler only validates arguments; the engine assembles every payload.

use serde_json::{Value, json};

use crate::ffi;

/// Handler for the `analyze_project` MCP tool.
///
/// Arguments:
/// - `profile` (required): "quick" or "conformance"
/// - `view` (required for quick): "symbol_context", "module_context",
///   or "compare_snapshots"
/// - `scope` (required for quick): JSON object whose fields depend on `view`
/// - `run_id` (optional): pin the read to one verification run (M2)
///
/// `view` is only meaningful with `profile=quick`. Passing `view` with
/// any other profile is rejected to avoid silently ignoring it.
///
/// Returns the engine's JSON verbatim.
pub fn h_analyze_project(project_id: u64, args: &Value) -> String {
    let profile = match args["profile"].as_str() {
        Some(p) => p,
        None => {
            return json!({
                "error": "profile field is required [module=mcp, tool=analyze_project]"
            })
            .to_string();
        }
    };

    match profile {
        // The rule set is the conformance profile. A failing rule is recorded
        // and the run is marked partial, so the other results survive.
        // `view` is not accepted here — it only applies to profile=quick.
        "conformance" => {
            if args.get("view").and_then(|v| v.as_str()).is_some() {
                return json!({
                    "error": "view is only valid with profile=quick \
                              [module=mcp, tool=analyze_project]"
                })
                .to_string();
            }
            ffi::run_rules(project_id, None)
        }
        "quick" => analyze_quick(project_id, args),
        other => json!({
            "error": format!(
                "profile '{other}' is not implemented yet (M1 supports quick and \
                 conformance) [module=mcp, tool=analyze_project]"
            )
        })
        .to_string(),
    }
}

/// Read-only task-context projection: definition, callers, callees, findings
/// and test candidates for one symbol or module (§18.7.2).
fn analyze_quick(project_id: u64, args: &Value) -> String {
    let view = match args["view"].as_str() {
        Some(v) => v,
        None => {
            return json!({
                "error": "view field is required when profile=quick \
                          [module=mcp, tool=analyze_project]"
            })
            .to_string();
        }
    };

    let scope = &args["scope"];
    if !scope.is_object() {
        return json!({
            "error": "scope field must be a JSON object [module=mcp, tool=analyze_project]"
        })
        .to_string();
    }

    let run_id = args["run_id"].as_str();
    ffi::analyze_project(project_id, view, &scope.to_string(), run_id)
}

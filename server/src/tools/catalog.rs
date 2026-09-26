// catalog.rs — the MCP tool catalog.
//
// Split out of tools/mod.rs (see plan/rules/code_rules.md 1000-line rule).
// `all_tools()` is the full list of tool names, descriptions and JSON
// input schemas the server advertises in `tools/list`. It is pure data,
// so it moved as one block: the handler table stays in mod.rs, and a tool
// whose schema is missing from here (or vice versa) shows up immediately
// as a discover/execute mismatch.

use crate::mcp::protocol::Tool;
use serde_json::json;

// ─── Tool Definitions ───────────────────────────────────────────

pub fn all_tools() -> Vec<Tool> {
    vec![
        Tool {
            name: "find_definition".into(),
            description: "[DEPRECATED — use find_symbol] Find where a symbol is defined.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "symbol_name": {"type": "string", "description": "Name of the symbol to find"},
                    "file_filter": {"type": "string", "description": "Optional: filter by file path substring"}
                },
                "required": ["symbol_name"]
            }),
        },
        Tool {
            name: "find_references".into(),
            description: "Find all locations that reference a given symbol.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "symbol_name": {"type": "string"},
                    "file_filter": {"type": "string"}
                },
                "required": ["symbol_name"]
            }),
        },
        Tool {
            name: "search_code".into(),
            description: "[DEPRECATED — use search] Full-text search across code symbols, file paths, and comments.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "query": {"type": "string", "description": "Search query string"},
                    "limit": {"type": "integer", "description": "Max results (default 20, max 100)"}
                },
                "required": ["query"]
            }),
        },
        Tool {
            name: "index_file".into(),
            description: "Index a single source file: parse, build IR, and add to the code graph.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "file_path": {"type": "string", "description": "Absolute path to the source file"}
                },
                "required": ["file_path"]
            }),
        },
        Tool {
            name: "force_index_files".into(),
            description: "Force-index specific files or directories, BYPASSING the default skip rules (test/, docs/, vendored/, node_modules/, .gitignore, etc.). Use this when the user asks to index a path that the default FilterPolicy would skip — e.g. 'go index xxx/yyy for me'. Files under the given paths are indexed regardless of the default skip list, so the user can pull in test fixtures, vendored deps, or generated code on demand. Nested compiler/package output trees are still skipped at any depth (node_modules/, target/, _deps/, __pycache__/, venvs, .git/, and build*/ dirs carrying build-system artifacts — CMake markers for any build*, Gradle leaves only for a dir named `build`) — pass such a directory itself as a path to index its contents anyway. Still respects: file size limit, language detectability, optional language whitelist.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "paths": {
                        "type": "array",
                        "items": {"type": "string"},
                        "description": "Array of absolute file/dir paths to force-index. Directories are walked recursively, bypassing skip-dir rules."
                    },
                    "path": {
                        "type": "string",
                        "description": "Convenience single-path form (merged into `paths` if both given)."
                    },
                    "language_filter": {
                        "type": "string",
                        "description": "Optional comma-separated language whitelist (e.g. \"java,python\"). Empty = all detectable languages."
                    }
                },
                "required": ["paths"]
            }),
        },
        Tool {
            name: "get_graph_stats".into(),
            description: "Get statistics about the current code graph (node count, edge count, file count).".into(),
            input_schema: json!({ "type": "object", "properties": {} }),
        },
        Tool {
            name: "trace_flow".into(),
            description: "Trace the execution flow from a function through its callees recursively. Returns a call chain showing how control flows through the codebase.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "function_name": {"type": "string", "description": "Starting function name"},
                    "depth": {"type": "integer", "description": "How many levels to trace (default 3, max 10)"}
                },
                "required": ["function_name"]
            }),
        },
        Tool {
            name: "explain_symbol".into(),
            description: "Get structured information about a symbol: definition location, callers, callees, and dependencies. Returns a comprehensive view of what the symbol does and how it relates to the rest of the codebase.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "symbol_name": {"type": "string", "description": "Name of the symbol to explain"}
                },
                "required": ["symbol_name"]
            }),
        },
        Tool {
            name: "verify_integrity".into(),
            description: "Verify codebase integrity: checks that README-promised features actually exist in the code. Returns findings with evidence chains and confidence scores. The findings array is capped at max_findings (default 200); `truncated` is the completeness signal. `total` counts every verdict (including Supported ones, which are not listed as findings), so it is normally larger than the findings array.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "max_findings": {
                        "type": "integer",
                        "description": "Maximum number of findings to include in the response (default 200, max 2000). `truncated` is true when the array was cut short; `total` counts all verdicts, not just findings.",
                        "minimum": 1,
                        "maximum": 2000
                    }
                }
            }),
        },
        Tool {
            name: "verify_claim".into(),
            description: "Verify a single claim against the codebase. Dispatches the claim to the appropriate verifier (CapabilityVerifier, ContractVerifier, or ArchitectureVerifier) via the VerifierRegistry, persists the claim + evidence, and returns the verdict with confidence and evidence facts.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "claim": {
                        "type": "string",
                        "description": "The claim as a JSON object (also accepted as a JSON-encoded string). Fields: type (capability_exists|contract_holds|architecture_follows|function_implements), subject, predicate, object, scope, source_kind, source_ref"
                    }
                },
                "required": ["claim"]
            }),
        },
        Tool {
            name: "verify_summary".into(),
            description: "Parse a natural-language summary (README excerpt, AI summary, PR description) into structured claims using the ClaimParser, then verify each claim. Returns aggregated verdicts and a trust_score. Useful for checking whether prose claims about the codebase are actually backed by code evidence.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "text": {
                        "type": "string",
                        "description": "Free-form text to parse + verify (e.g. 'CodeScope supports incremental indexing and is thread-safe')"
                    }
                },
                "required": ["text"]
            }),
        },
        Tool {
            name: "verify_review".into(),
            description: "Verify a code review comment by parsing it into claims and dispatching each through the Claim -> Verifier -> Evidence pipeline. Each claim is stamped source_kind=\"code_review\" so the evidence table can be filtered by origin. Output shape is identical to verify_summary.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "text": {
                        "type": "string",
                        "description": "The review comment body (e.g. 'This function should be thread-safe and the README claims JWT support')"
                    }
                },
                "required": ["text"]
            }),
        },
        Tool {
            name: "verify_reality".into(),
            description: "Verify a single AI statement about the current project reality. Returns a structured evidence report with an aggregate verdict (Supported / Contradicted / PartiallyVerified / Unknown), a confidence score, and the per-claim results array. Use this to check whether an AI's claim about project state is backed by code evidence.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "text": {
                        "type": "string",
                        "description": "A single natural-language statement (e.g. 'The login module supports JWT and Refresh tokens.')"
                    }
                },
                "required": ["text"]
            }),
        },
        Tool {
            name: "detect_drift".into(),
            description: "Scan all declared capabilities and contracts for drift between documentation/code comments and the actual codebase. Detects MissingCapability (declared in README but no implementing entity with callers) and BrokenContract (declared but no enforcing code). Each drift is persisted as a finding row and returned in the JSON output.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "detect_documentation_drift".into(),
            description: "Scan README for language support claims (e.g. 'supports C++, Python, Go') and cross-reference with actual entities in the codebase. Reports DocumentationDrift (severity 1) for any claimed language with zero entities. Returns claimed_languages, found_languages, missing_languages, and drifts arrays. Enables end-to-end verification: AI says X, CodeScope checks the tables.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "detect_capability_drift".into(),
            description: "Scan declared capabilities and cross-reference with actual implementing entities in the codebase. Reports CapabilityDrift (severity 2) for any declared capability with no implementing entity that has callers. Returns total_capabilities count and drifts array. Part of end-to-end verification: AI says X, CodeScope checks the tables.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "detect_architecture_drift".into(),
            description: "Scan call edges for architecture layer violations (e.g. Repository calling Controller, Controller calling another Controller directly). Classifies entities into Controller/Service/Repository layers by naming convention and file path, then checks the relation table for reverse calls and same-layer bypasses. Reports ArchitectureDrift (severity 1) for each violating call edge.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "explain_module".into(),
            description: "Build a Knowledge Card for a named module/directory. Returns module info, entities, capabilities, contracts, findings, and an integrity_score. Falls back to deriving module info from the files table by path prefix when the modules table is empty.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "module_name": {
                        "type": "string",
                        "description": "Name of the module/directory (e.g. 'engine', 'server')"
                    }
                },
                "required": ["module_name"]
            }),
        },
        Tool {
            name: "enhance_project".into(),
            description: "Run background enhancement for a project: full tree-sitter parse, call graph construction, FTS index build, and v0.3 semantic_fact extraction (Step 1.5). This is the prerequisite for build_evidence to produce non-empty findings — the semantic facts (sync/mutex/lock, memory/cstring/alloc, error/bare_except, pattern/todo, framework/gin, ffi/extern_call) are extracted here. Returns a JSON summary with files_processed, symbols_enhanced, call_edges, and timing breakdowns. v0.2.5: complexity metrics (cyclomatic/cognitive/nesting) and n-gram semantic vectors are restored and built during index; use engine_get_capabilities / engine_get_enhancement_status to see readiness.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "build_evidence".into(),
            description: "Build evidence findings for the project by applying the v0.3 rule set (sync/memory/error/pattern/framework/ffi) to the indexed semantic_fact rows. Each rule declares its fact needs and a combine mode (Collect / MissingMatch / MissingMatchPerFunction / Count); the engine returns a JSON array of Evidence objects with category, title, confidence, and items[]. Run after index/enhance so semantic facts are populated.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "category": {
                        "type": "string",
                        "enum": ["sync", "memory", "error", "pattern", "framework", "ffi"],
                        "description": "Optional: restrict the run to one rule category. Omit to run all categories."
                    }
                }
            }),
        },
        Tool {
            name: "build_project_state".into(),
            description: "Build (or rebuild) and persist the project state snapshot. Runs the full v0.3 Evidence Pipeline (evidence aggregation + state queries) and UPSERTs the result into the project_state table. Returns the snapshot JSON describing what each inspector ran and what it found (category counts, confidence score, snapshot metadata). Use this after build_evidence to materialize a health snapshot.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "get_project_state".into(),
            description: "Get the previously persisted project state snapshot (without rebuilding). Returns the snapshot_json string for the project, or a JSON error object if no snapshot exists yet (run build_project_state first). Use this for fast reads of the latest health snapshot.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        Tool {
            name: "detect_changes".into(),
            description: "Analyze the impact of code changes across the call graph. Given a list of modified files, returns directly modified functions, their callers (who calls them), and their callees (who they call).".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "modified_files": {"type": "string", "description": "JSON array of modified file paths"}
                },
                "required": ["modified_files"]
            }),
        },
        Tool {
            name: "find_symbol".into(),
            description: "Find symbol(s) by exact name match. Returns id, kind, file path, line/column for each match.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "symbol_name": {"type": "string", "description": "Symbol name to search for"}
                },
                "required": ["symbol_name"]
            }),
        },
        Tool {
            name: "get_module_tree".into(),
            description: "Get the hierarchical module (directory) tree for the current project. Returns modules with id, parent_id, name, path, and file_count.".into(),
            input_schema: json!({ "type": "object", "properties": {} }),
        },
        Tool {
            name: "get_knowledge_graph".into(),
            description: "Direct-query a knowledge-layer table (v0.2.1). Surfaces entity / relation / architecture_edge / module_edge / capability / document / module_summary so you can browse the knowledge graph directly instead of only benefiting indirectly via explain_module / detect_capability_drift / get_module_tree. Block-level: one call returns the whole result set.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "table": {
                        "type": "string",
                        "enum": ["entity", "relation", "architecture_edge",
                                 "module_edge", "capability", "document",
                                 "module_summary"],
                        "description": "Knowledge-layer table to query."
                    },
                    "limit": {
                        "type": "integer",
                        "default": 100,
                        "description": "Max rows to return, clamped to [0, 1000]."
                    }
                },
                "required": ["table"]
            }),
        },
        Tool {
            name: "search".into(),
            description: "Unified code search: FTS5 exact/prefix matching, complemented by n-gram semantic vector search (restored in v0.2.5) that adds lexically-similar name recall when FTS results run short. Supports prefix matching.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "query": {"type": "string", "description": "Search query string"},
                    "limit": {"type": "integer", "description": "Max results (default 20, max 100)"}
                },
                "required": ["query"]
            }),
        },
        Tool {
            name: "find_callers".into(),
            description: "Find all symbols that call the specified function. Auto-adapts to callgraph readiness: uses fast index first, falls back to full analysis.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "symbol_name": {"type": "string", "description": "Name of the symbol to find callers for"},
                    "file_filter": {"type": "string", "description": "Optional: absolute file path. Restricts the callee to the given file, disambiguating homonyms (same name across files/classes, e.g. __init__, run, main). Empty = aggregate all files (legacy behavior, may produce noise on common names)."}
                },
                "required": ["symbol_name"]
            }),
        },
        Tool {
            name: "find_callees".into(),
            description: "Find all symbols called by the specified function. Auto-adapts to callgraph readiness.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "symbol_name": {"type": "string", "description": "Name of the symbol to find callees for"},
                    "file_filter": {"type": "string", "description": "Optional: absolute file path. Restricts the caller to the given file, disambiguating homonyms. See find_callers doc."}
                },
                "required": ["symbol_name"]
            }),
        },
        Tool {
            name: "find_callers_by_entity".into(),
            description: "Find all symbols that call the specified entity (Step 7, plan §7.2). Targets a single entity by its id, so homonyms (same name across files/classes, e.g. __init__, run, main) are never aggregated. Use the candidates returned by find_callers/find_callees when a bare name is ambiguous, or locate the entity first via find_symbol, then pass its id here.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "entity_id": {"type": "integer", "description": "Entity id from the entity table (e.g. from the candidates list of an ambiguous bare-name query, or from find_symbol output)."}
                },
                "required": ["entity_id"]
            }),
        },
        Tool {
            name: "find_callees_by_entity".into(),
            description: "Find all symbols called by the specified entity (Step 7, plan §7.2). Targets a single entity by its id, so homonyms are never aggregated.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "entity_id": {"type": "integer", "description": "Entity id from the entity table (e.g. from the candidates list of an ambiguous bare-name query, or from find_symbol output)."}
                },
                "required": ["entity_id"]
            }),
        },
        Tool {
            name: "get_verifier_registry_status".into(),
            description: "Report verifier subsystem health (Step 9, plan §9.2): whether the verifier registry is armed, which public claim types are supported, and whether the canonical evidence backend (entity/relation) has data for the project. Pass project_id=0 to skip the evidence probe.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "project_id": {"type": "integer", "description": "Project id; 0 skips the evidence backend probe."}
                },
                "required": []
            }),
        },
        Tool {
            name: "shortest_path".into(),
            description: "Find the shortest call-graph path between two functions. Heuristic/approximate: the call graph is built from name-matched edges, so the BFS path may not reflect true runtime dispatch. Accepts either symbol names (from/to) or explicit graph node IDs (from_id/to_id).".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "from": {"type": "string", "description": "Source symbol name (resolved to a node ID via locate_by_name)"},
                    "to": {"type": "string", "description": "Target symbol name (resolved to a node ID via locate_by_name)"},
                    "from_id": {"type": "integer", "description": "Explicit source graph node ID (bypasses name resolution)"},
                    "to_id": {"type": "integer", "description": "Explicit target graph node ID (bypasses name resolution)"}
                }
            }),
        },
        Tool {
            name: "connected_components".into(),
            description: "Find connected components in the call graph via BFS over name-matched relation edges. Heuristic/approximate: call edges are inferred from name matches, so reported components are a heuristic grouping of the module structure.".into(),
            input_schema: json!({ "type": "object", "properties": {} }),
        },
        Tool {
            name: "get_communities".into(),
            description: "Detect communities (clusters) in the call graph via deterministic label propagation over CALLS edges. Returns {communities:[{id,label,member_count[,members]}], total_communities, returned_communities, inter_community_edges, truncated, approximation:\"heuristic\", note}. Summary-first: each community is a {id,label,member_count} summary unless include_members=true. Heuristic/approximate: edges are resolved by name matching, so indirect calls (virtual/pointer) may be missing.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "max_communities": {"type": "integer", "description": "Max communities to return (default 20, max 500)"},
                    "include_members": {"type": "boolean", "description": "Include each community's member list (default false; keeps the payload small)"},
                    "max_members": {"type": "integer", "description": "Max members per community when include_members is true (default 10, max 200)"}
                }
            }),
        },
        Tool {
            name: "get_entry_points".into(),
            description: "Get likely entry points from the new schema (main/init/setup/run/handler). Returns symbol id, name, file path, and line.".into(),
            input_schema: json!({ "type": "object", "properties": {} }),
        },
        Tool {
            name: "get_type_info".into(),
            description: "Get type information for the project: type definitions (structs, enums, traits) and their reference counts. Optionally filter by type name. Returns JSON with types array.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "type_name": {
                        "description": "Optional: filter types by name (substring match)",
                        "type": "string"
                    }
                }
            }),
        },
        Tool {
            name: "get_routes".into(),
            description: "Get HTTP routes registered in the project. Returns JSON with method, path, handler, file, and line for each route. Supports Gin, Echo, Chi, and net/http patterns.".into(),
            input_schema: json!({ "type": "object", "properties": {} }),
        },
        Tool {
            name: "project_overview".into(),
            description: "Get a comprehensive project overview: languages, modules/symbols, entry points, analysis progress, and ready features. Call this first after initialization.".into(),
            input_schema: json!({ "type": "object", "properties": {} }),
        },
        Tool {
            name: "detect_ffi_boundaries".into(),
            description: "Detect FFI (Foreign Function Interface) boundaries in the project. Returns language distribution, cross-language files, FFI-related symbols (extern, wasm, jni, cabi), and orphan symbols that may serve as FFI entry points.".into(),
            input_schema: json!({ "type": "object", "properties": {} }),
        },
        Tool {
            name: "codescope_trace".into(),
            description: "Explore a function's callers/callees recursively, or trace the shortest path between two functions. Use function_name+depth+direction for interactive exploration, or from+to for shortest path.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "function_name": {"type": "string", "description": "Starting function for interactive exploration"},
                    "depth": {"type": "integer", "description": "How many levels to explore (default: 1, max: 10 — the server clamps to MAX_TRAVERSAL_DEPTH)"},
                    "direction": {"type": "string", "description": "\"callers\", \"callees\", or \"both\" (default: \"both\")"},
                    "from": {"type": "string", "description": "Source function for shortest path (legacy)"},
                    "to": {"type": "string", "description": "Target function for shortest path (legacy)"}
                }
            }),
        },
        Tool {
            name: "count_tokens".into(),
            description: "Estimate token count for a text string using DeepSeek's formula: ASCII*0.3 + non-ASCII*0.6.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "text": {"type": "string", "description": "Text to estimate tokens for"}
                },
                "required": ["text"]
            }),
        },
        Tool {
            name: "get_subgraph".into(),
            description: "Fetch a local region of the code graph centered on a node: the center node plus its direct graph neighbors (1 hop). Returns {nodes:[...], total:N}. Optional node_types/edge_types are comma-separated integer id lists (e.g. \"0,1\") to filter by type. Use this to drill into a specific area instead of pulling the whole graph.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "node_id": {"type": "integer", "description": "Center graph node id (required)"},
                    "radius": {"type": "integer", "description": "Hop radius (reserved, currently 1)"},
                    "node_types": {"type": "string", "description": "Optional comma-separated node type ids, e.g. \"0,1\""},
                    "edge_types": {"type": "string", "description": "Optional comma-separated edge type ids, e.g. \"1\""}
                },
                "required": ["node_id"]
            }),
        },
        Tool {
            name: "get_neighbors".into(),
            description: "Fetch the direct neighbors (callers AND callees) of a graph node. Returns {neighbors:[{id,name,node_type,file_path,edge_type,direction}], total:N}. Optional edge_type filters to a single edge type id (-1 = all).".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "node_id": {"type": "integer", "description": "Graph node id (required)"},
                    "edge_type": {"type": "integer", "description": "Edge type id filter (-1 = all, default -1)"},
                    "radius": {"type": "integer", "description": "Hop radius (reserved, currently 1)"}
                },
                "required": ["node_id"]
            }),
        },
        Tool {
            name: "graph_query".into(),
            description: "Query the code graph with a Cypher-like DSL. Syntax: MATCH (srcType[:srcName])-[edgeType]->(tgtType[:tgtName]). Node types: Function(0), Method(1), Class(2), Struct(3), Interface(4), Variable(5), Module(6), File(7). Edge types: References(0), Calls(1), Defines(2), Contains(3), Imports(4), Inherits(5). Empty type matches any. Multi-hop: edgeType*min..max, e.g. MATCH (Function)-[Calls*1..3]->(Function). Returns {results:[{source, edge, target}], total:N}.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "dsl": {"type": "string", "description": "Graph query DSL string, e.g. \"MATCH (Function:main)-[Calls]->(Method)\""}
                },
                "required": ["dsl"]
            }),
        },
        Tool {
            name: "get_graph".into(),
            description: "Retrieve the COMPLETE code graph in paginated pages (required for large projects). Returns {totals:{nodes,edges}, nodes:[...], edges:[...], has_more:{nodes,edges}}. Page nodes via node_offset/node_limit and edges via edge_offset/edge_limit. Optional node_types/edge_types filter by comma-separated integer ids. Iterate while has_more is true to reconstruct the entire graph.".into(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "node_offset": {"type": "integer", "description": "Node page start offset (default 0)"},
                    "node_limit": {"type": "integer", "description": "Max nodes per page (default 5000, max 50000)"},
                    "edge_offset": {"type": "integer", "description": "Edge page start offset (default 0)"},
                    "edge_limit": {"type": "integer", "description": "Max edges per page (default 20000, max 200000)"},
                    "node_types": {"type": "string", "description": "Optional comma-separated node type ids to include"},
                    "edge_types": {"type": "string", "description": "Optional comma-separated edge type ids to include"}
                }
            }),
        },
    ]
}

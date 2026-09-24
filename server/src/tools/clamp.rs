//! Argument-clamping helpers for MCP tool inputs.
//!
//! Split out of tools/mod.rs (see plan/rules/code_rules.md 1000-line rule).
//! Every client-supplied numeric bound that feeds a recursive walk, a row
//! limit, or a typed filter is clamped here so a huge `i64` can never be
//! truncated by `as i32` into a negative or unbounded engine parameter.

/// Upper bound for `limit` arguments accepted by query-style tools.
/// Values above this are clamped down to prevent unbounded result sets.
pub const MAX_QUERY_LIMIT: i64 = 100;
/// Default `limit` used when the client omits the argument.
pub const DEFAULT_QUERY_LIMIT: i64 = 20;

/// Upper bound for recursive-traversal `depth` arguments. The C++ engine
/// expands `depth` levels of callers/callees recursively, so an unbounded
/// value would exhaust the stack and abort the long-running MCP process.
/// Matches the "max 10" documented in the tool schemas.
pub const MAX_TRAVERSAL_DEPTH: i64 = 10;
/// Default `depth` for `trace_flow` when the client omits it.
pub const DEFAULT_TRACE_FLOW_DEPTH: i64 = 3;
/// Default `depth` for `codescope_trace` when the client omits it.
pub const DEFAULT_CODESCOPE_TRACE_DEPTH: i64 = 1;
/// Upper bound for `radius` on neighbourhood / subgraph queries.
pub const MAX_NEIGHBOR_RADIUS: i64 = 3;
/// Upper bound for `get_knowledge_graph` limit (matches the C++ clamp).
pub const MAX_KNOWLEDGE_GRAPH_LIMIT: i64 = 1000;
/// Lowest / highest valid `relation.type` values, per
/// plan/rules/relation_contract.md (0 = References … 7 = HasType).
/// A value <= 0 means "no type filter"; values above the maximum are
/// clamped so a truncated i64 can never select an out-of-contract type.
pub const MIN_EDGE_TYPE_FILTER: i64 = -1;
pub const MAX_EDGE_TYPE_FILTER: i64 = 7;

/// Default / upper bound for `get_communities` `max_communities`. The engine
/// emits one JSON object per community, so an unbounded value would produce a
/// payload proportional to the graph size.
pub const DEFAULT_MAX_COMMUNITIES: i64 = 20;
pub const MAX_MAX_COMMUNITIES: i64 = 500;
/// Default / upper bound for `get_communities` `max_members`. Only used when
/// `include_members` is true.
pub const DEFAULT_MAX_MEMBERS: i64 = 10;
pub const MAX_MAX_MEMBERS: i64 = 200;

/// Clamp a client-supplied recursion depth into `[1, MAX_TRAVERSAL_DEPTH]`.
/// The engine recurses `depth` levels, so an unclamped value (or the
/// truncation of a huge i64 by `as i32`) could exhaust the stack and abort
/// the long-running MCP process.
pub fn clamp_depth(value: Option<i64>, default: i64) -> i32 {
    value.unwrap_or(default).clamp(1, MAX_TRAVERSAL_DEPTH) as i32
}

/// Clamp a client-supplied neighbourhood radius into
/// `[1, MAX_NEIGHBOR_RADIUS]`.
pub fn clamp_radius(value: Option<i64>) -> i32 {
    value.unwrap_or(1).clamp(1, MAX_NEIGHBOR_RADIUS) as i32
}

/// Clamp a client-supplied `relation.type` filter into the contract range.
pub fn clamp_edge_type(value: Option<i64>) -> i32 {
    value
        .unwrap_or(MIN_EDGE_TYPE_FILTER)
        .clamp(MIN_EDGE_TYPE_FILTER, MAX_EDGE_TYPE_FILTER) as i32
}

/// Clamp a client-supplied `get_knowledge_graph` row limit into
/// `[0, MAX_KNOWLEDGE_GRAPH_LIMIT]`.
pub fn clamp_knowledge_limit(value: Option<i64>) -> i32 {
    value
        .unwrap_or(MAX_QUERY_LIMIT)
        .clamp(0, MAX_KNOWLEDGE_GRAPH_LIMIT) as i32
}

/// Clamp a client-supplied `get_communities` community count into
/// `[1, MAX_MAX_COMMUNITIES]`. (The engine also treats a non-positive value
/// as "use the default", but clamping here keeps a huge i64 from being
/// truncated by `as i32` into a negative value.)
pub fn clamp_max_communities(value: Option<i64>) -> i32 {
    value
        .unwrap_or(DEFAULT_MAX_COMMUNITIES)
        .clamp(1, MAX_MAX_COMMUNITIES) as i32
}

/// Clamp a client-supplied `get_communities` member count into
/// `[1, MAX_MAX_MEMBERS]`.
pub fn clamp_max_members(value: Option<i64>) -> i32 {
    value
        .unwrap_or(DEFAULT_MAX_MEMBERS)
        .clamp(1, MAX_MAX_MEMBERS) as i32
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_clamp_depth_bounds() {
        // Missing argument → per-caller default.
        assert_eq!(clamp_depth(None, DEFAULT_TRACE_FLOW_DEPTH), 3);
        assert_eq!(clamp_depth(None, DEFAULT_CODESCOPE_TRACE_DEPTH), 1);
        // Below the minimum collapses to 1 (a zero/negative depth is
        // meaningless for a recursive walk).
        assert_eq!(clamp_depth(Some(0), 3), 1);
        assert_eq!(clamp_depth(Some(-5), 3), 1);
        assert_eq!(clamp_depth(Some(1), 3), 1);
        // Inside the range is preserved.
        assert_eq!(clamp_depth(Some(5), 3), 5);
        assert_eq!(clamp_depth(Some(MAX_TRAVERSAL_DEPTH), 3), 10);
        // Above the maximum is clamped, not truncated.
        assert_eq!(clamp_depth(Some(11), 3), 10);
        assert_eq!(clamp_depth(Some(i64::MAX), 3), 10);
        // 5_000_000_000 as i32 would be 705_032_704 — the old code would
        // have recursed that many levels.
        assert_eq!(clamp_depth(Some(5_000_000_000), 3), 10);
    }

    #[test]
    fn test_clamp_radius_bounds() {
        assert_eq!(clamp_radius(None), 1);
        assert_eq!(clamp_radius(Some(0)), 1);
        assert_eq!(clamp_radius(Some(-3)), 1);
        assert_eq!(clamp_radius(Some(1)), 1);
        assert_eq!(clamp_radius(Some(MAX_NEIGHBOR_RADIUS)), 3);
        assert_eq!(clamp_radius(Some(4)), 3);
        assert_eq!(clamp_radius(Some(i64::MAX)), 3);
    }

    #[test]
    fn test_clamp_edge_type_bounds() {
        // Absent / <= 0 means "no type filter" (the SQL only filters on
        // edge_type > 0).
        assert_eq!(clamp_edge_type(None), MIN_EDGE_TYPE_FILTER as i32);
        assert_eq!(clamp_edge_type(Some(-1)), -1);
        assert_eq!(clamp_edge_type(Some(-100)), -1);
        assert_eq!(clamp_edge_type(Some(0)), 0);
        // Contract range is preserved.
        assert_eq!(clamp_edge_type(Some(1)), 1);
        assert_eq!(clamp_edge_type(Some(MAX_EDGE_TYPE_FILTER)), 7);
        // Above the contract maximum is clamped to HasType, never a
        // truncated/out-of-contract type.
        assert_eq!(clamp_edge_type(Some(8)), 7);
        assert_eq!(clamp_edge_type(Some(i64::MAX)), 7);
    }

    #[test]
    fn test_clamp_knowledge_limit_bounds() {
        assert_eq!(clamp_knowledge_limit(None), MAX_QUERY_LIMIT as i32);
        // 0 is allowed (callers treat it as "no rows").
        assert_eq!(clamp_knowledge_limit(Some(0)), 0);
        assert_eq!(clamp_knowledge_limit(Some(-10)), 0);
        assert_eq!(clamp_knowledge_limit(Some(1)), 1);
        assert_eq!(
            clamp_knowledge_limit(Some(MAX_KNOWLEDGE_GRAPH_LIMIT)),
            MAX_KNOWLEDGE_GRAPH_LIMIT as i32
        );
        // 5_000_000_000 as i32 is negative — the old code passed that
        // straight through to the C++ row limit.
        assert_eq!(
            clamp_knowledge_limit(Some(5_000_000_000)),
            MAX_KNOWLEDGE_GRAPH_LIMIT as i32
        );
    }

    #[test]
    fn test_clamp_max_communities_bounds() {
        assert_eq!(clamp_max_communities(None), DEFAULT_MAX_COMMUNITIES as i32);
        assert_eq!(clamp_max_communities(Some(0)), 1);
        assert_eq!(clamp_max_communities(Some(-5)), 1);
        assert_eq!(clamp_max_communities(Some(100)), 100);
        assert_eq!(
            clamp_max_communities(Some(MAX_MAX_COMMUNITIES)),
            MAX_MAX_COMMUNITIES as i32
        );
        assert_eq!(
            clamp_max_communities(Some(9_999)),
            MAX_MAX_COMMUNITIES as i32
        );
        // 5_000_000_000 as i32 is negative — must not reach the engine.
        assert_eq!(
            clamp_max_communities(Some(5_000_000_000)),
            MAX_MAX_COMMUNITIES as i32
        );
    }

    #[test]
    fn test_clamp_max_members_bounds() {
        assert_eq!(clamp_max_members(None), DEFAULT_MAX_MEMBERS as i32);
        assert_eq!(clamp_max_members(Some(0)), 1);
        assert_eq!(clamp_max_members(Some(-1)), 1);
        assert_eq!(clamp_max_members(Some(50)), 50);
        assert_eq!(clamp_max_members(Some(9_999)), MAX_MAX_MEMBERS as i32);
        assert_eq!(
            clamp_max_members(Some(5_000_000_000)),
            MAX_MAX_MEMBERS as i32
        );
    }
}

// engine_index_metrics.h — Metric computation helpers for the indexer.
//
// Extracted from engine_index_project.cpp to keep that file under the
// 1000-line limit imposed by plan/rules/code_rules.md §1.
//
// Two entry points:
//   computeMetricsFromCST  — primary path, uses tree-sitter CST + records
//   computeMetricsFromUnit — fallback path, walks the legacy IR tree

#ifndef ENGINE_INDEX_METRICS_H
#define ENGINE_INDEX_METRICS_H

#include <vector>

#include "ir/ir.h"
#include "ir/semantic_unit.h"
#include "store/store.h"
#include <tree_sitter/api.h>

namespace index_metrics
{

/// Compute per-function metrics (cyclomatic, cognitive, branch/loop counts,
/// param/call counts, nesting depth, stub flag) from the tree-sitter CST
/// and the flattened record list produced by the Visitor pipeline.
///
/// The CST supplies control-flow nodes (if/for/while/switch/case) that
/// RecordKind intentionally elides. Records provide param/call counts with
/// correct RecordKind values (Parameter=8, CallExpr=9).
///
/// \param tree   The tree-sitter parse tree. Must outlive the call.
/// \param source Null-terminated source text the tree was parsed from.
/// \param records Flattened records from the Visitor (SemanticUnit).
/// \return One MetricRow per Function/Method record, in record order.
std::vector<store::MetricRow>
computeMetricsFromCST(TSTree *tree, const char *source,
		      const std::vector<ir::Record> &records);

/// Compute per-function metrics from the legacy IR TranslationUnit tree.
/// Used as a fallback when no Visitor is available for a language.
///
/// \param unit The IR translation unit. Must outlive the call.
/// \return One MetricRow per FunctionDecl/MethodDecl node.
std::vector<store::MetricRow> computeMetricsFromUnit(ir::TranslationUnit *unit);

} // namespace index_metrics

#endif // ENGINE_INDEX_METRICS_H

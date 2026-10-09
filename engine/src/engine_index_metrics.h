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
/// RecordKind intentionally elides, and the parameter lists: no Visitor emits
/// RecordKind::Parameter, so a record-side parameter count is always 0 (see
/// the note on `arity` below). Records provide call counts with correct
/// RecordKind values (CallExpr=9).
///
/// Records are NOT const: this also writes each Function/Method record's
/// `arity` (its parameter count), because buildGraph fills entity.arity from
/// semantic_records.arity and the Resolver Pipeline reads that column to tell
/// same-name overloads apart. The records must therefore be passed before they
/// are written to the database, which is what all three index paths do.
///
/// \param tree   The tree-sitter parse tree. Must outlive the call.
/// \param source Null-terminated source text the tree was parsed from.
/// \param records Flattened records from the Visitor (SemanticUnit); function
///                and method records get their `arity` set in place.
/// \return One MetricRow per Function/Method record, in record order.
std::vector<store::MetricRow>
computeMetricsFromCST(TSTree *tree, const char *source,
		      std::vector<ir::Record> &records);

/// Compute per-function metrics from the legacy IR TranslationUnit tree.
/// Used as a fallback when no Visitor is available for a language.
///
/// \param unit The IR translation unit. Must outlive the call.
/// \return One MetricRow per FunctionDecl/MethodDecl node.
std::vector<store::MetricRow> computeMetricsFromUnit(ir::TranslationUnit *unit);

} // namespace index_metrics

#endif // ENGINE_INDEX_METRICS_H

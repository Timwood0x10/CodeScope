// rule_metric.cpp — metric rules: one measured value per entity, compared to a
// declared threshold (development_plan §5.2 "指标类", §8.1 L1 measurement).
//
// This is the second fact source of the rule engine. The semantic-fact pipeline
// cannot express "this function is 150 lines": size is recorded on the entity by
// the indexer, not as a semantic fact, and approximating it (e.g. summing child
// rows) would turn an exact measurement into a guess.

#include "rule_metric.h"

#include "template_format.h"

#include "../store/store.h"

#include <sqlite3.h>

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace evidence
{
namespace
{

/// Column that holds the measured value. A fixed pair, so no SQL is built from
/// rule-file text.
const char *metricColumn(Metric metric)
{
	switch (metric) {
	case Metric::FunctionLines:
		return "lines";
	case Metric::ParameterCount:
		return "param_count";
	}
	return "lines";
}

/// One measured entity, read once per metric rule.
struct Measured {
	int64_t entity_id = 0;
	std::string symbol;
	std::string file;
	int64_t line = 0;
	int64_t value = 0;
};

bool compareValue(int64_t value, MetricOp op, int64_t threshold)
{
	switch (op) {
	case MetricOp::Greater:
		return value > threshold;
	case MetricOp::GreaterOrEqual:
		return value >= threshold;
	case MetricOp::Less:
		return value < threshold;
	case MetricOp::LessOrEqual:
		return value <= threshold;
	case MetricOp::Equal:
		return value == threshold;
	}
	return false;
}

std::string columnText(sqlite3_stmt *stmt, int col)
{
	const char *text =
		reinterpret_cast<const char *>(sqlite3_column_text(stmt, col));
	return text ? text : "";
}

/// Read the measured value of every function-like entity in the project.
/// Kind 0/1 (function/method) is the only scope these metrics apply to.
/// @return false when the statement could not run, so the caller records a
///         failed rule instead of "no matches".
bool queryMeasured(sqlite3 *db, uint64_t project_id, Metric metric,
		   std::vector<Measured> &out)
{
	std::string sql =
		"SELECT id, name, qualified_name, file_path, start_row, ";
	sql += metricColumn(metric);
	sql += " FROM entity WHERE project_id = ? AND kind IN (0,1) ORDER BY id";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK)
		return false;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		Measured m;
		m.entity_id = sqlite3_column_int64(stmt, 0);
		const std::string qualified = columnText(stmt, 2);
		// The qualified name is the §17.6 anchor; the bare name is only a
		// fallback for parsers that leave it empty.
		m.symbol = qualified.empty() ? columnText(stmt, 1) : qualified;
		m.file = columnText(stmt, 3);
		m.line = sqlite3_column_int64(stmt, 4);
		m.value = sqlite3_column_int64(stmt, 5);
		out.push_back(std::move(m));
	}
	sqlite3_finalize(stmt);
	return true;
}

/// One Evidence for one measured entity.
Evidence makeEvidence(const Rule &rule, const Measured &m)
{
	std::unordered_map<std::string, std::string> vars;
	vars["count"] = "1";
	vars["name"] = rule.name;
	vars["symbol"] = m.symbol;
	vars["file"] = m.file;
	vars["line"] = std::to_string(m.line);
	vars["value"] = std::to_string(m.value);
	vars["threshold"] = std::to_string(rule.metric.threshold);

	Evidence ev;
	ev.category = rule.category;
	// A value read straight from the index has no extraction uncertainty.
	ev.confidence = 1.0;
	ev.title = formatTemplate(rule.output.title_template, vars);

	EvidenceItem item;
	item.fact_id = static_cast<uint64_t>(m.entity_id);
	item.fact_kind = 0; // entity
	item.category = rule.category;
	item.primitive = metricToString(rule.metric.metric);
	item.kind = "measured";
	item.symbol = m.symbol;
	item.file = m.file;
	item.line = static_cast<int>(m.line);
	item.snippet = std::string(metricToString(rule.metric.metric)) + "=" +
		       std::to_string(m.value);
	ev.items.push_back(std::move(item));
	return ev;
}

} // namespace

std::vector<Evidence> buildMetricEvidence(store::GraphStore *store,
					  uint64_t project_id, const Rule &rule,
					  std::set<std::string> &failed)
{
	std::vector<Evidence> result;
	if (!store)
		return result;

	std::vector<Measured> rows;
	if (!queryMeasured(store->handle(), project_id, rule.metric.metric,
			   rows)) {
		failed.insert(rule.name);
		return result;
	}

	for (const auto &m : rows) {
		if (!compareValue(m.value, rule.metric.op,
				  rule.metric.threshold))
			continue;
		result.push_back(makeEvidence(rule, m));
	}
	return result;
}

} // namespace evidence

// evidence_builder.cpp — Evidence Builder (EB2) implementation.
//
// For each loaded Rule, queries semantic_fact rows matching each
// FactNeed (category, primitive, kind) and applies the Rule's
// CombineMode to produce one or more Evidence findings. The combine
// modes are:
//
//   Collect                 — every matched fact becomes an item
//   MissingMatch            — needs[0] facts MINUS needs[1] facts
//                             (set difference by function_id)
//   MissingMatchPerFunction — group by function_id; emit one item
//                             per function with needs[0] but no
//                             needs[1]
//   Count                   — single evidence with count = match
//                             count, items empty
//
// File/line/snippet for each EvidenceItem are extracted from the
// semantic_fact.detail_json column (a small JSON object written by
// SemanticFactExtractor::buildDetailJson). A tiny string-based
// extractor is used — full JSON parsing is overkill for the
// {"line":N,"snippet":"...","related_symbol":"..."} schema.
//
// All sqlite3_stmt handles are managed by an RAII guard so they are
// finalized on every exit path (early return, exception). The store
// pointer is borrowed; null is checked at every public entry point
// and returns an empty result (no crash).

#include "evidence_builder.h"

#include <algorithm>
#include <cstdio>
#include <sqlite3.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../store/store.h"

namespace evidence
{

// ─── Local helpers ───────────────────────────────────────────────

namespace
{

// RAII guard around a sqlite3_stmt. Calls sqlite3_finalize on
// destruction unless release() is called (transfer ownership).
// Ensures statements are finalized on every exit path, including
// early returns on error. Mirrors the pattern used in
// semantic_fact_extractor.cpp but as a reusable guard.
class StmtGuard {
    public:
	explicit StmtGuard(sqlite3_stmt *stmt = nullptr)
		: stmt_(stmt)
	{
	}
	~StmtGuard()
	{
		if (stmt_)
			sqlite3_finalize(stmt_);
	}
	StmtGuard(const StmtGuard &) = delete;
	StmtGuard &operator=(const StmtGuard &) = delete;
	StmtGuard(StmtGuard &&other) noexcept
		: stmt_(other.stmt_)
	{
		other.stmt_ = nullptr;
	}
	sqlite3_stmt *get() const
	{
		return stmt_;
	}
	sqlite3_stmt *release()
	{
		sqlite3_stmt *s = stmt_;
		stmt_ = nullptr;
		return s;
	}

    private:
	sqlite3_stmt *stmt_;
};

// Read a text column as a std::string; NULL → "".
std::string colText(sqlite3_stmt *stmt, int col)
{
	const unsigned char *t = sqlite3_column_text(stmt, col);
	if (!t)
		return "";
	return std::string(reinterpret_cast<const char *>(t));
}

// Extract the value of a string field from a flat JSON object. Only
// handles the simple "key":"value" form — sufficient for the
// detail_json schema written by buildDetailJson. Returns "" if the
// key is absent or the value is not a string literal. Used to pull
// the `snippet` field out of detail_json.
std::string detailJsonString(const std::string &json, const std::string &key)
{
	std::string needle = "\"" + key + "\":\"";
	size_t k = json.find(needle);
	if (k == std::string::npos)
		return "";
	k += needle.size();
	std::string out;
	while (k < json.size() && json[k] != '"') {
		if (json[k] == '\\' && k + 1 < json.size()) {
			char next = json[k + 1];
			switch (next) {
			case 'n':
				out += '\n';
				break;
			case 't':
				out += '\t';
				break;
			case 'r':
				out += '\r';
				break;
			case '"':
				out += '"';
				break;
			case '\\':
				out += '\\';
				break;
			default:
				out += next;
				break;
			}
			k += 2;
		} else {
			out += json[k++];
		}
	}
	return out;
}

// Extract the value of an integer field from a flat JSON object.
// Only handles the simple "key":N form. Returns 0 if the key is
// absent or the value is not an integer literal. Used to pull the
// `line` field out of detail_json.
int detailJsonInt(const std::string &json, const std::string &key)
{
	std::string needle = "\"" + key + "\":";
	size_t k = json.find(needle);
	if (k == std::string::npos)
		return 0;
	k += needle.size();
	// Skip optional whitespace.
	while (k < json.size() && (json[k] == ' ' || json[k] == '\t'))
		k++;
	int sign = 1;
	if (k < json.size() && json[k] == '-') {
		sign = -1;
		k++;
	}
	int value = 0;
	bool any = false;
	while (k < json.size() && json[k] >= '0' && json[k] <= '9') {
		value = value * 10 + (json[k] - '0');
		any = true;
		k++;
	}
	return any ? value * sign : 0;
}

// Parse detail_json and populate the file/line/snippet fields of an
// EvidenceItem. The detail_json schema (written by
// SemanticFactExtractor::buildDetailJson) is:
//   {"line":N,"snippet":"...","related_symbol":"..."}
// The snippet typically embeds the file path in parentheses
// ("name (file_path)"); we extract the file_path from inside the
// parentheses as a best-effort heuristic. Empty / unparseable
// detail_json leaves the fields at their defaults (file="", line=0,
// snippet="").
void applyDetailJson(const std::string &detail, EvidenceItem &item)
{
	if (detail.empty())
		return;
	item.line = detailJsonInt(detail, "line");
	item.snippet = detailJsonString(detail, "snippet");
	// Snippet shape: "name (file_path)" — extract file_path.
	if (!item.snippet.empty()) {
		size_t open = item.snippet.rfind('(');
		size_t close = item.snippet.rfind(')');
		if (open != std::string::npos && close != std::string::npos &&
		    close > open) {
			item.file =
				item.snippet.substr(open + 1, close - open - 1);
		}
	}
}

// Substitute {placeholder} tokens in a template string with values
// from a lookup map. Unknown tokens are left as-is (defensive: a
// typo in a template should not blank the output). Used to fill
// {count}, {symbol}, {file}, {line}, {name} in rule output
// templates.
std::string
formatTemplate(const std::string &tmpl,
	       const std::unordered_map<std::string, std::string> &vars)
{
	std::string out;
	out.reserve(tmpl.size() + 16);
	for (size_t i = 0; i < tmpl.size();) {
		if (tmpl[i] == '{') {
			size_t end = tmpl.find('}', i + 1);
			if (end == std::string::npos) {
				out += tmpl[i++];
				continue;
			}
			std::string key = tmpl.substr(i + 1, end - i - 1);
			auto it = vars.find(key);
			if (it != vars.end())
				out += it->second;
			else
				out += tmpl.substr(i, end - i + 1);
			i = end + 1;
		} else {
			out += tmpl[i++];
		}
	}
	return out;
}

// A matched semantic_fact row. Read once per (rule, FactNeed) and
// reused across combine modes. function_id is needed for set
// difference and per-function grouping.
struct MatchedFact {
	uint64_t fact_id = 0;
	uint64_t function_id = 0;
	std::string category;
	std::string primitive;
	std::string kind;
	std::string symbol;
	double confidence = 1.0;
	std::string detail_json;
};

// SELECT semantic_fact rows matching a single FactNeed for one
// project. Returns the rows as a vector of MatchedFact; logs a
// prepare/step error to stderr with the [module=evidence, ...]
// trace chain and returns what was collected so far (best-effort).
std::vector<MatchedFact> queryFactsForNeed(store::GraphStore *store,
					   uint64_t project_id,
					   const FactNeed &need)
{
	std::vector<MatchedFact> out;
	if (!store)
		return out;
	sqlite3 *db = store->handle();
	if (!db)
		return out;
	const char *sql = "SELECT id, function_id, category, primitive, kind, "
			  "symbol, confidence, IFNULL(detail_json, '') "
			  "FROM semantic_fact WHERE project_id = ? "
			  "AND category = ? AND primitive = ? AND kind = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=evidence, method=queryFactsForNeed] "
			"prepare failed: %s\n",
			sqlite3_errmsg(db));
		return out;
	}
	StmtGuard guard(stmt);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, need.category.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, need.primitive.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, need.kind.c_str(), -1, SQLITE_TRANSIENT);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		MatchedFact mf;
		mf.fact_id =
			static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
		mf.function_id =
			static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
		mf.category = colText(stmt, 2);
		mf.primitive = colText(stmt, 3);
		mf.kind = colText(stmt, 4);
		mf.symbol = colText(stmt, 5);
		mf.confidence = sqlite3_column_double(stmt, 6);
		mf.detail_json = colText(stmt, 7);
		out.push_back(std::move(mf));
	}
	return out;
}

// Convert a MatchedFact to an EvidenceItem (parses detail_json for
// file/line/snippet). The fact_id is preserved for traceability.
EvidenceItem toEvidenceItem(const MatchedFact &mf)
{
	EvidenceItem item;
	item.fact_id = mf.fact_id;
	item.category = mf.category;
	item.primitive = mf.primitive;
	item.kind = mf.kind;
	item.symbol = mf.symbol;
	applyDetailJson(mf.detail_json, item);
	return item;
}

// Compute the minimum confidence across a list of MatchedFact. Used
// as the Evidence.confidence value (1.0 for empty / Count combine).
double minConfidence(const std::vector<MatchedFact> &facts)
{
	if (facts.empty())
		return 1.0;
	double m = facts[0].confidence;
	for (const auto &f : facts)
		m = std::min(m, f.confidence);
	return m;
}

// Build the variable map for formatTemplate from a representative
// item + a count. The first matched fact's symbol/file/line are used
// for {symbol}/{file}/{line}; {count} is the item count; {name} is
// the rule name (filled by the caller).
std::unordered_map<std::string, std::string>
buildTemplateVars(const std::vector<MatchedFact> &facts,
		  const std::string &rule_name)
{
	std::unordered_map<std::string, std::string> vars;
	vars["count"] = std::to_string(facts.size());
	vars["name"] = rule_name;
	if (!facts.empty()) {
		EvidenceItem item = toEvidenceItem(facts.front());
		vars["symbol"] = item.symbol;
		vars["file"] = item.file;
		vars["line"] = std::to_string(item.line);
	} else {
		vars["symbol"] = "";
		vars["file"] = "";
		vars["line"] = "0";
	}
	return vars;
}

// ─── Combine mode implementations ────────────────────────────────

// Collect: every matched fact becomes an item. Multiple non-optional
// needs are merged (deduplicated by fact_id). Optional needs are
// ignored — they only matter for MissingMatch.
std::vector<Evidence> combineCollect(store::GraphStore *store,
				     uint64_t project_id, const Rule &rule)
{
	std::vector<Evidence> result;
	std::vector<MatchedFact> all;
	std::unordered_set<uint64_t> seen;
	for (const auto &need : rule.needs) {
		if (need.optional)
			continue;
		auto facts = queryFactsForNeed(store, project_id, need);
		for (auto &f : facts) {
			if (seen.insert(f.fact_id).second)
				all.push_back(std::move(f));
		}
	}
	if (all.empty())
		return result;
	Evidence ev;
	ev.category = rule.category;
	ev.confidence = minConfidence(all);
	auto vars = buildTemplateVars(all, rule.name);
	ev.title = formatTemplate(rule.output.title_template, vars);
	for (const auto &f : all)
		ev.items.push_back(toEvidenceItem(f));
	result.push_back(std::move(ev));
	return result;
}

// MissingMatch: needs[0] facts MINUS needs[1] facts, where the set
// difference is by function_id (a needs[0] fact is dropped if its
// function_id appears in any needs[1] fact). Emits one Evidence with
// all surviving needs[0] facts as items.
std::vector<Evidence> combineMissingMatch(store::GraphStore *store,
					  uint64_t project_id, const Rule &rule)
{
	std::vector<Evidence> result;
	if (rule.needs.empty())
		return result;
	auto primary = queryFactsForNeed(store, project_id, rule.needs[0]);
	if (primary.empty())
		return result;
	// Collect function_ids of all optional needs (needs[1..]).
	std::unordered_set<uint64_t> excluded;
	for (size_t i = 1; i < rule.needs.size(); ++i) {
		if (!rule.needs[i].optional)
			continue;
		auto secondary =
			queryFactsForNeed(store, project_id, rule.needs[i]);
		for (const auto &s : secondary)
			excluded.insert(s.function_id);
	}
	std::vector<MatchedFact> kept;
	kept.reserve(primary.size());
	for (auto &f : primary) {
		if (excluded.find(f.function_id) == excluded.end())
			kept.push_back(std::move(f));
	}
	if (kept.empty())
		return result;
	Evidence ev;
	ev.category = rule.category;
	ev.confidence = minConfidence(kept);
	auto vars = buildTemplateVars(kept, rule.name);
	ev.title = formatTemplate(rule.output.title_template, vars);
	for (const auto &f : kept)
		ev.items.push_back(toEvidenceItem(f));
	result.push_back(std::move(ev));
	return result;
}

// MissingMatchPerFunction: group needs[0] facts by function_id; for
// each function whose function_id is NOT in any optional need's
// matches, emit one Evidence with that function's needs[0] facts as
// items. Returns multiple Evidence values (one per surviving
// function), each titled with the rule template substituted with
// that function's first fact.
std::vector<Evidence> combineMissingMatchPerFunction(store::GraphStore *store,
						     uint64_t project_id,
						     const Rule &rule)
{
	std::vector<Evidence> result;
	if (rule.needs.empty())
		return result;
	auto primary = queryFactsForNeed(store, project_id, rule.needs[0]);
	if (primary.empty())
		return result;
	// Collect function_ids of all optional needs (needs[1..]).
	std::unordered_set<uint64_t> excluded;
	for (size_t i = 1; i < rule.needs.size(); ++i) {
		if (!rule.needs[i].optional)
			continue;
		auto secondary =
			queryFactsForNeed(store, project_id, rule.needs[i]);
		for (const auto &s : secondary)
			excluded.insert(s.function_id);
	}
	// Group primary by function_id, preserving first-seen order so
	// the output is deterministic.
	std::vector<uint64_t> order;
	std::unordered_map<uint64_t, std::vector<MatchedFact>> by_function;
	for (auto &f : primary) {
		if (excluded.find(f.function_id) != excluded.end())
			continue;
		if (by_function.find(f.function_id) == by_function.end())
			order.push_back(f.function_id);
		by_function[f.function_id].push_back(std::move(f));
	}
	for (uint64_t fn_id : order) {
		const auto &facts = by_function[fn_id];
		Evidence ev;
		ev.category = rule.category;
		ev.confidence = minConfidence(facts);
		auto vars = buildTemplateVars(facts, rule.name);
		ev.title = formatTemplate(rule.output.title_template, vars);
		for (const auto &f : facts)
			ev.items.push_back(toEvidenceItem(f));
		result.push_back(std::move(ev));
	}
	return result;
}

// Count: emit a single Evidence with count = needs[0] match count,
// items empty. Used for aggregate counts (e.g. total TODO comments).
std::vector<Evidence> combineCount(store::GraphStore *store,
				   uint64_t project_id, const Rule &rule)
{
	std::vector<Evidence> result;
	if (rule.needs.empty())
		return result;
	auto primary = queryFactsForNeed(store, project_id, rule.needs[0]);
	Evidence ev;
	ev.category = rule.category;
	ev.confidence = 1.0;
	auto vars = buildTemplateVars(primary, rule.name);
	ev.title = formatTemplate(rule.output.title_template, vars);
	// items intentionally left empty for Count combine.
	result.push_back(std::move(ev));
	return result;
}

// Dispatch a single Rule through its CombineMode. Returns 0+ Evidence
// values (0 = no matches, 1 = Collect/MissingMatch/Count, N =
// MissingMatchPerFunction).
std::vector<Evidence> applyRule(store::GraphStore *store, uint64_t project_id,
				const Rule &rule)
{
	switch (rule.combine) {
	case CombineMode::Collect:
		return combineCollect(store, project_id, rule);
	case CombineMode::MissingMatch:
		return combineMissingMatch(store, project_id, rule);
	case CombineMode::MissingMatchPerFunction:
		return combineMissingMatchPerFunction(store, project_id, rule);
	case CombineMode::Count:
		return combineCount(store, project_id, rule);
	}
	return {};
}

} // namespace

// ─── EvidenceBuilder public API ──────────────────────────────────

void EvidenceBuilder::loadRules(const std::string &dir_path)
{
	RuleLoader loader;
	rule_sets_ = loader.loadFromDirectory(dir_path);
}

std::vector<Evidence> EvidenceBuilder::buildAll(uint64_t project_id) const
{
	std::vector<Evidence> result;
	if (!store_) {
		fprintf(stderr, "[module=evidence, method=buildAll] "
				"store is null\n");
		return result;
	}
	for (const auto &rs : rule_sets_) {
		for (const auto &rule : rs.rules) {
			auto evs = applyRule(store_, project_id, rule);
			for (auto &ev : evs)
				result.push_back(std::move(ev));
		}
	}
	return result;
}

std::vector<Evidence>
EvidenceBuilder::buildByCategory(uint64_t project_id,
				 const std::string &category) const
{
	std::vector<Evidence> result;
	if (!store_) {
		fprintf(stderr, "[module=evidence, method=buildByCategory] "
				"store is null\n");
		return result;
	}
	for (const auto &rs : rule_sets_) {
		if (rs.category != category)
			continue;
		for (const auto &rule : rs.rules) {
			auto evs = applyRule(store_, project_id, rule);
			for (auto &ev : evs)
				result.push_back(std::move(ev));
		}
	}
	return result;
}

std::vector<Evidence>
EvidenceBuilder::buildByRule(uint64_t project_id,
			     const std::string &rule_name) const
{
	std::vector<Evidence> result;
	if (!store_) {
		fprintf(stderr, "[module=evidence, method=buildByRule] "
				"store is null\n");
		return result;
	}
	for (const auto &rs : rule_sets_) {
		for (const auto &rule : rs.rules) {
			if (rule.name != rule_name)
				continue;
			auto evs = applyRule(store_, project_id, rule);
			for (auto &ev : evs)
				result.push_back(std::move(ev));
		}
	}
	return result;
}

} // namespace evidence

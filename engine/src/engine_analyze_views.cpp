// engine_analyze_views.cpp — read-only task-context views of analyze_project.
//
// development_plan §18.7.2: an Agent asks for the context around one symbol or
// module and gets definitions, direct callers/callees, findings and test
// candidates, each tagged with its source and bounded by an explicit budget.
//
// The SQL lives in engine_analyze_queries.cpp; this file owns the JSON shape and
// the rule that a failed read is written as `unknown`, never as an empty list.

#include "engine_analyze_views.h"

#include "engine_analyze_queries.h"
#include "engine_internal.h"
#include "util/json_parse.h"
#include "util/json_writer.h"

#include <sqlite3.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace engine_views
{
namespace
{

/// Read a string field from a parsed object; missing means empty.
std::string stringField(const util::JsonValue &obj, const std::string &key)
{
	const util::JsonValue *value = obj.find(key);
	if (!value || value->type != util::JsonValue::Type::String)
		return "";
	return value->string_value;
}

/// Parse scope_json into a JSON object.
///
/// Every view parses its scope through this function so they validate the same
/// way: empty input, invalid JSON, and a non-object are all errors.
/// @param scope_json  NUL-terminated JSON string.
/// @param[out] root   Parsed JSON value.
/// @param[out] error  Error message on failure.
/// @return false when the input is not a JSON object.
bool parseObjectScope(const char *scope_json, util::JsonValue &root,
		      std::string &error)
{
	if (!scope_json || !*scope_json) {
		error = "scope is empty";
		return false;
	}
	std::string parse_error;
	if (!util::parseJsonDocument(scope_json, root, parse_error)) {
		error = "scope is not valid JSON: " + parse_error;
		return false;
	}
	if (root.type != util::JsonValue::Type::Object) {
		error = "scope must be a JSON object";
		return false;
	}
	return true;
}

} // namespace

bool parseScope(const char *scope_json, Scope &out, std::string &error)
{
	util::JsonValue root;
	if (!parseObjectScope(scope_json, root, error))
		return false;
	out.symbol = stringField(root, "symbol");
	out.module = stringField(root, "module");
	out.file_filter = stringField(root, "file_filter");

	// `src/` and `src` must address the same module: the query builds a
	// `src//%` prefix otherwise, which silently drops every sub-directory.
	while (out.module.size() > 1 && out.module.back() == '/')
		out.module.pop_back();
	while (out.module.size() > 2 && out.module.compare(0, 2, "./") == 0)
		out.module.erase(0, 2);
	return true;
}

namespace
{

// ── Section writers ─────────────────────────────────────────────

void writeFreshness(util::JsonWriter &w, const RunInfo &run)
{
	w.key("freshness").beginObject();
	if (run.run_id.empty()) {
		w.key("status").value("unknown");
		w.key("reason").value("no verification run found");
	} else {
		w.key("run_id").value(run.run_id);
		w.key("status").value(run.status);
	}
	if (!run.ok)
		w.key("query").value("failed");
	w.key("source").value(run.ok ? "fact" : "unknown");
	w.endObject();
}

void writeDefinition(util::JsonWriter &w, const EntityRow &e)
{
	w.key("definition").beginObject();
	w.key("id").value(e.id);
	w.key("name").value(e.name);
	w.key("qualified_name").value(e.qualified_name);
	w.key("kind").value(e.kind);
	w.key("file_path").value(e.file_path);
	w.key("start_row").value(e.start_row);
	w.key("language").value(e.language);
	w.key("module_path").value(e.module_path);
	w.key("source").value("fact");
	w.endObject();
}

/// Write a capped relation page. `total` comes from the count query, so a
/// truncated list says so instead of pretending it is complete.
void writeRelations(util::JsonWriter &w, const char *label,
		    const Page<RelationRow> &page, int max_count)
{
	w.key(label).beginObject();
	if (!page.ok) {
		w.key("status").value("unknown");
		w.key("reason").value("relation query failed");
		w.key("source").value("unknown");
		w.endObject();
		return;
	}
	const int returned =
		std::min(static_cast<int>(page.rows.size()), max_count);
	w.key("items").beginArray();
	for (int i = 0; i < returned; ++i) {
		const auto &r = page.rows[i];
		w.beginObject();
		w.key("entity_name").value(r.entity_name);
		w.key("file_path").value(r.file_path);
		w.key("line").value(r.line);
		w.key("edge_type").value(r.edge_type);
		w.key("source").value("fact");
		w.endObject();
	}
	w.endArray();
	w.key("total").value(page.total);
	w.key("returned").value(returned);
	w.key("has_more").value(page.total > returned);
	w.key("source").value("fact");
	w.endObject();
}

/// Write the findings page. A failed query is `unknown`, not "no findings"
/// (§17.5): an empty list here reads as a clean file.
void writeFindings(util::JsonWriter &w, const Page<FindingRow> &page)
{
	w.key("findings").beginObject();
	if (!page.ok) {
		w.key("status").value("unknown");
		w.key("reason").value("findings query failed");
		w.key("source").value("unknown");
		w.endObject();
		return;
	}
	const int returned =
		std::min(static_cast<int>(page.rows.size()), kMaxFindings);
	w.key("items").beginArray();
	for (int i = 0; i < returned; ++i) {
		const auto &f = page.rows[i];
		w.beginObject();
		w.key("finding_key").value(f.finding_key);
		w.key("dimension").value(f.dimension);
		w.key("severity").value(f.severity);
		w.key("title").value(f.title);
		w.key("file_path").value(f.file_path);
		w.key("line").value(f.line);
		w.key("source").value("fact");
		w.endObject();
	}
	w.endArray();
	w.key("total").value(page.total);
	w.key("returned").value(returned);
	w.key("has_more").value(page.total > returned);
	w.key("source").value("fact");
	w.endObject();
}

void writeTestCandidates(util::JsonWriter &w, const Page<std::string> &page)
{
	w.key("test_candidates").beginObject();
	if (!page.ok) {
		w.key("status").value("unknown");
		w.key("reason").value("test-candidate query failed");
		w.key("source").value("unknown");
		w.endObject();
		return;
	}
	w.key("items").beginArray();
	for (const auto &path : page.rows)
		w.value(path);
	w.endArray();
	w.key("total").value(static_cast<int64_t>(page.rows.size()));
	w.key("source").value("inferred");
	w.key("note").value("test files referencing this symbol; not executed");
	w.endObject();
}

/// Write the unknowns section. `failed` names sections whose query did not run.
void writeUnknowns(util::JsonWriter &w, bool has_run,
		   const std::vector<std::string> &failed)
{
	w.key("unknowns").beginArray();
	if (!has_run)
		w.value("verification_run: no run found, freshness and findings "
			"may be stale");
	for (const auto &name : failed)
		w.value("query failed: " + name);
	w.value("test_candidates: inferred from naming, not from execution");
	w.value("runtime_behavior: static analysis only, no dynamic evidence");
	w.endArray();
}

void writeNextQueries(util::JsonWriter &w, const std::string &view)
{
	w.key("next_queries").beginArray();
	if (view == "symbol_context") {
		w.value("analyze_project(profile=\"quick\", "
			"view=\"module_context\", scope={module: "
			"\"<module_path>\"})");
		w.value("verify_claim(claim={type: "
			"\"capability_exists\", subject: \"<symbol>\"})");
	} else {
		w.value("analyze_project(profile=\"quick\", "
			"view=\"symbol_context\", scope={symbol: "
			"\"<entity_name>\"})");
		w.value("get_project_briefing()");
	}
	w.endArray();
}

/// A query that failed must be visible in `unknowns`, not only in its section.
void collectFailure(std::vector<std::string> &failed, bool ok, const char *name)
{
	if (!ok)
		failed.push_back(name);
}

} // namespace

char *buildSymbolContext(EngineContext *ctx, uint64_t project_id,
			 const Scope &scope)
{
	sqlite3 *db = ctx->store->handle();
	util::JsonWriter w;
	w.beginObject();
	w.key("view").value("symbol_context");
	w.key("project_id").value(project_id);

	// Read the run up front: every branch reports freshness, and claiming
	// "no run found" on a symbol typo would be a statement nobody queried.
	const RunInfo run = queryLatestRun(db, project_id);

	// Resolve entity. A failed lookup is an error, not "symbol not found":
	// telling the caller the symbol does not exist when the query died would
	// send it looking for a rename that never happened.
	Page<EntityRow> resolved = resolveEntities(db, project_id, scope.symbol,
						   scope.file_filter);
	if (!resolved.ok)
		return dupString(util::errorEnvelope(
			"ffi", "engine_analyze_project",
			"entity lookup failed; cannot answer whether the symbol "
			"exists"));
	if (resolved.rows.empty()) {
		w.key("error").value("symbol not found");
		w.key("symbol").value(scope.symbol);
		w.key("source").value("fact");
		std::vector<std::string> failed;
		collectFailure(failed, run.ok, "verification_run");
		writeFreshness(w, run);
		writeUnknowns(w, !run.run_id.empty(), failed);
		writeNextQueries(w, "symbol_context");
		w.endObject();
		return dupString(w.str());
	}

	// Ambiguity: report candidates instead of guessing (development_plan
	// §11.1: duplicate names return candidates).
	if (resolved.total > 1) {
		const int returned =
			std::min(static_cast<int>(resolved.rows.size()),
				 kMaxResolvedEntities);
		w.key("ambiguous").value(true);
		w.key("symbol").value(scope.symbol);
		w.key("candidates").beginArray();
		for (int i = 0; i < returned; ++i) {
			const auto &e = resolved.rows[i];
			w.beginObject();
			w.key("name").value(e.name);
			w.key("qualified_name").value(e.qualified_name);
			w.key("file_path").value(e.file_path);
			w.key("line").value(e.start_row);
			w.endObject();
		}
		w.endArray();
		w.key("total").value(resolved.total);
		w.key("returned").value(returned);
		w.key("has_more").value(resolved.total > returned);
		w.key("source").value("fact");
		w.key("note").value("multiple entities match; narrow with "
				    "file_filter");
		std::vector<std::string> failed;
		collectFailure(failed, run.ok, "verification_run");
		writeFreshness(w, run);
		writeUnknowns(w, !run.run_id.empty(), failed);
		writeNextQueries(w, "symbol_context");
		w.endObject();
		return dupString(w.str());
	}

	const EntityRow &entity = resolved.rows.front();
	writeDefinition(w, entity);

	std::vector<std::string> failed;
	const Page<RelationRow> callers =
		queryRelations(db, project_id, entity.id, false);
	collectFailure(failed, callers.ok, "callers");
	writeRelations(w, "callers", callers, kMaxCallers);
	const Page<RelationRow> callees =
		queryRelations(db, project_id, entity.id, true);
	collectFailure(failed, callees.ok, "callees");
	writeRelations(w, "callees", callees, kMaxCallees);

	const Page<FindingRow> findings =
		queryFindings(db, project_id, entity.file_path);
	collectFailure(failed, findings.ok, "findings");
	writeFindings(w, findings);

	const Page<std::string> tests =
		queryTestCandidates(db, project_id, scope.symbol);
	collectFailure(failed, tests.ok, "test_candidates");
	writeTestCandidates(w, tests);

	collectFailure(failed, run.ok, "verification_run");
	writeFreshness(w, run);
	writeUnknowns(w, !run.run_id.empty(), failed);
	writeNextQueries(w, "symbol_context");

	w.endObject();
	if (!w.ok())
		return dupString(util::errorEnvelope(
			"ffi", "engine_analyze_project", w.error()));
	return dupString(w.str());
}

char *buildModuleContext(EngineContext *ctx, uint64_t project_id,
			 const Scope &scope)
{
	sqlite3 *db = ctx->store->handle();
	util::JsonWriter w;
	w.beginObject();
	w.key("view").value("module_context");
	w.key("project_id").value(project_id);
	w.key("module").value(scope.module);

	const Page<EntityRow> entities =
		queryModuleEntities(db, project_id, scope.module);
	const int returned = std::min(static_cast<int>(entities.rows.size()),
				      kMaxModuleEntities);
	w.key("entities").beginObject();
	if (!entities.ok) {
		w.key("status").value("unknown");
		w.key("reason").value("entity query failed");
		w.key("source").value("unknown");
	} else {
		w.key("items").beginArray();
		for (int i = 0; i < returned; ++i) {
			const auto &e = entities.rows[i];
			w.beginObject();
			w.key("name").value(e.name);
			w.key("kind").value(e.kind);
			w.key("file_path").value(e.file_path);
			w.key("line").value(e.start_row);
			w.key("language").value(e.language);
			w.endObject();
		}
		w.endArray();
		w.key("total").value(entities.total);
		w.key("returned").value(returned);
		w.key("has_more").value(entities.total > returned);
		w.key("source").value("fact");
	}
	w.endObject();

	std::vector<std::string> failed;
	collectFailure(failed, entities.ok, "entities");
	const Page<FindingRow> findings =
		queryFindings(db, project_id, scope.module);
	collectFailure(failed, findings.ok, "findings");
	writeFindings(w, findings);

	const RunInfo run = queryLatestRun(db, project_id);
	collectFailure(failed, run.ok, "verification_run");
	writeFreshness(w, run);
	writeUnknowns(w, !run.run_id.empty(), failed);
	writeNextQueries(w, "module_context");

	w.endObject();
	if (!w.ok())
		return dupString(util::errorEnvelope(
			"ffi", "engine_analyze_project", w.error()));
	return dupString(w.str());
}

} // namespace engine_views

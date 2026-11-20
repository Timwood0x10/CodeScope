// engine_analyze_views.cpp — read-only task-context views of analyze_project.
//
// development_plan §18.7.2: an Agent asks for the context around one symbol or
// module and gets definitions, direct callers/callees, findings and test
// candidates, each tagged with its source and bounded by an explicit budget.
//
// The SQL lives in engine_analyze_queries.cpp; this file owns the JSON shape and
// the rule that a failed read is written as `unknown`, never as an empty list.

#include "engine_analyze_views.h"

#include "arch/gen_defects.h"
#include "arch/incremental_verify.h"
#include "arch/robustness.h"
#include "engine_analyze_queries.h"
#include "engine_internal.h"
#include "util/json_parse.h"
#include "util/json_writer.h"
#include "verify/drift_adapter.h"

#include <sqlite3.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine_views
{
namespace
{

/// Read a text column and JSON-escape it for safe embedding.
std::string columnTextEscaped(sqlite3_stmt *stmt, int col)
{
	const unsigned char *text = sqlite3_column_text(stmt, col);
	return util::jsonEscapeString(
		text ? reinterpret_cast<const char *>(text) : "");
}

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

bool parseDriftHistoryScope(const char *scope_json, std::string &run_id,
			    std::string &drift_type, std::string &error)
{
	// Empty scope is valid — means no filter.
	if (!scope_json || !*scope_json)
		return true;

	util::JsonValue root;
	if (!parseObjectScope(scope_json, root, error))
		return false;

	// Scope overrides run_id if provided.
	std::string scope_run = stringField(root, "run_id");
	if (!scope_run.empty())
		run_id = scope_run;
	drift_type = stringField(root, "drift_type");
	return true;
}

bool parseRunIds(const char *scope_json, std::string &base_run_id,
		 std::string &target_run_id, std::string &error)
{
	util::JsonValue root;
	if (!parseObjectScope(scope_json, root, error))
		return false;
	base_run_id = stringField(root, "base_run_id");
	target_run_id = stringField(root, "target_run_id");
	if (base_run_id.empty()) {
		error = "scope.base_run_id is required for "
			"compare_snapshots view";
		return false;
	}
	if (target_run_id.empty()) {
		error = "scope.target_run_id is required for "
			"compare_snapshots view";
		return false;
	}
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

/// Write the test-candidate page.
///
/// Each item says HOW the association was made: a call edge from a test entity
/// is evidence, a name match in a test file is a hint (§12.2). Reporting them as
/// one undifferentiated list would let a coincidental match read as coverage.
void writeTestCandidates(util::JsonWriter &w, const Page<TestCandidate> &page)
{
	w.key("test_candidates").beginObject();
	if (!page.ok) {
		w.key("status").value("unknown");
		w.key("reason").value("test-candidate query failed");
		w.key("source").value("unknown");
		w.endObject();
		return;
	}
	const int returned = std::min(static_cast<int>(page.rows.size()),
				      kMaxTestCandidates);
	w.key("items").beginArray();
	for (int i = 0; i < returned; ++i) {
		const auto &c = page.rows[i];
		w.beginObject();
		w.key("file_path").value(c.file_path);
		if (!c.entity.empty())
			w.key("test_entity").value(c.entity);
		if (c.line > 0)
			w.key("line").value(c.line);
		w.key("method").value(c.method);
		w.key("confidence").value(c.confidence);
		w.endObject();
	}
	w.endArray();
	w.key("total").value(page.total);
	w.key("returned").value(returned);
	w.key("has_more").value(page.total > returned);
	w.key("source").value("fact");
	w.key("note").value(
		"call_edge = a test entity calls the symbol (measured); "
		"name_match = a test file mentions the name (hint only)");
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
				    "file_filter. C/C++ symbols usually appear "
				    "twice (declaration and definition); "
				    "file_filter restricts which source rows "
				    "count as this symbol but does not limit "
				    "the files of callers/callees");
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
	const Page<RelationRow> callers = queryRelations(
		db, project_id, entity.id, false, scope.file_filter);
	collectFailure(failed, callers.ok, "callers");
	writeRelations(w, "callers", callers, kMaxCallers);
	const Page<RelationRow> callees = queryRelations(
		db, project_id, entity.id, true, scope.file_filter);
	collectFailure(failed, callees.ok, "callees");
	writeRelations(w, "callees", callees, kMaxCallees);

	const Page<FindingRow> findings =
		queryFindings(db, project_id, entity.file_path);
	collectFailure(failed, findings.ok, "findings");
	writeFindings(w, findings);

	const Page<TestCandidate> tests = queryTestCandidates(
		db, project_id, entity.id, scope.symbol, scope.file_filter);
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

// ── change_context view (§18.7.2, §18.6) ───────────────────────

bool parseFileList(const char *scope_json, std::vector<std::string> &files,
		   std::string &error)
{
	util::JsonValue root;
	if (!parseObjectScope(scope_json, root, error))
		return false;

	const util::JsonValue *arr = root.find("files");
	if (!arr || arr->type != util::JsonValue::Type::Array) {
		error = "scope.files is required and must be an array";
		return false;
	}

	files.clear();
	for (const auto &val : arr->array_value) {
		if (val.type == util::JsonValue::Type::String)
			files.push_back(val.string_value);
	}

	if (files.empty()) {
		error = "scope.files array is empty";
		return false;
	}
	return true;
}

char *buildChangeContext(EngineContext *ctx, uint64_t project_id,
			 const std::vector<std::string> &files)
{
	sqlite3 *db = ctx->store->handle();
	util::JsonWriter w;
	w.beginObject();
	w.key("view").value("change_context");
	w.key("project_id").value(project_id);

	// Report the input files.
	w.key("modified_files").beginObject();
	w.key("items").beginArray();
	for (const auto &f : files)
		w.value(f);
	w.endArray();
	w.key("total").value(static_cast<int64_t>(files.size()));
	w.key("source").value("input");
	w.endObject();

	std::vector<std::string> failed;

	// 1. Find affected entities (entities in the modified files).
	w.key("affected_entities").beginObject();
	int64_t affected_entity_count = 0;
	{
		sqlite3_stmt *stmt = nullptr;
		// Build a query that finds entities in any of the modified
		// files. We use a parameterized IN (...) by building the
		// placeholder list manually — file paths are user input but
		// bound as text parameters, so injection-safe.
		std::string sql = "SELECT id, name, qualified_name, kind, "
				  "file_path, start_row, language "
				  "FROM entity WHERE project_id=? AND "
				  "file_path IN (";
		for (size_t i = 0; i < files.size(); ++i) {
			if (i > 0)
				sql += ",";
			sql += "?";
		}
		sql += ") ORDER BY file_path, start_row";

		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			for (size_t i = 0; i < files.size(); ++i)
				sqlite3_bind_text(stmt, i + 2, files[i].c_str(),
						  -1, SQLITE_TRANSIENT);

			w.key("items").beginArray();
			int returned = 0;
			while (sqlite3_step(stmt) == SQLITE_ROW &&
			       returned < kMaxModuleEntities) {
				EntityRow e;
				e.id = sqlite3_column_int64(stmt, 0);
				e.name = columnTextEscaped(stmt, 1);
				e.qualified_name = columnTextEscaped(stmt, 2);
				e.kind = sqlite3_column_int(stmt, 3);
				e.file_path = columnTextEscaped(stmt, 4);
				e.start_row = sqlite3_column_int64(stmt, 5);
				e.language = columnTextEscaped(stmt, 6);
				w.beginObject();
				w.key("id").value(e.id);
				w.key("name").value(e.name);
				w.key("qualified_name").value(e.qualified_name);
				w.key("kind").value(e.kind);
				w.key("file_path").value(e.file_path);
				w.key("start_row").value(e.start_row);
				w.key("language").value(e.language);
				w.endObject();
				++returned;
				++affected_entity_count;
			}
			w.endArray();

			// Count total affected (may be more than returned).
			if (sqlite3_column_count(stmt) > 0) {
				sqlite3_reset(stmt);
				sqlite3_clear_bindings(stmt);
			}
			sqlite3_finalize(stmt);

			// Get the exact total with a count query.
			stmt = nullptr;
			std::string count_sql =
				"SELECT COUNT(*) FROM entity "
				"WHERE project_id=? AND file_path IN (";
			for (size_t i = 0; i < files.size(); ++i) {
				if (i > 0)
					count_sql += ",";
				count_sql += "?";
			}
			count_sql += ")";
			if (sqlite3_prepare_v2(db, count_sql.c_str(), -1, &stmt,
					       nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(
					stmt, 1,
					static_cast<int64_t>(project_id));
				for (size_t i = 0; i < files.size(); ++i)
					sqlite3_bind_text(stmt, i + 2,
							  files[i].c_str(), -1,
							  SQLITE_TRANSIENT);
				if (sqlite3_step(stmt) == SQLITE_ROW)
					affected_entity_count =
						sqlite3_column_int64(stmt, 0);
				sqlite3_finalize(stmt);
			}

			w.key("total").value(affected_entity_count);
			w.key("returned")
				.value(std::min(affected_entity_count,
						static_cast<int64_t>(
							kMaxModuleEntities)));
			w.key("has_more")
				.value(affected_entity_count >
				       static_cast<int64_t>(
					       kMaxModuleEntities));
			w.key("source").value("fact");
		} else {
			w.key("status").value("unknown");
			w.key("reason").value("entity query failed");
			w.key("source").value("unknown");
			failed.push_back("affected_entities");
		}
	}
	w.endObject();

	// 2. Find affected modules (modules containing the modified files).
	w.key("affected_modules").beginObject();
	{
		sqlite3_stmt *stmt = nullptr;
		// Derive module paths from file paths and check arch_module.
		std::string sql = "SELECT DISTINCT path FROM arch_module "
				  "WHERE project_id=? AND (";
		bool first = true;
		for (size_t fi = 0; fi < files.size(); ++fi) {
			(void)fi;
			if (!first)
				sql += " OR ";
			sql += "substr(?, 1, length(path) + 1) = path || '/'";
			first = false;
		}
		sql += ")";

		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			for (size_t i = 0; i < files.size(); ++i)
				sqlite3_bind_text(stmt, i + 2, files[i].c_str(),
						  -1, SQLITE_TRANSIENT);

			w.key("items").beginArray();
			int mod_count = 0;
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const unsigned char *path =
					sqlite3_column_text(stmt, 0);
				if (path) {
					w.value(reinterpret_cast<const char *>(
						path));
					++mod_count;
				}
			}
			w.endArray();
			w.key("total").value(mod_count);
			w.key("source").value("fact");
			sqlite3_finalize(stmt);
		} else {
			w.key("status").value("unknown");
			w.key("reason").value("module query failed");
			w.key("source").value("unknown");
			failed.push_back("affected_modules");
		}
	}
	w.endObject();

	// 3. Find existing findings on the affected files.
	w.key("affected_findings").beginObject();
	{
		Page<FindingRow> all_findings;
		for (const auto &f : files) {
			auto page = queryFindings(db, project_id, f);
			if (!page.ok) {
				all_findings.ok = false;
				continue;
			}
			for (auto &row : page.rows)
				all_findings.rows.push_back(std::move(row));
			all_findings.total += page.total;
		}
		if (!all_findings.ok) {
			w.key("status").value("unknown");
			w.key("reason").value("findings query failed");
			w.key("source").value("unknown");
			failed.push_back("affected_findings");
		} else {
			// Deduplicate by finding_key.
			std::unordered_set<std::string> seen;
			std::vector<FindingRow> deduped;
			for (auto &row : all_findings.rows) {
				if (seen.insert(row.finding_key).second)
					deduped.push_back(std::move(row));
			}
			w.key("items").beginArray();
			int returned = std::min(
				static_cast<int>(deduped.size()), kMaxFindings);
			for (int i = 0; i < returned; ++i) {
				const auto &f = deduped[i];
				w.beginObject();
				w.key("finding_key").value(f.finding_key);
				w.key("dimension").value(f.dimension);
				w.key("severity").value(f.severity);
				w.key("title").value(f.title);
				w.key("file_path").value(f.file_path);
				w.key("line").value(f.line);
				w.endObject();
			}
			w.endArray();
			w.key("total").value(
				static_cast<int64_t>(deduped.size()));
			w.key("returned").value(returned);
			w.key("has_more")
				.value(static_cast<int>(deduped.size()) >
				       returned);
			w.key("source").value("fact");
		}
	}
	w.endObject();

	// 4. Report structural edges touching affected modules.
	w.key("affected_structural_edges").beginObject();
	{
		sqlite3_stmt *stmt = nullptr;
		// Find arch_edge rows where src or dst module starts with
		// any affected module path. This is a conservative
		// over-approximation (§18.6: "变更前后依赖并集").
		std::string sql =
			"SELECT ae.src_module, ae.dst_module, ae.edge_kind, "
			"       ae.resolution_kind, ae.confidence "
			"FROM arch_edge ae WHERE ae.project_id=? AND (";
		bool first = true;
		for (size_t fi = 0; fi < files.size(); ++fi) {
			(void)fi;
			if (!first)
				sql += " OR ";
			sql += "ae.src_module LIKE substr(?, 1, "
			       "length(?)) || '%' OR "
			       "ae.dst_module LIKE substr(?, 1, "
			       "length(?)) || '%'";
			first = false;
		}
		sql += ") LIMIT 50";

		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			int param = 2;
			for (const auto &f : files) {
				sqlite3_bind_text(stmt, param++, f.c_str(), -1,
						  SQLITE_TRANSIENT);
				sqlite3_bind_text(stmt, param++, f.c_str(), -1,
						  SQLITE_TRANSIENT);
				sqlite3_bind_text(stmt, param++, f.c_str(), -1,
						  SQLITE_TRANSIENT);
				sqlite3_bind_text(stmt, param++, f.c_str(), -1,
						  SQLITE_TRANSIENT);
			}

			w.key("items").beginArray();
			int edge_count = 0;
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				w.beginObject();
				w.key("src_module")
					.value(columnTextEscaped(stmt, 0));
				w.key("dst_module")
					.value(columnTextEscaped(stmt, 1));
				w.key("edge_kind")
					.value(columnTextEscaped(stmt, 2));
				w.key("resolution_kind")
					.value(columnTextEscaped(stmt, 3));
				w.key("confidence")
					.value(sqlite3_column_double(stmt, 4));
				w.endObject();
				++edge_count;
			}
			w.endArray();
			w.key("total").value(edge_count);
			w.key("source").value("fact");
			sqlite3_finalize(stmt);
		} else {
			w.key("status").value("unknown");
			w.key("reason").value("structural edge query failed");
			w.key("source").value("unknown");
			failed.push_back("affected_structural_edges");
		}
	}
	w.endObject();

	// 5. Scope expansion reason (§18.6.1).
	w.key("scope_expansion").beginObject();
	w.key("policy").value("file_level_incremental");
	w.key("reason").value("M2 first version: file-level parse "
			      "increment + module graph full recompute + "
			      "conservative affected-rule re-verification");
	w.key("global_recompute").beginArray();
	w.value("module_graph");
	w.value("scc");
	w.endArray();
	w.key("source").value("declared");
	w.endObject();

	// 6. Freshness and unknowns.
	const RunInfo run = queryLatestRun(db, project_id);
	collectFailure(failed, run.ok, "verification_run");
	writeFreshness(w, run);
	writeUnknowns(w, !run.run_id.empty(), failed);
	writeNextQueries(w, "change_context");

	w.endObject();
	if (!w.ok())
		return dupString(util::errorEnvelope(
			"ffi", "engine_analyze_project", w.error()));
	return dupString(w.str());
}

// ── robustness view (§5.4 D4, §12.4 M3-1) ───────────────────────

char *buildRobustness(EngineContext *ctx, uint64_t project_id)
{
	// Compute D4 robustness observations from the module graph.
	auto result = arch::computeRobustness(*ctx->store, project_id);

	util::JsonWriter w;
	w.beginObject();
	w.key("view").value("robustness");
	w.key("project_id").value(project_id);

	if (!result.error.empty()) {
		w.key("status").value("error");
		w.key("error").value(result.error);
		w.key("source").value("inferred");
		w.endObject();
		if (!w.ok())
			return dupString(util::errorEnvelope(
				"ffi", "engine_analyze_project", w.error()));
		return dupString(w.str());
	}

	w.key("status").value("observations");
	w.key("dimension").value("D4");
	w.key("source").value("inferred");

	// Group observations by category for readability.
	w.key("observations").beginArray();
	for (const auto &obs : result.observations) {
		w.beginObject();
		w.key("category").value(obs.category);
		w.key("metric").value(obs.metric);
		w.key("value").value(obs.value);
		w.key("target").value(obs.target);
		w.key("description").value(obs.description);
		w.key("source").value(obs.source);
		w.endObject();
	}
	w.endArray();

	w.key("total_observations")
		.value(static_cast<int64_t>(result.observations.size()));

	// Summary counts per category.
	std::unordered_map<std::string, int> by_category;
	for (const auto &obs : result.observations)
		++by_category[obs.category];

	w.key("summary").beginObject();
	w.key("concentration").value(by_category["concentration"]);
	w.key("propagation").value(by_category["propagation"]);
	w.key("boundary").value(by_category["boundary"]);
	w.endObject();

	// All D4 outputs are observations in 0.3.x (§5.4).
	w.key("verdict").value("observation_only");
	w.key("note").value(
		"D4 observations do not produce violations in 0.3.x "
		"unless a declared threshold exists (§5.4)");

	w.endObject();
	if (!w.ok())
		return dupString(util::errorEnvelope(
			"ffi", "engine_analyze_project", w.error()));
	return dupString(w.str());
}

// ── gen_defects view (§8.4, §12.4 M3-2) ─────────────────────────

char *buildGenDefects(EngineContext *ctx, uint64_t project_id)
{
	auto result = arch::detectGenDefects(*ctx->store, project_id);

	util::JsonWriter w;
	w.beginObject();
	w.key("view").value("gen_defects");
	w.key("project_id").value(project_id);

	if (!result.error.empty()) {
		w.key("status").value("error");
		w.key("error").value(result.error);
		w.key("source").value("inferred");
		w.endObject();
		if (!w.ok())
			return dupString(util::errorEnvelope(
				"ffi", "engine_analyze_project", w.error()));
		return dupString(w.str());
	}

	w.key("status").value("candidates");
	w.key("dimension").value("D2");
	w.key("source").value("inferred");

	w.key("candidates").beginArray();
	for (const auto &d : result.candidates) {
		w.beginObject();
		w.key("category").value(d.category);
		w.key("description").value(d.description);
		w.key("target").value(d.target);
		w.key("file_path").value(d.file_path);
		w.key("row").value(d.row);
		w.key("confidence").value(d.confidence);
		w.key("source").value(d.source);
		w.key("evidence_basis").value(d.evidence_basis);
		w.endObject();
	}
	w.endArray();

	w.key("total_candidates")
		.value(static_cast<int64_t>(result.candidates.size()));

	// Summary counts per category.
	std::unordered_map<std::string, int> by_category;
	for (const auto &d : result.candidates)
		++by_category[d.category];

	w.key("summary").beginObject();
	w.key("dependency_anomaly").value(by_category["dependency_anomaly"]);
	w.key("shell_implementation").value(by_category["shell_implementation"]);
	w.key("duplicate_implementation")
		.value(by_category["duplicate_implementation"]);
	w.key("invalid_abstraction").value(by_category["invalid_abstraction"]);
	w.endObject();

	w.key("verdict").value("observation_only");
	w.key("note").value(
		"All §8.4 detections are L2 observations — candidates, "
		"not violations. Source cannot prove generative origin.");

	w.endObject();
	if (!w.ok())
		return dupString(util::errorEnvelope(
			"ffi", "engine_analyze_project", w.error()));
	return dupString(w.str());
}

// ── reverify view (§18.6, §12.4 M3-3) ───────────────────────────

char *buildReverify(EngineContext *ctx, uint64_t project_id,
		    const std::vector<std::string> &files)
{
	auto result = arch::incrementalReverify(*ctx->store, project_id, files);

	util::JsonWriter w;
	w.beginObject();
	w.key("view").value("reverify");
	w.key("project_id").value(project_id);

	if (!result.error.empty()) {
		w.key("status").value("error");
		w.key("error").value(result.error);
		w.endObject();
		if (!w.ok())
			return dupString(util::errorEnvelope(
				"ffi", "engine_analyze_project", w.error()));
		return dupString(w.str());
	}

	w.key("status").value("stale_marked");
	w.key("source").value("inferred");

	w.key("stale_evidence_count").value(result.stale_count);
	w.key("affected_entity_count").value(result.affected_entity_count);
	w.key("affected_module_count").value(result.affected_module_count);
	w.key("affected_rule_count").value(result.affected_rule_count);

	w.key("scope_expansion").beginObject();
	w.key("policy").value("file_level_incremental");
	w.key("reason").value(result.scope_expansion_reason);
	w.key("global_recompute").beginArray();
	w.value("module_graph");
	w.value("scc");
	w.value("concentration");
	w.value("condensed_dag");
	w.endArray();
	w.key("incomplete").value(result.incomplete);
	w.key("source").value("declared");
	w.endObject();

	w.key("next_step")
		.value("Run engine_run_rules to produce fresh evidence for "
		       "the affected scope. Stale evidence is retained as "
		       "history; new evidence is stored with status=observed.");

	w.endObject();
	if (!w.ok())
		return dupString(util::errorEnvelope(
			"ffi", "engine_analyze_project", w.error()));
	return dupString(w.str());
}

// ── drift view (§18.2.2 D, §12.4 M3-4) ──────────────────────────

char *buildDrift(EngineContext *ctx, uint64_t project_id,
		 const std::string &run_id)
{
	auto result =
		verify::detectAndPersistDrift(*ctx->store, project_id, run_id);

	util::JsonWriter w;
	w.beginObject();
	w.key("view").value("drift");
	w.key("project_id").value(project_id);
	w.key("run_id").value(run_id);

	if (!result.error.empty()) {
		w.key("status").value("error");
		w.key("error").value(result.error);
		w.endObject();
		if (!w.ok())
			return dupString(util::errorEnvelope(
				"ffi", "engine_analyze_project", w.error()));
		return dupString(w.str());
	}

	w.key("status").value("detected");
	w.key("source").value("inferred");

	// Architecture drift.
	w.key("architecture").beginArray();
	for (const auto &item : result.architecture) {
		w.beginObject();
		w.key("type").value(item.type);
		w.key("severity").value(item.severity);
		w.key("subject").value(item.subject);
		w.key("detail").value(item.detail);
		w.endObject();
	}
	w.endArray();

	// Capability drift.
	w.key("capability").beginArray();
	for (const auto &item : result.capability) {
		w.beginObject();
		w.key("type").value(item.type);
		w.key("severity").value(item.severity);
		w.key("subject").value(item.subject);
		w.key("detail").value(item.detail);
		w.endObject();
	}
	w.endArray();

	// Documentation drift.
	w.key("documentation").beginArray();
	for (const auto &item : result.documentation) {
		w.beginObject();
		w.key("type").value(item.type);
		w.key("severity").value(item.severity);
		w.key("subject").value(item.subject);
		w.key("detail").value(item.detail);
		w.endObject();
	}
	w.endArray();

	// Summary.
	w.key("summary").beginObject();
	w.key("architecture_count")
		.value(static_cast<int64_t>(result.architecture.size()));
	w.key("capability_count")
		.value(static_cast<int64_t>(result.capability.size()));
	w.key("documentation_count")
		.value(static_cast<int64_t>(result.documentation.size()));
	w.key("total").value(static_cast<int64_t>(result.architecture.size() +
						  result.capability.size() +
						  result.documentation.size()));
	w.key("records_written").value(result.records_written);
	w.endObject();

	w.key("note").value(
		"Architecture drift uses declared layers when available "
		"(arch_layer_decl + arch_edge); falls back to heuristic "
		"classification (inferred) otherwise. Results persisted to "
		"drift_record for historical traceability.");

	w.endObject();
	if (!w.ok())
		return dupString(util::errorEnvelope(
			"ffi", "engine_analyze_project", w.error()));
	return dupString(w.str());
}

// ── drift_history view (§18.5, §12.4 M3-5) ──────────────────────

char *buildDriftHistory(EngineContext *ctx, uint64_t project_id,
			const std::string &run_id,
			const std::string &drift_type)
{
	sqlite3 *db = ctx->store->handle();

	util::JsonWriter w;
	w.beginObject();
	w.key("view").value("drift_history");
	w.key("project_id").value(project_id);

	// Build query with optional filters.
	std::string sql = "SELECT run_id, drift_type, severity, "
			  "description, evidence_json, detected_at "
			  "FROM drift_record WHERE project_id=?";
	if (!run_id.empty())
		sql += " AND run_id=?";
	if (!drift_type.empty())
		sql += " AND drift_type=?";
	sql += " ORDER BY detected_at DESC LIMIT 100";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		w.key("status").value("error");
		w.key("error").value("drift_history query failed");
		w.endObject();
		return dupString(w.str());
	}

	int bind_idx = 1;
	sqlite3_bind_int64(stmt, bind_idx++, static_cast<int64_t>(project_id));
	if (!run_id.empty())
		sqlite3_bind_text(stmt, bind_idx++, run_id.c_str(), -1,
				  SQLITE_TRANSIENT);
	if (!drift_type.empty())
		sqlite3_bind_text(stmt, bind_idx++, drift_type.c_str(), -1,
				  SQLITE_TRANSIENT);

	w.key("status").value("records");
	w.key("source").value("fact");
	w.key("records").beginArray();

	int count = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		auto text = [&](int col) -> std::string {
			const char *t = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, col));
			return t ? t : "";
		};
		w.beginObject();
		w.key("run_id").value(text(0));
		w.key("drift_type").value(text(1));
		w.key("severity").value(sqlite3_column_int(stmt, 2));
		w.key("description").value(text(3));
		w.key("evidence_json").value(text(4));
		w.key("detected_at").value(text(5));
		w.endObject();
		++count;
	}
	sqlite3_finalize(stmt);

	w.endArray();
	w.key("total").value(count);
	w.key("returned").value(count);

	w.endObject();
	if (!w.ok())
		return dupString(util::errorEnvelope(
			"ffi", "engine_analyze_project", w.error()));
	return dupString(w.str());
}

// ── profile_history view (§18.5, §18.6.2, §12.4 M3-5) ───────────

char *buildProfileHistory(EngineContext *ctx, uint64_t project_id, int limit)
{
	sqlite3 *db = ctx->store->handle();

	util::JsonWriter w;
	w.beginObject();
	w.key("view").value("profile_history");
	w.key("project_id").value(project_id);

	std::string sql = "SELECT run_id, status, profile, started_at, "
			  "finished_at, worktree_digest, source_revision "
			  "FROM verification_run WHERE project_id=? "
			  "ORDER BY started_at DESC LIMIT ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		w.key("status").value("error");
		w.key("error").value("profile_history query failed");
		w.endObject();
		return dupString(w.str());
	}

	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 2, limit);

	w.key("status").value("runs");
	w.key("source").value("fact");
	w.key("runs").beginArray();

	int count = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		auto text = [&](int col) -> std::string {
			const char *t = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, col));
			return t ? t : "";
		};
		std::string rid = text(0);

		w.beginObject();
		w.key("run_id").value(rid);
		w.key("status").value(text(1));
		w.key("profile").value(text(2));
		w.key("started_at").value(text(3));
		w.key("finished_at").value(text(4));
		w.key("worktree_digest").value(text(5));
		w.key("source_revision").value(text(6));

		// Query per-dimension snapshots for this run.
		w.key("dimensions").beginArray();
		{
			const char *snap_sql =
				"SELECT dimension, status, metrics_json, "
				"coverage_json, created_at "
				"FROM profile_snapshot "
				"WHERE project_id=? AND run_id=? "
				"ORDER BY dimension";
			sqlite3_stmt *snap_stmt = nullptr;
			if (sqlite3_prepare_v2(db, snap_sql, -1, &snap_stmt,
					       nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(
					snap_stmt, 1,
					static_cast<int64_t>(project_id));
				sqlite3_bind_text(snap_stmt, 2, rid.c_str(), -1,
						  SQLITE_TRANSIENT);
				while (sqlite3_step(snap_stmt) == SQLITE_ROW) {
					auto stext =
						[&](int col) -> std::string {
						const char *t = reinterpret_cast<
							const char *>(
							sqlite3_column_text(
								snap_stmt,
								col));
						return t ? t : "";
					};
					w.beginObject();
					w.key("dimension").value(stext(0));
					w.key("status").value(stext(1));
					w.key("metrics_json").value(stext(2));
					w.key("coverage_json").value(stext(3));
					w.key("created_at").value(stext(4));
					w.endObject();
				}
				sqlite3_finalize(snap_stmt);
			}
		}
		w.endArray();

		w.endObject();
		++count;
	}
	sqlite3_finalize(stmt);

	w.endArray();
	w.key("total").value(count);
	w.key("returned").value(count);

	w.endObject();
	if (!w.ok())
		return dupString(util::errorEnvelope(
			"ffi", "engine_analyze_project", w.error()));
	return dupString(w.str());
}

} // namespace engine_views

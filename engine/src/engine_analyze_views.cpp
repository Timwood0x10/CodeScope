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

namespace
{

/// Build a parameterised IN (?,?,...) placeholder list for `n` values.
std::string inPlaceholders(size_t n)
{
	std::string sql;
	for (size_t i = 0; i < n; ++i) {
		if (i > 0)
			sql += ",";
		sql += "?";
	}
	return sql;
}

/// Bind file paths to a statement starting at param index 2
/// (param 1 is project_id).
void bindFileParams(sqlite3_stmt *stmt, const std::vector<std::string> &files)
{
	for (size_t i = 0; i < files.size(); ++i)
		sqlite3_bind_text(stmt, i + 2, files[i].c_str(), -1,
				  SQLITE_TRANSIENT);
}

/// Section 1 of change_context: affected entities in the modified files.
void writeAffectedEntities(util::JsonWriter &w, sqlite3 *db,
			   uint64_t project_id,
			   const std::vector<std::string> &files,
			   std::vector<std::string> &failed)
{
	w.key("affected_entities").beginObject();
	int64_t affected_entity_count = 0;
	{
		sqlite3_stmt *stmt = nullptr;
		std::string sql = "SELECT id, name, qualified_name, kind, "
				  "file_path, start_row, language "
				  "FROM entity WHERE project_id=? AND "
				  "file_path IN (";
		sql += inPlaceholders(files.size());
		sql += ") ORDER BY file_path, start_row";

		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			bindFileParams(stmt, files);

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
			}
			w.endArray();
			sqlite3_finalize(stmt);

			// Get the exact total with a count query.
			std::string count_sql =
				"SELECT COUNT(*) FROM entity "
				"WHERE project_id=? AND file_path IN (";
			count_sql += inPlaceholders(files.size());
			count_sql += ")";
			if (sqlite3_prepare_v2(db, count_sql.c_str(), -1, &stmt,
					       nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(
					stmt, 1,
					static_cast<int64_t>(project_id));
				bindFileParams(stmt, files);
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
}

/// Section 2 of change_context: affected modules.
void writeAffectedModules(util::JsonWriter &w, sqlite3 *db, uint64_t project_id,
			  const std::vector<std::string> &files,
			  std::vector<std::string> &failed)
{
	w.key("affected_modules").beginObject();
	{
		sqlite3_stmt *stmt = nullptr;
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
			bindFileParams(stmt, files);

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
}

/// Section 3 of change_context: affected findings (deduplicated).
void writeAffectedFindings(util::JsonWriter &w, sqlite3 *db,
			   uint64_t project_id,
			   const std::vector<std::string> &files,
			   std::vector<std::string> &failed)
{
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
}

/// Section 4 of change_context: structural edges touching affected modules.
void writeAffectedStructuralEdges(util::JsonWriter &w, sqlite3 *db,
				  uint64_t project_id,
				  const std::vector<std::string> &files,
				  std::vector<std::string> &failed)
{
	w.key("affected_structural_edges").beginObject();
	{
		sqlite3_stmt *stmt = nullptr;
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
				for (int j = 0; j < 4; ++j)
					sqlite3_bind_text(stmt, param++,
							  f.c_str(), -1,
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
}

} // namespace

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
	writeAffectedEntities(w, db, project_id, files, failed);
	writeAffectedModules(w, db, project_id, files, failed);
	writeAffectedFindings(w, db, project_id, files, failed);
	writeAffectedStructuralEdges(w, db, project_id, files, failed);

	// Scope expansion reason (§18.6.1).
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

	// Freshness and unknowns.
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

} // namespace engine_views

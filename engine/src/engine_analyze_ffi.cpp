// engine_analyze_ffi.cpp — Analyze Project FFI export (M1 task context).
//
// Exposes a read-only entry point:
//
//   char *engine_analyze_project(engine_t handle, uint64_t project_id,
//                                const char *view, const char *scope_json,
//                                const char *run_id);
//
// This is the M1 "task context" from development_plan §18.7.2.
// It returns bounded structural context for a symbol or module so an
// AI Agent can understand what it is about to modify without scanning
// the whole codebase.
//
// Design constraints (plan/rules/code_rules.md §16 + development_plan §18.7.0):
//   - Read-only: no indexing, no rule execution, no target-project scripts.
//   - Every fact carries a "source" field: fact | declared | documented | inferred.
//   - Unknowns are explicit: missing data is "unknown", never a confident "clean".
//   - Deterministic: no LLM, no wall-clock-dependent ordering.
//   - Budget: default ≤256 KiB, hard upper limit 1 MiB; lists are truncated
//     with total/returned/has_more.

#include "engine_internal.h"
#include "async_knowledge.h"
#include "util/json_writer.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace
{

// ── Budget constants (§18.7.0) ─────────────────────────────────
constexpr int kMaxCallers = 30;
constexpr int kMaxCallees = 30;
constexpr int kMaxFindings = 20;
constexpr int kMaxTestCandidates = 15;
constexpr int kMaxModuleEntities = 50;

// ── Row types ───────────────────────────────────────────────────

struct EntityRow {
	int64_t id;
	std::string name;
	std::string qualified_name;
	int kind;
	std::string file_path;
	int64_t start_row;
	std::string language;
	std::string module_path;
};

struct RelationRow {
	int64_t source_id;
	int64_t target_id;
	int type;
	std::string name;
	std::string file_path;
	int64_t line;
};

struct FindingRow {
	std::string finding_key;
	std::string dimension;
	int64_t severity;
	std::string title;
	std::string file_path;
	int64_t line;
};

// ── Scope parsing ───────────────────────────────────────────────

/// Parsed scope from the JSON argument.
struct Scope {
	std::string symbol;
	std::string module;
	std::string file_filter;
};

/// Extract a string value for a key from a flat JSON object.
/// Minimal parser — the Rust side validates the schema before FFI.
std::string extractJsonField(const char *json, const char *key)
{
	std::string pat = "\"";
	pat += key;
	pat += "\"";
	const char *p = std::strstr(json, pat.c_str());
	if (!p)
		return "";
	p += pat.size();
	while (*p && (*p == ' ' || *p == '\t' || *p == ':'))
		++p;
	if (*p != '"')
		return "";
	++p;
	const char *end = p;
	while (*end && *end != '"')
		++end;
	return std::string(p, end - p);
}

Scope parseScope(const char *scope_json)
{
	Scope s;
	if (!scope_json || !*scope_json)
		return s;
	s.symbol = extractJsonField(scope_json, "symbol");
	s.module = extractJsonField(scope_json, "module");
	s.file_filter = extractJsonField(scope_json, "file_filter");
	return s;
}

// ── Entity queries ──────────────────────────────────────────────

/// Helper to read a text column safely.
std::string textCol(sqlite3_stmt *st, int col)
{
	const char *t =
		reinterpret_cast<const char *>(sqlite3_column_text(st, col));
	return t ? t : "";
}

/// Read an entity row from the current statement position.
EntityRow readEntityRow(sqlite3_stmt *st)
{
	EntityRow r;
	r.id = sqlite3_column_int64(st, 0);
	r.name = textCol(st, 1);
	r.qualified_name = textCol(st, 2);
	r.kind = sqlite3_column_int(st, 3);
	r.file_path = textCol(st, 4);
	r.start_row = sqlite3_column_int64(st, 5);
	r.language = textCol(st, 6);
	r.module_path = textCol(st, 7);
	return r;
}

/// Resolve entities matching a symbol name (bare or qualified).
std::vector<EntityRow> resolveEntities(sqlite3 *db, uint64_t project_id,
				       const std::string &name,
				       const std::string &file_filter)
{
	std::string sql = "SELECT id, name, qualified_name, kind, file_path, "
			  "       start_row, language, module_path "
			  "FROM entity WHERE project_id=? "
			  "AND (name=? OR qualified_name=?)";
	bool has_filter = !file_filter.empty();
	if (has_filter)
		sql += " AND file_path LIKE ?";
	sql += " ORDER BY id LIMIT 50";

	sqlite3_stmt *st = nullptr;
	std::vector<EntityRow> rows;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
		return rows;
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, name.c_str(), -1, SQLITE_TRANSIENT);
	if (has_filter) {
		std::string pat = "%" + file_filter + "%";
		sqlite3_bind_text(st, 4, pat.c_str(), -1, SQLITE_TRANSIENT);
	}
	while (sqlite3_step(st) == SQLITE_ROW)
		rows.push_back(readEntityRow(st));
	sqlite3_finalize(st);
	return rows;
}

/// Resolve entities in a module (by module_path prefix).
std::vector<EntityRow> resolveModuleEntities(sqlite3 *db, uint64_t project_id,
					     const std::string &mod_path)
{
	const char *sql = "SELECT id, name, qualified_name, kind, file_path, "
			  "       start_row, language, module_path "
			  "FROM entity WHERE project_id=? "
			  "AND (module_path=? OR module_path LIKE ?) "
			  "AND kind IN (0,1,2) "
			  "ORDER BY kind, id LIMIT ?";
	sqlite3_stmt *st = nullptr;
	std::vector<EntityRow> rows;
	if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
		return rows;
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(st, 2, mod_path.c_str(), -1, SQLITE_TRANSIENT);
	std::string prefix = mod_path + "/%";
	sqlite3_bind_text(st, 3, prefix.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 4, kMaxModuleEntities + 1);
	while (sqlite3_step(st) == SQLITE_ROW)
		rows.push_back(readEntityRow(st));
	sqlite3_finalize(st);
	return rows;
}

// ── Relation queries ────────────────────────────────────────────

/// Read a relation row from the current statement position.
RelationRow readRelationRow(sqlite3_stmt *st)
{
	RelationRow r;
	r.source_id = sqlite3_column_int64(st, 0);
	r.target_id = sqlite3_column_int64(st, 1);
	r.type = sqlite3_column_int(st, 2);
	r.name = textCol(st, 3);
	r.file_path = textCol(st, 4);
	r.line = sqlite3_column_int64(st, 5);
	return r;
}

/// Query callers: entities that call the target entity.
std::vector<RelationRow> queryCallers(sqlite3 *db, uint64_t project_id,
				      int64_t target_id)
{
	const char *sql = "SELECT r.source_id, r.target_id, r.type, "
			  "       e.name, e.file_path, e.start_row "
			  "FROM relation r JOIN entity e ON e.id = r.source_id "
			  "WHERE r.project_id=? AND r.type IN (1,3) "
			  "AND r.target_id=? "
			  "GROUP BY e.id ORDER BY e.id LIMIT ?";
	sqlite3_stmt *st = nullptr;
	std::vector<RelationRow> rows;
	if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
		return rows;
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(st, 2, target_id);
	sqlite3_bind_int(st, 3, kMaxCallers + 1);
	while (sqlite3_step(st) == SQLITE_ROW)
		rows.push_back(readRelationRow(st));
	sqlite3_finalize(st);
	return rows;
}

/// Query callees: entities called by the source entity.
std::vector<RelationRow> queryCallees(sqlite3 *db, uint64_t project_id,
				      int64_t source_id)
{
	const char *sql = "SELECT r.source_id, r.target_id, r.type, "
			  "       e.name, e.file_path, e.start_row "
			  "FROM relation r JOIN entity e ON e.id = r.target_id "
			  "WHERE r.project_id=? AND r.type IN (1,3) "
			  "AND r.source_id=? "
			  "GROUP BY e.id ORDER BY e.id LIMIT ?";
	sqlite3_stmt *st = nullptr;
	std::vector<RelationRow> rows;
	if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
		return rows;
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(st, 2, source_id);
	sqlite3_bind_int(st, 3, kMaxCallees + 1);
	while (sqlite3_step(st) == SQLITE_ROW)
		rows.push_back(readRelationRow(st));
	sqlite3_finalize(st);
	return rows;
}

// ── Findings query ──────────────────────────────────────────────

/// Query findings related to a path (a file, or a module prefix).
///
/// `finding` has no run, no location and no dimension column: it stores
/// (rule, category, severity, description, finding_key, status). Locations and
/// run attribution exist only through `finding_evidence` → `evidence` →
/// `evidence_location`, so this joins that chain instead of filtering columns
/// that do not exist (a failed prepare there used to look like "no findings").
/// `ok` is false when the statement failed, which the caller reports as
/// unknown rather than as "clean" (§17.5).
std::vector<FindingRow> queryFindings(sqlite3 *db, uint64_t project_id,
				      const std::string &path, bool *ok)
{
	const char *sql =
		"SELECT f.finding_key, f.category, f.severity, f.description, "
		"       MIN(l.file_path), MIN(l.start_row) "
		"FROM finding f "
		"JOIN finding_evidence fe "
		"  ON fe.project_id = f.project_id "
		" AND fe.finding_key = f.finding_key "
		"JOIN evidence e ON e.id = fe.evidence_id "
		"JOIN evidence_location l ON l.evidence_id = e.id "
		"WHERE f.project_id = ? AND l.file_path LIKE ? "
		"  AND f.status NOT IN ('resolved', 'superseded') "
		"GROUP BY f.finding_key "
		"ORDER BY f.severity DESC, f.finding_key LIMIT ?";
	sqlite3_stmt *st = nullptr;
	std::vector<FindingRow> rows;
	if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
		if (ok)
			*ok = false;
		return rows;
	}
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
	std::string pat = "%" + path + "%";
	sqlite3_bind_text(st, 2, pat.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 3, kMaxFindings + 1);
	while (sqlite3_step(st) == SQLITE_ROW) {
		FindingRow r;
		r.finding_key = textCol(st, 0);
		r.dimension = textCol(st, 1);
		r.severity = sqlite3_column_int64(st, 2);
		r.title = textCol(st, 3);
		r.file_path = textCol(st, 4);
		r.line = sqlite3_column_int64(st, 5);
		rows.push_back(std::move(r));
	}
	sqlite3_finalize(st);
	return rows;
}

// ── Test candidates ─────────────────────────────────────────────

/// Query test files that reference the symbol.
std::vector<std::string> queryTestCandidates(sqlite3 *db, uint64_t project_id,
					     const std::string &sym)
{
	const char *sql = "SELECT DISTINCT e.file_path FROM entity e "
			  "WHERE e.project_id=? "
			  "AND (e.file_path LIKE '%test%' "
			  "     OR e.file_path LIKE '%Test%') "
			  "AND (e.name LIKE ? OR e.qualified_name LIKE ?) "
			  "ORDER BY e.file_path LIMIT ?";
	sqlite3_stmt *st = nullptr;
	std::vector<std::string> rows;
	if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
		return rows;
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
	std::string pat = "%" + sym + "%";
	sqlite3_bind_text(st, 2, pat.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, pat.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 4, kMaxTestCandidates);
	while (sqlite3_step(st) == SQLITE_ROW)
		rows.push_back(textCol(st, 0));
	sqlite3_finalize(st);
	return rows;
}

// ── Run info ────────────────────────────────────────────────────

struct RunInfo {
	std::string run_id;
	std::string status;
};

/// The most recent run for the project, whatever its status.
///
/// The primary key of `verification_run` is `run_id` (there is no `id`
/// column); selecting `id` silently failed to prepare and made every caller
/// report "no run found" next to a completed run. `run_id` is the tie-breaker
/// so two runs started in the same second resolve deterministically (R4).
RunInfo queryLatestRun(sqlite3 *db, uint64_t project_id)
{
	RunInfo info;
	const char *sql = "SELECT run_id, status FROM verification_run "
			  "WHERE project_id=? "
			  "ORDER BY started_at DESC, run_id DESC LIMIT 1";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(st) == SQLITE_ROW) {
			info.run_id = textCol(st, 0);
			info.status = textCol(st, 1);
		}
		sqlite3_finalize(st);
	}
	return info;
}

// ── Section writers ─────────────────────────────────────────────

void writeFreshness(util::JsonWriter &w, sqlite3 *db, uint64_t project_id)
{
	RunInfo run = queryLatestRun(db, project_id);
	w.key("freshness").beginObject();
	if (run.run_id.empty()) {
		w.key("status").value("unknown");
		w.key("source").value("fact");
		w.key("reason").value("no verification run found");
	} else {
		w.key("run_id").value(run.run_id);
		w.key("status").value(run.status);
		w.key("source").value("fact");
	}
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

/// Write a capped relation list. The query fetches one row past `max_count`,
/// so `has_more` reflects truncation instead of always saying false
/// (§18.7.0: a truncated list must advertise itself).
void writeRelationList(util::JsonWriter &w, const char *label,
		       const std::vector<RelationRow> &rows, int max_count)
{
	const int returned =
		std::min(static_cast<int>(rows.size()), max_count);
	w.key(label).beginObject();
	w.key("items").beginArray();
	for (int i = 0; i < returned; ++i) {
		const auto &r = rows[i];
		w.beginObject();
		w.key("entity_name").value(r.name);
		w.key("file_path").value(r.file_path);
		w.key("line").value(r.line);
		w.key("edge_type").value(r.type);
		w.key("source").value("fact");
		w.endObject();
	}
	w.endArray();
	w.key("total").value(static_cast<int64_t>(rows.size()));
	w.key("returned").value(returned);
	w.key("has_more").value(static_cast<int>(rows.size()) > max_count);
	w.key("source").value("fact");
	w.endObject();
}

/// Write the findings section. `ok == false` means the query did not run:
/// reporting an empty list there would read as "no findings" (§17.5).
void writeFindings(util::JsonWriter &w, const std::vector<FindingRow> &rows,
		   bool ok)
{
	w.key("findings").beginObject();
	if (!ok) {
		w.key("status").value("unknown");
		w.key("reason").value("findings query failed");
		w.key("source").value("unknown");
		w.endObject();
		return;
	}
	w.key("items").beginArray();
	const int returned =
		std::min(static_cast<int>(rows.size()), kMaxFindings);
	for (int i = 0; i < returned; ++i) {
		const auto &f = rows[i];
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
	w.key("total").value(static_cast<int64_t>(rows.size()));
	w.key("returned").value(returned);
	w.key("has_more").value(static_cast<int>(rows.size()) > kMaxFindings);
	w.key("source").value("fact");
	w.endObject();
}

void writeTestCandidates(util::JsonWriter &w,
			 const std::vector<std::string> &tests)
{
	w.key("test_candidates").beginObject();
	w.key("items").beginArray();
	for (const auto &t : tests)
		w.value(t);
	w.endArray();
	w.key("total").value(static_cast<int64_t>(tests.size()));
	w.key("source").value("inferred");
	w.key("note").value("test files referencing this symbol; "
			    "not executed");
	w.endObject();
}

/// Write the unknowns section. `failed` names the sections whose query did
/// not run; a failed query must be visible as unknown, not as an empty result
/// (§17.5).
void writeUnknowns(util::JsonWriter &w, bool has_run,
		   const std::vector<std::string> &failed)
{
	w.key("unknowns").beginArray();
	if (!has_run)
		w.value("verification_run: no run found, "
			"freshness and findings may be stale");
	for (const auto &name : failed)
		w.value("query failed: " + name);
	w.value("test_candidates: inferred from naming, "
		"not from execution");
	w.value("runtime_behavior: static analysis only, "
		"no dynamic evidence");
	w.endArray();
}

void writeNextQueries(util::JsonWriter &w, const std::string &view)
{
	w.key("next_queries").beginArray();
	if (view == "symbol_context") {
		w.value("analyze_project(view=\"module_context\", "
			"scope={module: \"<module_path>\"})");
		w.value("verify_claim(claim={type: "
			"\"capability_exists\", subject: \"<symbol>\"})");
	} else {
		w.value("analyze_project(view=\"symbol_context\", "
			"scope={symbol: \"<entity_name>\"})");
		w.value("get_project_briefing()");
	}
	w.endArray();
}

// ── View: symbol_context ────────────────────────────────────────

/// Build the symbol_context view JSON.
/// Returns the assembled JSON string.
char *buildSymbolContext(EngineContext *ctx, uint64_t project_id,
			 const Scope &scope)
{
	auto db = ctx->store->handle();
	util::JsonWriter w;
	w.beginObject();
	w.key("view").value("symbol_context");
	w.key("project_id").value(project_id);

	// Resolve entity.
	auto entities = resolveEntities(db, project_id, scope.symbol,
					scope.file_filter);
	if (entities.empty()) {
		w.key("error").value("symbol not found");
		w.key("symbol").value(scope.symbol);
		w.key("source").value("fact");
		writeUnknowns(w, false, {});
		w.endObject();
		return dupString(w.str());
	}

	// Ambiguity: multiple entities with same name.
	if (entities.size() > 1) {
		w.key("ambiguous").value(true);
		w.key("symbol").value(scope.symbol);
		w.key("candidates").beginArray();
		for (const auto &e : entities) {
			w.beginObject();
			w.key("name").value(e.name);
			w.key("file_path").value(e.file_path);
			w.key("line").value(e.start_row);
			w.endObject();
		}
		w.endArray();
		w.key("total").value(static_cast<int64_t>(entities.size()));
		w.key("source").value("fact");
		w.key("note").value("multiple entities match; "
				    "narrow with file_filter");
		w.endObject();
		return dupString(w.str());
	}

	const auto &e = entities[0];
	writeDefinition(w, e);

	// Callers and callees.
	auto callers = queryCallers(db, project_id, e.id);
	writeRelationList(w, "callers", callers, kMaxCallers);
	auto callees = queryCallees(db, project_id, e.id);
	writeRelationList(w, "callees", callees, kMaxCallees);

	// Findings that point at this file.
	bool findings_ok = true;
	auto findings = queryFindings(db, project_id, e.file_path,
				      &findings_ok);
	writeFindings(w, findings, findings_ok);

	// Test candidates.
	auto tests = queryTestCandidates(db, project_id, scope.symbol);
	writeTestCandidates(w, tests);

	// Freshness + unknowns + next_queries.
	writeFreshness(w, db, project_id);
	RunInfo run = queryLatestRun(db, project_id);
	std::vector<std::string> failed;
	if (!findings_ok)
		failed.push_back("findings");
	writeUnknowns(w, !run.run_id.empty(), failed);
	writeNextQueries(w, "symbol_context");

	w.endObject();
	if (!w.ok())
		return dupString(util::errorEnvelope(
			"ffi", "engine_analyze_project", w.error()));
	return dupString(w.str());
}

// ── View: module_context ────────────────────────────────────────

/// Build the module_context view JSON.
char *buildModuleContext(EngineContext *ctx, uint64_t project_id,
			 const Scope &scope)
{
	auto db = ctx->store->handle();
	util::JsonWriter w;
	w.beginObject();
	w.key("view").value("module_context");
	w.key("project_id").value(project_id);
	w.key("module").value(scope.module);
	w.key("source").value("fact");

	// Entities in the module. The query fetches one row past the display
	// cap so `has_more` is truthful.
	auto entities = resolveModuleEntities(db, project_id, scope.module);
	const int returned =
		std::min(static_cast<int>(entities.size()), kMaxModuleEntities);
	w.key("entities").beginObject();
	w.key("items").beginArray();
	for (int i = 0; i < returned; ++i) {
		const auto &e = entities[i];
		w.beginObject();
		w.key("name").value(e.name);
		w.key("kind").value(e.kind);
		w.key("file_path").value(e.file_path);
		w.key("line").value(e.start_row);
		w.key("language").value(e.language);
		w.endObject();
	}
	w.endArray();
	w.key("total").value(static_cast<int64_t>(entities.size()));
	w.key("returned").value(returned);
	w.key("has_more")
		.value(static_cast<int>(entities.size()) > kMaxModuleEntities);
	w.key("source").value("fact");
	w.endObject();

	// Findings in the module (attributed through evidence_location).
	bool findings_ok = true;
	auto findings = queryFindings(db, project_id, scope.module,
				      &findings_ok);
	writeFindings(w, findings, findings_ok);

	// Freshness + unknowns + next_queries.
	writeFreshness(w, db, project_id);
	RunInfo run = queryLatestRun(db, project_id);
	std::vector<std::string> failed;
	if (!findings_ok)
		failed.push_back("findings");
	writeUnknowns(w, !run.run_id.empty(), failed);
	writeNextQueries(w, "module_context");

	w.endObject();
	if (!w.ok())
		return dupString(util::errorEnvelope(
			"ffi", "engine_analyze_project", w.error()));
	return dupString(w.str());
}

} // namespace

// ─── FFI entry point ────────────────────────────────────────────

/// Analyze a project with a given view and scope (M1 task context).
/// See engine.h for the full contract.
// Ownership: the caller frees the returned JSON string with
// engine_free_string().
// Lifetime: input strings are borrowed for the call only.
// Thread safety: shares one store connection; not concurrent with indexing.
char *engine_analyze_project(engine_t handle, uint64_t project_id,
			     const char *view, const char *scope_json,
			     const char *run_id)
{
	EngineContext *ctx = engineInstance(handle);

	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!ctx || !ctx->store)
			return dupString(util::errorEnvelope(
				"ffi", "engine_analyze_project",
				"engine not initialized"));

		std::string view_str = view ? view : "";
		Scope scope = parseScope(scope_json);

		// run_id is accepted but not yet used in M1;
		// it will select the run for change_context in M2.
		(void)run_id;

		if (view_str == "symbol_context") {
			if (scope.symbol.empty())
				return dupString(util::errorEnvelope(
					"ffi", "engine_analyze_project",
					"scope.symbol is required for "
					"symbol_context view"));
			return buildSymbolContext(ctx, project_id, scope);
		} else if (view_str == "module_context") {
			if (scope.module.empty())
				return dupString(util::errorEnvelope(
					"ffi", "engine_analyze_project",
					"scope.module is required for "
					"module_context view"));
			return buildModuleContext(ctx, project_id, scope);
		} else {
			return dupString(util::errorEnvelope(
				"ffi", "engine_analyze_project",
				"unsupported view; use symbol_context "
				"or module_context"));
		}
	} catch (const std::exception &e) {
		return dupString(util::errorEnvelope(
			"ffi", "engine_analyze_project", e.what()));
	} catch (...) {
		return dupString(util::errorEnvelope(
			"ffi", "engine_analyze_project", "unknown exception"));
	}
}

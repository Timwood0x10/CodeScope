// engine_briefing_ffi.cpp — Project Briefing FFI export (M1 navigation).
//
// Exposes a single read-only entry point:
//
//   char *engine_get_project_briefing(engine_t handle, uint64_t project_id);
//
// The briefing is the M1 "navigation card" from development_plan §18.7.1.
// It aggregates existing structural facts, the latest verification run, and
// document metadata into a bounded JSON payload (target ≤ 4 KiB).
//
// Design constraints (plan/rules/code_rules.md §16 + development_plan §18.7.0):
//   - Read-only: no indexing, no rule execution, no target-project scripts.
//   - Every fact carries a "source" field: fact | declared | documented | inferred.
//   - Unknowns are explicit: a missing run, empty tables, or unprovable aspects
//     are reported as "unknown" or "unverified", never as a confident "clean".
//   - Deterministic: no LLM, no wall-clock-dependent ordering.
//   - Budget: top-k lists are sorted by a stable key and truncated; the
//     response carries total/returned/has_more for each truncated list.

#include "engine_internal.h"
#include "async_knowledge.h"
#include "util/json_writer.h"

#include <sqlite3.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{

// ── Briefing field counts (§18.7.0 budget) ──────────────────────
constexpr int kMaxLanguages = 8;
constexpr int kMaxModules = 12;
constexpr int kMaxEntryPoints = 8;
constexpr int kMaxFindings = 10;
constexpr int kMaxNextQueries = 6;

// ── Row types ───────────────────────────────────────────────────

struct LanguageRow {
	std::string language;
	int64_t count;
};
struct ModuleRow {
	std::string path;
	int64_t entity_count;
};
struct EntryPointRow {
	std::string name;
	std::string file;
	int64_t line;
};
struct FindingSummary {
	std::string finding_key;
	std::string dimension;
	int64_t severity;
	std::string title;
};

/// A scalar aggregate plus its success flag.
///
/// The flag is not decoration: `queryCount()` returning a bare 0 on a failed
/// prepare would be reported as "0 files" with `source: "fact"`, which is the
/// "zero hits vs never ran" confusion the plan forbids (invariant 1, §17.5).
/// Callers pass the failures into `unknowns` so the response says it does not
/// know instead of claiming emptiness.
struct CountResult {
	int64_t value = 0;
	bool ok = true;
};

/// A capped list plus the fact that the query ran at all.
template <typename T> struct Rows {
	std::vector<T> items;
	bool ok = true;
};
struct LatestRun {
	std::string run_id;
	std::string status;
	std::string profile;
	std::string started_at;
};

// ── Query helpers ───────────────────────────────────────────────

/// Query a single aggregate integer. `ok` is false when the statement could
/// not be prepared or did not return a row, so the caller reports "unknown"
/// rather than a fabricated 0.
CountResult queryCount(sqlite3 *db, const char *sql, uint64_t project_id)
{
	CountResult out;
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		out.ok = false;
		return out;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	if (sqlite3_step(stmt) == SQLITE_ROW)
		out.value = sqlite3_column_int64(stmt, 0);
	else
		out.ok = false;
	sqlite3_finalize(stmt);
	return out;
}

/// Text column of the current row, never null.
const char *textColumn(sqlite3_stmt *stmt, int col)
{
	const char *text =
		reinterpret_cast<const char *>(sqlite3_column_text(stmt, col));
	return text ? text : "";
}

/// Languages by symbol count. Fetches one row past the display cap so the
/// caller can report `has_more` instead of silently truncating.
Rows<LanguageRow> queryLanguages(sqlite3 *db, uint64_t project_id)
{
	const char *sql = "SELECT language, COUNT(*) AS cnt "
			  "FROM entity WHERE project_id = ? AND kind IN (0,1) "
			  "GROUP BY language ORDER BY cnt DESC, language LIMIT ?";
	Rows<LanguageRow> out;
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		out.ok = false;
		return out;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 2, kMaxLanguages + 1);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		LanguageRow r;
		r.language = textColumn(stmt, 0);
		r.count = sqlite3_column_int64(stmt, 1);
		out.items.push_back(std::move(r));
	}
	sqlite3_finalize(stmt);
	return out;
}

/// Physical modules, derived from the parsed `entity.module_path` (not from
/// `scope`: that table has no path column and the entity table has no
/// `scope_id`, so a scope-based join would silently return nothing).
Rows<ModuleRow> queryModules(sqlite3 *db, uint64_t project_id)
{
	const char *sql = "SELECT module_path, COUNT(*) AS cnt "
			  "FROM entity "
			  "WHERE project_id = ? AND kind IN (0,1) "
			  "  AND module_path <> '' "
			  "GROUP BY module_path "
			  "ORDER BY cnt DESC, module_path LIMIT ?";
	Rows<ModuleRow> out;
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		out.ok = false;
		return out;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 2, kMaxModules + 1);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		ModuleRow r;
		r.path = textColumn(stmt, 0);
		r.entity_count = sqlite3_column_int64(stmt, 1);
		out.items.push_back(std::move(r));
	}
	sqlite3_finalize(stmt);
	return out;
}

/// Entry-point *candidates*. The name whitelist is the same heuristic the
/// existing entry-point query uses; it can only produce candidates, never a
/// complete entry set (development_plan §9.4). Reported with `source:
/// inferred` so the caller cannot read it as a declared entry point.
Rows<EntryPointRow> queryEntryPoints(sqlite3 *db, uint64_t project_id)
{
	const char *sql =
		"SELECT name, file_path, start_row "
		"FROM entity "
		"WHERE project_id = ? AND kind IN (0,1) "
		"AND (name = 'main' OR name = 'init' OR name = 'setup' "
		"  OR name = 'run' OR name LIKE '%handler%') "
		"ORDER BY name, file_path, start_row LIMIT ?";
	Rows<EntryPointRow> out;
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		out.ok = false;
		return out;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 2, kMaxEntryPoints + 1);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		EntryPointRow r;
		r.name = textColumn(stmt, 0);
		r.file = textColumn(stmt, 1);
		r.line = sqlite3_column_int64(stmt, 2);
		out.items.push_back(std::move(r));
	}
	sqlite3_finalize(stmt);
	return out;
}

/// Most recent *completed* run (development_plan §18.6.2 point 3: the
/// briefing reads the last completed run; a partial run is only visible
/// through an explicit query). `run_id` breaks the tie so two runs started
/// in the same second do not make the briefing non-deterministic (R4).
LatestRun queryLatestRun(sqlite3 *db, uint64_t project_id)
{
	const char *sql = "SELECT run_id, status, profile, started_at "
			  "FROM verification_run "
			  "WHERE project_id = ? AND status = 'completed' "
			  "ORDER BY started_at DESC, run_id DESC LIMIT 1";
	sqlite3_stmt *stmt = nullptr;
	LatestRun run;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			run.run_id = textColumn(stmt, 0);
			run.status = textColumn(stmt, 1);
			run.profile = textColumn(stmt, 2);
			run.started_at = textColumn(stmt, 3);
		}
		sqlite3_finalize(stmt);
	}
	return run;
}

/// Findings produced by `run_id`.
///
/// `finding` carries neither a run nor a location: its columns are
/// (rule, category, severity, description, finding_key, status, ...). A finding
/// belongs to a run only through `finding_evidence` → `evidence.run_id`, which
/// is why this joins instead of filtering `finding.run_id`.
Rows<FindingSummary> queryFindings(sqlite3 *db, uint64_t project_id,
				   const std::string &run_id)
{
	Rows<FindingSummary> out;
	if (run_id.empty())
		return out; // no run: nothing to attribute, not a failure
	const char *sql = "SELECT f.finding_key, f.category, f.severity, "
			  "       f.description "
			  "FROM finding f "
			  "JOIN finding_evidence fe "
			  "  ON fe.project_id = f.project_id "
			  " AND fe.finding_key = f.finding_key "
			  "JOIN evidence e ON e.id = fe.evidence_id "
			  "WHERE f.project_id = ? AND e.run_id = ? "
			  "  AND f.status NOT IN ('resolved', 'superseded') "
			  "GROUP BY f.finding_key "
			  "ORDER BY f.severity DESC, f.finding_key LIMIT ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		out.ok = false;
		return out;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, run_id.c_str(),
			  static_cast<int>(run_id.size()), SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 3, kMaxFindings + 1);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		FindingSummary f;
		f.finding_key = textColumn(stmt, 0);
		f.dimension = textColumn(stmt, 1);
		f.severity = sqlite3_column_int64(stmt, 2);
		f.title = textColumn(stmt, 3);
		out.items.push_back(std::move(f));
	}
	sqlite3_finalize(stmt);
	return out;
}

CountResult queryEvidenceCount(sqlite3 *db, const std::string &run_id)
{
	CountResult out;
	if (run_id.empty())
		return out;
	const char *sql = "SELECT COUNT(*) FROM evidence WHERE run_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		out.ok = false;
		return out;
	}
	sqlite3_bind_text(stmt, 1, run_id.c_str(),
			  static_cast<int>(run_id.size()), SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		out.value = sqlite3_column_int64(stmt, 0);
	else
		out.ok = false;
	sqlite3_finalize(stmt);
	return out;
}

// ── JSON section writers ────────────────────────────────────────

/// Write the freshness section (run metadata + source tag).
/// Status is "unknown" when no run exists, "stale" when a run exists
/// but the worktree change is not verified (§18.6.2 point 4).
/// Never "current" without an input digest check (M2).
void writeFreshness(util::JsonWriter &w, const LatestRun &run)
{
	w.key("freshness");
	w.beginObject();
	if (run.run_id.empty()) {
		w.key("status").value("unknown");
		w.key("reason").value("no completed verification run");
	} else {
		w.key("status").value("stale");
		w.key("run_id").value(run.run_id);
		w.key("profile").value(run.profile);
		w.key("started_at").value(run.started_at);
		w.key("reason").value("worktree change not verified; "
				      "run analysis to confirm");
	}
	w.key("source").value("fact");
	w.endObject();
}

/// Write the project scale section.
///
/// `files` and `symbols` are row counts; `function_lines` is the sum of the
/// per-function line counts measured at parse time. There is deliberately no
/// `loc`: `files.loc` does not exist yet (D1), and summing entity row spans
/// would double-count nested symbols while missing non-symbol lines — a
/// number that looks measured but is not. A failed count is emitted as `null`
/// with `source: "unknown"` instead of a fabricated 0 (§17.5).
void writeScale(util::JsonWriter &w, const CountResult &files,
		const CountResult &symbols, const CountResult &function_lines)
{
	w.key("scale");
	w.beginObject();
	if (files.ok)
		w.key("files").value(files.value);
	else
		w.key("files").nullValue();
	if (symbols.ok)
		w.key("symbols").value(symbols.value);
	else
		w.key("symbols").nullValue();
	if (function_lines.ok)
		w.key("function_lines").value(function_lines.value);
	else
		w.key("function_lines").nullValue();
	const bool all_ok = files.ok && symbols.ok && function_lines.ok;
	w.key("source").value(all_ok ? "fact" : "unknown");
	w.endObject();
}

/// Write a capped list with total/returned/has_more metadata.
/// Each item is written by `write_item`. The caller must have already
/// emitted the section key and opened the outer object.
template <typename T, typename WriteItem>
void writeCappedSection(util::JsonWriter &w, const std::vector<T> &items,
			int max_count, const char *source, WriteItem write_item)
{
	w.key("items");
	w.beginArray();
	const int returned =
		std::min(static_cast<int>(items.size()), max_count);
	for (int i = 0; i < returned; ++i)
		write_item(w, items[i]);
	w.endArray();
	w.key("total").value(static_cast<int>(items.size()));
	w.key("returned").value(returned);
	w.key("has_more").value(static_cast<int>(items.size()) > max_count);
	w.key("source").value(source);
}

/// Write the languages section (capped at kMaxLanguages).
void writeLanguages(util::JsonWriter &w, const std::vector<LanguageRow> &langs)
{
	w.key("languages");
	w.beginObject();
	writeCappedSection(w, langs, kMaxLanguages, "fact",
			   [](util::JsonWriter &jw, const LanguageRow &r) {
				   jw.beginObject();
				   jw.key("name").value(r.language);
				   jw.key("symbols").value(r.count);
				   jw.endObject();
			   });
	w.endObject();
}

/// Last path component of a parsed module path ("engine/src/store" →
/// "store"). The parser records the directory; the briefing shows both.
std::string moduleName(const std::string &path)
{
	const std::size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// Write the modules section (capped at kMaxModules).
/// Module role is "inferred" — not declared (§18.7.0).
void writeModules(util::JsonWriter &w, const std::vector<ModuleRow> &mods)
{
	w.key("modules");
	w.beginObject();
	writeCappedSection(w, mods, kMaxModules, "fact",
			   [](util::JsonWriter &jw, const ModuleRow &r) {
				   jw.beginObject();
				   jw.key("name").value(moduleName(r.path));
				   jw.key("path").value(r.path);
				   jw.key("symbols").value(r.entity_count);
				   jw.key("role").value("inferred");
				   jw.endObject();
			   });
	w.endObject();
}

/// Write the entry points section (capped at kMaxEntryPoints).
void writeEntryPoints(util::JsonWriter &w,
		      const std::vector<EntryPointRow> &eps)
{
	w.key("entry_points");
	w.beginObject();
	writeCappedSection(w, eps, kMaxEntryPoints, "inferred",
			   [](util::JsonWriter &jw, const EntryPointRow &r) {
				   jw.beginObject();
				   jw.key("name").value(r.name);
				   jw.key("file").value(r.file);
				   jw.key("line").value(r.line);
				   jw.key("source").value("inferred");
				   jw.endObject();
			   });
	w.endObject();
}

/// Write the verification section (run state + findings summary).
/// `evidence` and `findings` are already query results with their success
/// flags: a failed count is written as `null`, never as 0 (§17.5).
void writeVerification(util::JsonWriter &w, const LatestRun &run,
		       const CountResult &evidence,
		       const Rows<FindingSummary> &findings)
{
	w.key("verification");
	w.beginObject();
	if (run.run_id.empty()) {
		w.key("status").value("unverified");
		w.key("reason").value("no completed run");
	} else {
		w.key("run_id").value(run.run_id);
		w.key("status").value(run.status);
		if (evidence.ok)
			w.key("evidence_count").value(evidence.value);
		else
			w.key("evidence_count").nullValue();
		if (findings.ok)
			w.key("finding_count")
				.value(static_cast<int64_t>(findings.items.size()));
		else
			w.key("finding_count").nullValue();
		w.key("findings");
		w.beginObject();
		writeCappedSection(
			w, findings.items, kMaxFindings, "fact",
			[](util::JsonWriter &jw, const FindingSummary &f) {
				jw.beginObject();
				jw.key("key").value(f.finding_key);
				jw.key("dimension").value(f.dimension);
				jw.key("severity").value(f.severity);
				jw.key("title").value(f.title);
				jw.endObject();
			});
		w.endObject();
	}
	w.key("source").value("fact");
	w.endObject();
}

/// Write the unknowns section — explicit list of unverified aspects
/// per §18.7.0. `failed` names the sections whose query did not run: an
/// empty list there means "nothing found", a failed query means "unknown",
/// and the two must not look the same.
void writeUnknowns(util::JsonWriter &w, const LatestRun &run,
		   const std::vector<LanguageRow> &langs,
		   const std::vector<ModuleRow> &mods,
		   const std::vector<EntryPointRow> &eps,
		   const std::vector<std::string> &failed)
{
	w.key("unknowns");
	w.beginArray();
	if (run.run_id.empty())
		w.value("verification: no completed run");
	for (const auto &name : failed)
		w.value("query failed: " + name);
	if (langs.empty())
		w.value("languages: no indexed entities");
	if (mods.empty())
		w.value("modules: no parsed module_path");
	if (eps.empty())
		w.value("entry_points: name-based candidates only, none matched");
	w.value("loc: total file lines not measured (files.loc is D1)");
	w.value("constraints: not declared (M2)");
	w.value("coverage: not computed (M2)");
	w.value("drift: not detected (M3)");
	w.endArray();
}

/// Write the next_queries section (navigation suggestions).
void writeNextQueries(util::JsonWriter &w)
{
	struct Query {
		const char *view;
		const char *description;
	};
	const Query queries[] = {
		{ "module_context",
		  "Drill into a module's entities, callers and findings" },
		{ "symbol_context",
		  "Get definition, callers and impact for a symbol" },
		{ "find_definition", "Locate a symbol's definition by name" },
		{ "get_entry_points", "List all entry-point candidates" },
		{ "get_module_tree", "Browse the directory/module tree" },
		{ "search", "Full-text + semantic search for code" },
	};
	static_assert(sizeof(queries) / sizeof(queries[0]) <= kMaxNextQueries);

	w.key("next_queries");
	w.beginObject();
	w.key("items");
	w.beginArray();
	for (const auto &q : queries) {
		w.beginObject();
		w.key("view").value(q.view);
		w.key("description").value(q.description);
		w.endObject();
	}
	w.endArray();
	w.key("total").value(kMaxNextQueries);
	w.key("returned").value(kMaxNextQueries);
	w.key("has_more").value(false);
	w.endObject();
}

// ── Briefing assembler ──────────────────────────────────────────

/// Gather all facts from the database and assemble the briefing JSON.
/// Each section is written by a dedicated function; this function only
/// orchestrates the query + section order.
char *buildBriefing(EngineContext *ctx, uint64_t project_id)
{
	auto _store_guard = waitForKnowledgeBuilder();
	if (!ctx || !ctx->store)
		return dupString(util::errorEnvelope(
			"ffi", "engine_get_project_briefing",
			"engine not initialized"));

	auto db = ctx->store->handle();

	// Gather facts. Every result carries `ok`; the failures end up in
	// `unknowns` instead of turning into a confident empty answer.
	const auto languages = queryLanguages(db, project_id);
	const auto modules = queryModules(db, project_id);
	const auto entry_points = queryEntryPoints(db, project_id);
	const auto latest_run = queryLatestRun(db, project_id);
	const auto findings = queryFindings(db, project_id, latest_run.run_id);
	const auto evidence_count = queryEvidenceCount(db, latest_run.run_id);
	const auto file_count = queryCount(
		db, "SELECT COUNT(*) FROM files WHERE project_id = ?",
		project_id);
	// Measured per-function line counts. Not file LOC: that needs the
	// `files.loc` columns D1 has not added yet.
	const auto function_lines = queryCount(
		db,
		"SELECT COALESCE(SUM(lines), 0) FROM entity "
		"WHERE project_id = ? AND kind IN (0,1)",
		project_id);
	const auto total_symbols = queryCount(
		db,
		"SELECT COUNT(*) FROM entity WHERE project_id = ? "
		"AND kind IN (0,1)",
		project_id);

	std::vector<std::string> failed;
	if (!languages.ok)
		failed.push_back("languages");
	if (!modules.ok)
		failed.push_back("modules");
	if (!entry_points.ok)
		failed.push_back("entry_points");
	if (!findings.ok)
		failed.push_back("findings");
	if (!evidence_count.ok)
		failed.push_back("evidence_count");
	if (!file_count.ok || !total_symbols.ok || !function_lines.ok)
		failed.push_back("scale");

	// Serialize sections.
	util::JsonWriter w;
	w.beginObject();
	w.key("ok").value(true);
	w.key("project_id").value(project_id);
	w.key("scoring").value("none");
	writeFreshness(w, latest_run);
	writeScale(w, file_count, total_symbols, function_lines);
	writeLanguages(w, languages.items);
	writeModules(w, modules.items);
	writeEntryPoints(w, entry_points.items);
	writeVerification(w, latest_run, evidence_count, findings);
	writeUnknowns(w, latest_run, languages.items, modules.items,
		      entry_points.items, failed);
	writeNextQueries(w);
	w.endObject();

	if (!w.ok())
		return dupString(util::errorEnvelope(
			"ffi", "engine_get_project_briefing", w.error()));
	return dupString(w.str());
}

} // namespace

// ─── FFI entry point ────────────────────────────────────────────

/// Get a read-only project briefing (M1 navigation card).
/// See engine.h for the full contract.
// Ownership: the caller frees the returned JSON string with
// engine_free_string().
// Lifetime: no input strings; reads existing DB state only.
// Thread safety: shares one store connection; not concurrent with indexing.
char *engine_get_project_briefing(engine_t handle, uint64_t project_id)
{
	EngineContext *ctx = engineInstance(handle);

	try {
		return buildBriefing(ctx, project_id);
	} catch (const std::exception &e) {
		return dupString(util::errorEnvelope(
			"ffi", "engine_get_project_briefing", e.what()));
	} catch (...) {
		return dupString(util::errorEnvelope(
			"ffi", "engine_get_project_briefing",
			"unknown exception"));
	}
}

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

#include "engine_briefing_sections.h"
#include "engine_internal.h"
#include "async_knowledge.h"
#include "util/json_writer.h"

#include <sqlite3.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{

// The three structural sections live in engine_briefing_sections.cpp; these
// names are used throughout the card, so keep the spellings local.
using engine_briefing::EntryPointRow;
using engine_briefing::LanguageRow;
using engine_briefing::ModuleRow;
using engine_briefing::kMaxEntryPoints;
using engine_briefing::kMaxLanguages;
using engine_briefing::kMaxModules;

// ── Briefing field counts (§18.7.0 budget) ──────────────────────
// The per-list display caps of the three structural sections live next to the
// writers that use them (engine_briefing_sections.h).
constexpr int kMaxFindings = 10;
constexpr int kMaxNextQueries = 6;

// ── Row types ───────────────────────────────────────────────────

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

/// Languages by symbol count. Fetched whole: the table holds a handful of
/// rows, and a real total lets `has_more` mean truncation instead of "the page
/// was full".
Rows<LanguageRow> queryLanguages(sqlite3 *db, uint64_t project_id)
{
	const char *sql = "SELECT language, COUNT(*) AS cnt "
			  "FROM entity WHERE project_id = ? AND kind IN (0,1) "
			  "GROUP BY language ORDER BY cnt DESC, language";
	Rows<LanguageRow> out;
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		out.ok = false;
		return out;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
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
			  "ORDER BY cnt DESC, module_path";
	Rows<ModuleRow> out;
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		out.ok = false;
		return out;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
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
		"ORDER BY name, file_path, start_row";
	Rows<EntryPointRow> out;
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		out.ok = false;
		return out;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
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
/// Membership comes from that run's `evidence` rows, not from
/// `finding_evidence`: the association table is rewritten every time a finding
/// is upserted, so a later partial re-run silently detaches the old run's
/// findings and changes a finished run's briefing (development_plan §6.1: a
/// finding keeps current state, evidence instances are history).
///
/// The display fields (category/severity/description) are read from the current
/// `finding` row, which is what "current state" means; a finding whose row was
/// deleted keeps its key with empty fields.
Rows<FindingSummary> queryFindings(sqlite3 *db, uint64_t project_id,
				   const std::string &run_id)
{
	Rows<FindingSummary> out;
	if (run_id.empty())
		return out; // no run: nothing to attribute, not a failure
	const char *sql =
		"SELECT e.finding_key, COALESCE(f.category, ''), "
		"       COALESCE(f.severity, -1), COALESCE(f.description, '') "
		"FROM evidence e "
		"LEFT JOIN finding f "
		"  ON f.project_id = ? AND f.finding_key = e.finding_key "
		"WHERE e.run_id = ? "
		"  AND COALESCE(f.status, '') NOT IN ('resolved', 'superseded') "
		"GROUP BY e.finding_key "
		"ORDER BY COALESCE(f.severity, -1) DESC, e.finding_key LIMIT ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		out.ok = false;
		return out;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, run_id.c_str(),
			  static_cast<int>(run_id.size()), SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 3, kMaxFindings + 1);
	int rc = SQLITE_ROW;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		FindingSummary f;
		f.finding_key = textColumn(stmt, 0);
		f.dimension = textColumn(stmt, 1);
		f.severity = sqlite3_column_int64(stmt, 2);
		f.title = textColumn(stmt, 3);
		out.items.push_back(std::move(f));
	}
	if (rc != SQLITE_DONE)
		out.ok = false;
	sqlite3_finalize(stmt);
	return out;
}

/// Exact number of distinct findings the run produced. The list above is
/// capped, so its length is a page size, not a total (§18.7.0).
CountResult queryFindingTotal(sqlite3 *db, uint64_t project_id,
			      const std::string &run_id)
{
	CountResult out;
	if (run_id.empty())
		return out;
	// `evidence` has no project_id column; a run id already belongs to exactly
	// one project, and the caller passed that run. The status filter matches the
	// list query, so the count and the page always describe the same set.
	const char *sql =
		"SELECT COUNT(DISTINCT e.finding_key) FROM evidence e "
		"LEFT JOIN finding f "
		"  ON f.project_id = ? AND f.finding_key = e.finding_key "
		"WHERE e.run_id = ? "
		"  AND COALESCE(f.status, '') NOT IN ('resolved', 'superseded')";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		out.ok = false;
		return out;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, run_id.c_str(),
			  static_cast<int>(run_id.size()), SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		out.value = sqlite3_column_int64(stmt, 0);
	else
		out.ok = false;
	sqlite3_finalize(stmt);
	return out;
}

/// Measured LOC of the project: total lines across files that carry a
/// measurement, and how many files do.
///
/// `lines.value == -1` means nothing was measured (every `files.loc` is NULL,
/// i.e. the rows predate the column and were never re-indexed). Reporting that
/// as 0 would claim an empty project, which is the confusion §17.5 forbids.
struct LocTotals {
	CountResult lines;
	CountResult files;
};

LocTotals queryLocTotals(sqlite3 *db, uint64_t project_id)
{
	LocTotals out;
	const char *sql = "SELECT COALESCE(SUM(loc), -1), COUNT(loc) "
			  "FROM files WHERE project_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		out.lines.ok = false;
		out.files.ok = false;
		return out;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		out.lines.value = sqlite3_column_int64(stmt, 0);
		out.files.value = sqlite3_column_int64(stmt, 1);
	} else {
		out.lines.ok = false;
		out.files.ok = false;
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
/// per-function line counts measured at parse time. `loc` is the sum of
/// `files.loc` — a measured file-level line count (D1 §5.1). A failed count
/// is emitted as `null` with `source: "unknown"` instead of a fabricated 0
/// (§17.5).
void writeScale(util::JsonWriter &w, const CountResult &files,
		const CountResult &symbols, const CountResult &function_lines,
		const LocTotals &loc)
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
	if (loc.lines.ok && loc.lines.value >= 0)
		w.key("loc").value(loc.lines.value);
	else
		w.key("loc").nullValue();
	// How many files the sum covers: a card that reports loc without this
	// cannot be checked against `files`.
	if (loc.files.ok)
		w.key("loc_files").value(loc.files.value);
	else
		w.key("loc_files").nullValue();
	const bool all_ok = files.ok && symbols.ok && function_lines.ok &&
			    loc.lines.ok;
	w.key("source").value(all_ok ? "fact" : "unknown");
	w.endObject();
}

/// Write the verification section (run state + findings summary).
///
/// `finding_count` is the counted total, never the page size: reporting the
/// capped length turned 25 findings into "11" (the plan requires an explicit
/// total/returned/has_more split, §18.7.0). A failed count is written as
/// `null`, not 0 (§17.5).
void writeVerification(util::JsonWriter &w, const LatestRun &run,
		       const CountResult &evidence,
		       const CountResult &finding_total,
		       const Rows<FindingSummary> &findings, int max_count)
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
		if (finding_total.ok)
			w.key("finding_count").value(finding_total.value);
		else
			w.key("finding_count").nullValue();
		w.key("findings");
		w.beginObject();
		if (!findings.ok) {
			w.key("status").value("unknown");
			w.key("reason").value("findings query failed");
			w.key("source").value("unknown");
		} else {
			const int returned = std::min(
				static_cast<int>(findings.items.size()),
				max_count);
			w.key("items").beginArray();
			for (int i = 0; i < returned; ++i) {
				const auto &f = findings.items[i];
				w.beginObject();
				w.key("key").value(f.finding_key);
				w.key("dimension").value(f.dimension);
				w.key("severity").value(f.severity);
				w.key("title").value(f.title);
				w.endObject();
			}
			w.endArray();
			w.key("total").value(
				finding_total.ok ?
					finding_total.value :
					static_cast<int64_t>(
						findings.items.size()));
			w.key("returned").value(returned);
			w.key("has_more")
				.value(finding_total.ok &&
				       finding_total.value > returned);
			w.key("source").value("fact");
		}
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
		   const std::vector<engine_briefing::LanguageRow> &langs,
		   const std::vector<engine_briefing::ModuleRow> &mods,
		   const std::vector<engine_briefing::EntryPointRow> &eps,
		   const std::vector<std::string> &failed,
		   const CountResult &files, const LocTotals &loc)
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
	// A file without a measurement is not a file with zero lines.
	if (files.ok && loc.files.ok && loc.files.value < files.value)
		w.value("loc: " +
			std::to_string(files.value - loc.files.value) +
			" file(s) have no measured line count (re-index to "
			"fill)");
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

/// Serialized-size budget of the navigation card (development_plan §11.2,
/// §18.7.1). It is enforced on the finished JSON: a row cap cannot promise a
/// byte count, and a card whose fields are long paths reached 8 KiB while every
/// list was still under its item limit.
///
/// The budget accounts for the MCP JSON-RPC envelope that wraps the FFI
/// payload: `{"jsonrpc":"2.0","id":N,"result":{"content":[{"type":"text",
/// "text":"..."}]}}`. The envelope size varies with the `id` length and
/// any extra result metadata; 128 bytes is a safe upper bound that covers
/// observed real-world messages, so the FFI JSON must be at most
/// `kBriefingBudgetBytes - kMcpEnvelopeOverhead` to keep the final wire
/// message ≤ 4 KiB.
constexpr std::size_t kBriefingBudgetBytes = 4096;
constexpr std::size_t kMcpEnvelopeOverhead = 128;

/// Display caps for one budget level. Level 0 is the full card; each further
/// level shrinks the lists. Counts, freshness, unknowns and next_queries are
/// never dropped — they are what makes the card trustworthy.
struct Caps {
	int languages;
	int modules;
	int entry_points;
	int findings;
};

constexpr int kMaxBudgetLevel = 3;

Caps capsForLevel(int level)
{
	switch (level) {
	case 0:
		return { kMaxLanguages, kMaxModules, kMaxEntryPoints,
			 kMaxFindings };
	case 1:
		return { 4, 6, 4, 4 };
	case 2:
		return { 2, 3, 2, 2 };
	default:
		return { 0, 0, 0, 0 };
	}
}

/// Everything the card needs, read once.
///
/// Gathering once and re-rendering per budget level is deliberate: a retry must
/// not observe a database that changed between attempts, or the card would mix
/// two states.
struct BriefingFacts {
	Rows<LanguageRow> languages;
	Rows<ModuleRow> modules;
	Rows<EntryPointRow> entry_points;
	Rows<FindingSummary> findings;
	LatestRun run;
	CountResult evidence_count;
	CountResult finding_total;
	CountResult files;
	CountResult symbols;
	CountResult function_lines;
	LocTotals loc;
	std::vector<std::string> failed;
};

BriefingFacts gatherFacts(sqlite3 *db, uint64_t project_id)
{
	BriefingFacts facts;
	facts.languages = queryLanguages(db, project_id);
	facts.modules = queryModules(db, project_id);
	facts.entry_points = queryEntryPoints(db, project_id);
	facts.run = queryLatestRun(db, project_id);
	facts.findings = queryFindings(db, project_id, facts.run.run_id);
	facts.evidence_count = queryEvidenceCount(db, facts.run.run_id);
	facts.finding_total =
		queryFindingTotal(db, project_id, facts.run.run_id);
	facts.files = queryCount(
		db, "SELECT COUNT(*) FROM files WHERE project_id = ?",
		project_id);
	// Measured file LOC: files.loc is populated at index time.
	facts.loc = queryLocTotals(db, project_id);
	// Measured per-function line counts.
	facts.function_lines =
		queryCount(db,
			   "SELECT COALESCE(SUM(lines), 0) FROM entity "
			   "WHERE project_id = ? AND kind IN (0,1)",
			   project_id);
	facts.symbols =
		queryCount(db,
			   "SELECT COUNT(*) FROM entity WHERE project_id = ? "
			   "AND kind IN (0,1)",
			   project_id);

	if (!facts.languages.ok)
		facts.failed.push_back("languages");
	if (!facts.modules.ok)
		facts.failed.push_back("modules");
	if (!facts.entry_points.ok)
		facts.failed.push_back("entry_points");
	if (!facts.findings.ok)
		facts.failed.push_back("findings");
	if (!facts.finding_total.ok)
		facts.failed.push_back("finding_count");
	if (!facts.evidence_count.ok)
		facts.failed.push_back("evidence_count");
	if (!facts.files.ok || !facts.symbols.ok || !facts.function_lines.ok ||
	    !facts.loc.lines.ok || !facts.loc.files.ok)
		facts.failed.push_back("scale");
	return facts;
}

/// Render the card at one budget level.
/// @param[out] error Set when the writer rejected the section order.
/// @return The JSON text ("" on error).
std::string renderBriefing(uint64_t project_id, int level,
			   const BriefingFacts &facts, std::string &error)
{
	const Caps caps = capsForLevel(level);
	util::JsonWriter w;
	w.beginObject();
	w.key("ok").value(true);
	w.key("project_id").value(project_id);
	w.key("scoring").value("none");
	w.key("truncated").value(level > 0);
	w.key("budget_bytes").value(static_cast<int64_t>(kBriefingBudgetBytes));
	writeFreshness(w, facts.run);
	writeScale(w, facts.files, facts.symbols, facts.function_lines,
		   facts.loc);
	engine_briefing::writeLanguages(w, facts.languages.items,
					caps.languages);
	engine_briefing::writeModules(w, facts.modules.items, caps.modules);
	engine_briefing::writeEntryPoints(w, facts.entry_points.items,
					  caps.entry_points);
	writeVerification(w, facts.run, facts.evidence_count,
			  facts.finding_total, facts.findings, caps.findings);
	writeUnknowns(w, facts.run, facts.languages.items, facts.modules.items,
		      facts.entry_points.items, facts.failed, facts.files,
		      facts.loc);
	writeNextQueries(w);
	w.endObject();
	if (!w.ok()) {
		error = w.error();
		return {};
	}
	return w.str();
}

/// Assemble the briefing within the byte budget.
///
/// Levels are tried in order and the first card that fits wins. If even the
/// smallest card is too large it is returned with `truncated: true` — never cut
/// mid-JSON, because a truncated object is not a smaller answer, it is an
/// unparseable one.
char *buildBriefing(EngineContext *ctx, uint64_t project_id)
{
	auto _store_guard = waitForKnowledgeBuilder();
	if (!ctx || !ctx->store)
		return dupString(util::errorEnvelope(
			"ffi", "engine_get_project_briefing",
			"engine not initialized"));

	const BriefingFacts facts =
		gatherFacts(ctx->store->handle(), project_id);

	std::string json;
	for (int level = 0; level <= kMaxBudgetLevel; ++level) {
		std::string error;
		json = renderBriefing(project_id, level, facts, error);
		if (json.empty())
			return dupString(util::errorEnvelope(
				"ffi", "engine_get_project_briefing", error));
		if (json.size() <= kBriefingBudgetBytes - kMcpEnvelopeOverhead)
			break;
	}
	return dupString(json);
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

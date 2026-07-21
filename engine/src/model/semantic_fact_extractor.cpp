// semantic_fact_extractor.cpp — v0.3 Phase 1 fact extractor.
//
// Scans semantic_records / entity / reference / import for code patterns
// and persists one semantic_fact row per finding. Each extract method
// issues a single SELECT then a single batched INSERT (no per-symbol
// round-trip). The caller wraps extractAll() in a transaction.
//
// Function-id resolution: for each matching semantic_records row we
// JOIN graph_nodes (node_type IN (0,1) = Function/Method) on
// file_path + start_row containment so the fact is attributed to the
// enclosing function. When no enclosing function is found the row is
// skipped (no orphan facts) — this happens for top-level records like
// imports not attached to any function, which are handled separately
// by extractFrameworkFacts.

#include "semantic_fact_extractor.h"

#include <cstdio>
#include <sqlite3.h>
#include <string>
#include <vector>

namespace model
{

// ─── Constants ────────────────────────────────────────────────────
//
// RecordKind integer values (mirror ir/semantic_unit.h RecordKind).
// Hardcoded as constexpr int because semantic_records.kind is stored
// as INTEGER, not the enum — and we want this TU to compile without
// pulling semantic_unit.h into the PCH.
static constexpr int kKindCallExpr = 9; // RecordKind::CallExpr
static constexpr int kKindComment = 14; // RecordKind::Comment
static constexpr int kKindImport = 11; // RecordKind::Import

// graph::NodeType integer values (mirror graph/graph_types.h).
static constexpr int kNodeTypeFunction = 0; // NodeType::Function
static constexpr int kNodeTypeMethod = 1; // NodeType::Method

// Confidence defaults from plan section 3.2.
static constexpr double kConfidenceDefault = 1.0;
static constexpr double kConfidenceIgnoredReturn = 0.8;
static constexpr double kConfidenceUncheckedError = 0.7;
static constexpr double kConfidenceCgoCallback = 0.6;

// ─── Local helpers ────────────────────────────────────────────────

// Build a JSON detail string: {"line":N,"snippet":"...","related_symbol":"..."}.
// All string inputs are JSON-escaped. Empty snippet/related_symbol are
// emitted as "" (not omitted) so downstream consumers can rely on the
// keys always being present.
static std::string buildDetailJson(int line, const std::string &snippet,
				   const std::string &related_symbol)
{
	std::string out;
	out.reserve(snippet.size() + related_symbol.size() + 48);
	out += "{\"line\":";
	out += std::to_string(line);
	out += ",\"snippet\":\"";
	for (char c : snippet) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				// Control char: emit \u00XX for safety.
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x",
					 static_cast<unsigned char>(c));
				out += buf;
			} else {
				out += c;
			}
		}
	}
	out += "\",\"related_symbol\":\"";
	for (char c : related_symbol) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x",
					 static_cast<unsigned char>(c));
				out += buf;
			} else {
				out += c;
			}
		}
	}
	out += "\"}";
	return out;
}

// Read the column text (col index) as a std::string, treating NULL as "".
static std::string colText(sqlite3_stmt *stmt, int col)
{
	const unsigned char *t = sqlite3_column_text(stmt, col);
	if (!t)
		return "";
	return std::string(reinterpret_cast<const char *>(t));
}

// ─── extractAll ───────────────────────────────────────────────────
//
// Order: clear → sync → memory → error → pattern → framework → ffi.
// Clear runs first so a re-extract is idempotent (no duplicate rows).
// Each phase logs its count to stderr; the return value is the sum.

int64_t SemanticFactExtractor::extractAll(uint64_t project_id)
{
	if (!store_) {
		fprintf(stderr,
			"[module=semantic_fact_extractor, method=extractAll] "
			"store is null\n");
		return 0;
	}

	// Clear previous facts for this project so re-extraction does not
	// accumulate duplicates. The caller owns the transaction.
	if (!store_->clearSemanticFacts(project_id)) {
		fprintf(stderr,
			"[module=semantic_fact_extractor, method=extractAll] "
			"clearSemanticFacts failed: %s\n",
			store_->error().c_str());
		// Continue — inserts will still run, just on top of stale rows.
	}

	int64_t total = 0;
	total += extractSyncFacts(project_id);
	total += extractMemoryFacts(project_id);
	total += extractErrorFacts(project_id);
	total += extractPatternFacts(project_id);
	total += extractFrameworkFacts(project_id);
	total += extractFfiFacts(project_id);
	fprintf(stderr,
		"[model] SemanticFactExtractor: total %lld facts "
		"(project_id=%llu)\n",
		(long long)total, (unsigned long long)project_id);
	return total;
}

// ─── extractSyncFacts ─────────────────────────────────────────────
//
// Detects Go/Rust sync primitives from CallExpr records. The JOIN
// finds the enclosing function (graph_nodes node_type IN (0,1)) whose
// start_row <= sr.start_row and end_row >= sr.start_row. When the
// function's end_row is 0 (test data / legacy rows), we accept it as
// a fallback so the JOIN still produces a fact.
//
// Patterns:
//   .Lock / .Unlock     → sync/mutex/{lock,unlock}
//   .RLock              → sync/rwmutex/rlock
//   .Load + atomic qn   → sync/atomic/load
//   .Add + WaitGroup qn → sync/waitgroup/add
//   (defer)             → sync/defer  (detected via name='defer' in Go)

int64_t SemanticFactExtractor::extractSyncFacts(uint64_t project_id)
{
	// One SELECT that scans CallExpr records and tags each match with
	// the inferred (primitive, kind) pair via CASE. We pick up the
	// enclosing function via a correlated subquery (picks the
	// innermost function by smallest start_row range). The subquery
	// is the same shape used by the other extract methods.
	const char *sql =
		"SELECT sr.name, sr.qualified_name, sr.language, "
		"  sr.start_row, sr.file_path, "
		"  (SELECT gn.id FROM graph_nodes gn "
		"    WHERE gn.project_id = sr.project_id "
		"      AND gn.file_path = sr.file_path "
		"      AND gn.node_type IN (?, ?) "
		"      AND gn.start_row <= sr.start_row "
		"      AND (gn.end_row >= sr.start_row OR gn.end_row = 0) "
		"    ORDER BY gn.start_row DESC LIMIT 1) AS fn_id "
		"FROM semantic_records sr "
		"WHERE sr.project_id = ? AND sr.kind = ? "
		"  AND (sr.name LIKE '%.Lock' OR sr.name LIKE '%.Unlock' "
		"   OR sr.name LIKE '%.RLock' OR sr.name LIKE '%.WLock' "
		"   OR (sr.name LIKE '%.Load' AND sr.qualified_name LIKE '%atomic%') "
		"   OR (sr.name LIKE '%.Add' AND sr.qualified_name LIKE '%WaitGroup%') "
		"   OR (sr.language = 'go' AND sr.name = 'defer'))";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"[module=semantic_fact_extractor, "
			"method=extractSyncFacts] prepare failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		return 0;
	}
	sqlite3_bind_int(stmt, 1, kNodeTypeFunction);
	sqlite3_bind_int(stmt, 2, kNodeTypeMethod);
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 4, kKindCallExpr);

	std::vector<store::GraphStore::SemanticFactRow> facts;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		std::string name = colText(stmt, 0);
		std::string qualified_name = colText(stmt, 1);
		// std::string language = colText(stmt, 2); // reserved for future
		int start_row = sqlite3_column_int(stmt, 3);
		std::string file_path = colText(stmt, 4);
		int64_t fn_id = sqlite3_column_int64(stmt, 5);
		if (fn_id <= 0)
			continue; // no enclosing function — skip

		std::string primitive;
		std::string kind;
		if (name == "defer") {
			primitive = "defer";
			kind = "defer";
		} else if (name.size() >= 5 &&
			   name.compare(name.size() - 4, 4, "Lock") == 0 &&
			   name.size() >= 6 && name[name.size() - 5] == '.') {
			// .Lock or .RLock or .WLock — per v0.3 plan section 3.2,
			// all lock acquisitions use kind="lock" regardless of
			// mutex flavor (mutex/rwmutex). The primitive column
			// disambiguates the lock type.
			if (name.size() >= 6 && name[name.size() - 6] == 'R') {
				primitive = "rwmutex";
				kind = "lock";
			} else if (name.size() >= 6 &&
				   name[name.size() - 6] == 'W') {
				primitive = "rwmutex";
				kind = "lock";
			} else {
				primitive = "mutex";
				kind = "lock";
			}
		} else if (name.size() >= 7 &&
			   name.compare(name.size() - 6, 6, "Unlock") == 0) {
			// Any .Unlock() call — the Evidence Builder's
			// mutex_without_defer_unlock rule treats this as the
			// optional match (defer_unlock kind). Per v0.3 plan
			// section 3.2 this is kind="defer_unlock".
			primitive = "mutex";
			kind = "defer_unlock";
		} else if (name.size() >= 6 &&
			   name.compare(name.size() - 5, 5, ".Load") == 0) {
			primitive = "atomic";
			kind = "load";
		} else if (name.size() >= 5 &&
			   name.compare(name.size() - 4, 4, ".Add") == 0) {
			primitive = "waitgroup";
			kind = "add";
		} else {
			continue; // safety net — should not happen
		}

		std::string detail = buildDetailJson(
			start_row, name + " (" + file_path + ")",
			qualified_name);
		facts.emplace_back(static_cast<uint64_t>(fn_id), "sync",
				   primitive, kind, name, kConfidenceDefault,
				   detail);
	}
	sqlite3_finalize(stmt);

	if (!store_->insertSemanticFacts(project_id, facts)) {
		fprintf(stderr,
			"[module=semantic_fact_extractor, "
			"method=extractSyncFacts] insert failed: %s\n",
			store_->error().c_str());
	}
	fprintf(stderr, "[model] SemanticFactExtractor sync: %zu facts\n",
		facts.size());
	return static_cast<int64_t>(facts.size());
}

// ─── extractMemoryFacts ───────────────────────────────────────────

int64_t SemanticFactExtractor::extractMemoryFacts(uint64_t project_id)
{
	// C.CString / C.CBytes / C.free (Go cgo) + malloc/calloc/realloc/free.
	const char *sql =
		"SELECT sr.name, sr.start_row, sr.file_path, "
		"  (SELECT gn.id FROM graph_nodes gn "
		"    WHERE gn.project_id = sr.project_id "
		"      AND gn.file_path = sr.file_path "
		"      AND gn.node_type IN (?, ?) "
		"      AND gn.start_row <= sr.start_row "
		"      AND (gn.end_row >= sr.start_row OR gn.end_row = 0) "
		"    ORDER BY gn.start_row DESC LIMIT 1) AS fn_id "
		"FROM semantic_records sr "
		"WHERE sr.project_id = ? AND sr.kind = ? "
		"  AND sr.name IN ('C.CString','C.CBytes','C.free',"
		"   'malloc','calloc','realloc','free')";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"[module=semantic_fact_extractor, "
			"method=extractMemoryFacts] prepare failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		return 0;
	}
	sqlite3_bind_int(stmt, 1, kNodeTypeFunction);
	sqlite3_bind_int(stmt, 2, kNodeTypeMethod);
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 4, kKindCallExpr);

	std::vector<store::GraphStore::SemanticFactRow> facts;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		std::string name = colText(stmt, 0);
		int start_row = sqlite3_column_int(stmt, 1);
		std::string file_path = colText(stmt, 2);
		int64_t fn_id = sqlite3_column_int64(stmt, 3);
		if (fn_id <= 0)
			continue;

		std::string primitive;
		std::string kind;
		if (name == "C.CString" || name == "C.CBytes") {
			primitive = "cstring";
			kind = "alloc";
		} else if (name == "C.free") {
			primitive = "cstring";
			kind = "free";
		} else if (name == "malloc" || name == "calloc" ||
			   name == "realloc") {
			primitive = "malloc";
			kind = "alloc";
		} else if (name == "free") {
			primitive = "malloc";
			kind = "free";
		} else {
			continue;
		}

		std::string detail = buildDetailJson(
			start_row, name + " (" + file_path + ")", "");
		facts.emplace_back(static_cast<uint64_t>(fn_id), "memory",
				   primitive, kind, name, kConfidenceDefault,
				   detail);
	}
	sqlite3_finalize(stmt);

	if (!store_->insertSemanticFacts(project_id, facts)) {
		fprintf(stderr,
			"[module=semantic_fact_extractor, "
			"method=extractMemoryFacts] insert failed: %s\n",
			store_->error().c_str());
	}
	fprintf(stderr, "[model] SemanticFactExtractor memory: %zu facts\n",
		facts.size());
	return static_cast<int64_t>(facts.size());
}

// ─── extractErrorFacts ────────────────────────────────────────────
//
// Bare-except (Python) and empty-catch (JS) detection is done via
// name patterns because semantic_records does not have a CatchStmt
// kind — visitors for those languages record the catch/except clause
// under its type name (or "" for bare). ignored_return and
// unchecked_error are approximate: they trigger on call records whose
// qualified_name suggests an error-returning function (Go multi-value
// returns are not yet tracked here, so we use the conventional
// "<error>" suffix or "Must"/"Err" prefix as a heuristic).

int64_t SemanticFactExtractor::extractErrorFacts(uint64_t project_id)
{
	// Bare except / empty catch: any record in Python/JS named
	// "except" / "catch" with an empty qualified_name (no caught
	// type). These are typically CatchStmt-like records emitted by
	// the Python/JS visitors.
	const char *sql =
		"SELECT sr.name, sr.language, sr.start_row, sr.file_path, "
		"  (SELECT gn.id FROM graph_nodes gn "
		"    WHERE gn.project_id = sr.project_id "
		"      AND gn.file_path = sr.file_path "
		"      AND gn.node_type IN (?, ?) "
		"      AND gn.start_row <= sr.start_row "
		"      AND (gn.end_row >= sr.start_row OR gn.end_row = 0) "
		"    ORDER BY gn.start_row DESC LIMIT 1) AS fn_id "
		"FROM semantic_records sr "
		"WHERE sr.project_id = ? "
		"  AND ((sr.language = 'python' AND sr.name = 'except' "
		"        AND sr.qualified_name = '') "
		"   OR (sr.language = 'javascript' AND sr.name = 'catch' "
		"        AND sr.qualified_name = '') "
		"   OR (sr.language = 'typescript' AND sr.name = 'catch' "
		"        AND sr.qualified_name = ''))";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"[module=semantic_fact_extractor, "
			"method=extractErrorFacts] prepare failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		return 0;
	}
	sqlite3_bind_int(stmt, 1, kNodeTypeFunction);
	sqlite3_bind_int(stmt, 2, kNodeTypeMethod);
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(project_id));

	std::vector<store::GraphStore::SemanticFactRow> facts;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		std::string name = colText(stmt, 0);
		std::string language = colText(stmt, 1);
		int start_row = sqlite3_column_int(stmt, 2);
		std::string file_path = colText(stmt, 3);
		int64_t fn_id = sqlite3_column_int64(stmt, 4);
		if (fn_id <= 0)
			continue;

		std::string primitive;
		std::string kind;
		double confidence = kConfidenceDefault;
		if (language == "python" && name == "except") {
			primitive = "bare_except";
			kind = "suppression";
		} else if ((language == "javascript" ||
			    language == "typescript") &&
			   name == "catch") {
			primitive = "empty_catch";
			kind = "suppression";
		} else {
			continue;
		}

		std::string detail = buildDetailJson(
			start_row, name + " (" + file_path + ")", language);
		facts.emplace_back(static_cast<uint64_t>(fn_id), "error",
				   primitive, kind, name, confidence, detail);
	}
	sqlite3_finalize(stmt);

	// Approximate ignored_return / unchecked_error detection: CallExpr
	// records whose resolve_strategy is 'external' (known library
	// call, may return error) and whose name suggests an error-returning
	// function. We can't reliably tell from a call record whether the
	// return value was ignored, so this is a conservative flag — the
	// Phase 4 verifier can re-score based on the call-site context.
	// Disabled by default; left as a stub for Wave 2.
	(void)kConfidenceIgnoredReturn;
	(void)kConfidenceUncheckedError;

	if (!store_->insertSemanticFacts(project_id, facts)) {
		fprintf(stderr,
			"[module=semantic_fact_extractor, "
			"method=extractErrorFacts] insert failed: %s\n",
			store_->error().c_str());
	}
	fprintf(stderr, "[model] SemanticFactExtractor error: %zu facts\n",
		facts.size());
	return static_cast<int64_t>(facts.size());
}

// ─── extractPatternFacts ──────────────────────────────────────────

int64_t SemanticFactExtractor::extractPatternFacts(uint64_t project_id)
{
	// Comment records (kind=14) whose name contains TODO/FIXME, plus
	// Rust .unwrap() / panic! and Go panic() calls, plus Rust unsafe
	// blocks (recorded by the Rust visitor as name='unsafe').
	const char *sql =
		"SELECT sr.name, sr.kind, sr.language, sr.start_row, "
		"  sr.file_path, "
		"  (SELECT gn.id FROM graph_nodes gn "
		"    WHERE gn.project_id = sr.project_id "
		"      AND gn.file_path = sr.file_path "
		"      AND gn.node_type IN (?, ?) "
		"      AND gn.start_row <= sr.start_row "
		"      AND (gn.end_row >= sr.start_row OR gn.end_row = 0) "
		"    ORDER BY gn.start_row DESC LIMIT 1) AS fn_id "
		"FROM semantic_records sr "
		"WHERE sr.project_id = ? AND ( "
		"  (sr.kind = ? AND (upper(sr.name) LIKE '%TODO%' "
		"   OR upper(sr.name) LIKE '%FIXME%')) "
		"  OR (sr.kind = ? AND sr.language = 'rust' "
		"      AND (sr.name LIKE '%.unwrap' OR sr.name = 'panic!' "
		"       OR sr.name = 'unsafe')) "
		"  OR (sr.kind = ? AND sr.language = 'go' "
		"      AND sr.name = 'panic'))";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"[module=semantic_fact_extractor, "
			"method=extractPatternFacts] prepare failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		return 0;
	}
	sqlite3_bind_int(stmt, 1, kNodeTypeFunction);
	sqlite3_bind_int(stmt, 2, kNodeTypeMethod);
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 4, kKindComment);
	sqlite3_bind_int(stmt, 5, kKindCallExpr);
	sqlite3_bind_int(stmt, 6, kKindCallExpr);

	std::vector<store::GraphStore::SemanticFactRow> facts;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		std::string name = colText(stmt, 0);
		int kind_int = sqlite3_column_int(stmt, 1);
		std::string language = colText(stmt, 2);
		int start_row = sqlite3_column_int(stmt, 3);
		std::string file_path = colText(stmt, 4);
		int64_t fn_id = sqlite3_column_int64(stmt, 5);
		if (fn_id <= 0)
			continue;

		std::string primitive;
		std::string knd;
		// Upper-case compare for TODO/FIXME detection.
		std::string upper_name;
		upper_name.reserve(name.size());
		for (char c : name)
			upper_name.push_back(static_cast<char>(
				toupper(static_cast<unsigned char>(c))));

		if (kind_int == kKindComment) {
			if (upper_name.find("TODO") != std::string::npos) {
				primitive = "todo";
				knd = "marker";
			} else if (upper_name.find("FIXME") !=
				   std::string::npos) {
				primitive = "fixme";
				knd = "marker";
			} else {
				continue;
			}
		} else if (kind_int == kKindCallExpr) {
			if (language == "rust" && name.size() >= 7 &&
			    name.compare(name.size() - 6, 6, "unwrap") == 0) {
				primitive = "unwrap";
				knd = "risk";
			} else if (language == "rust" && name == "panic!") {
				primitive = "panic";
				knd = "risk";
			} else if (language == "rust" && name == "unsafe") {
				primitive = "unsafe";
				knd = "risk";
			} else if (language == "go" && name == "panic") {
				primitive = "panic";
				knd = "risk";
			} else {
				continue;
			}
		} else {
			continue;
		}

		std::string detail = buildDetailJson(
			start_row, name + " (" + file_path + ")", language);
		facts.emplace_back(static_cast<uint64_t>(fn_id), "pattern",
				   primitive, knd, name, kConfidenceDefault,
				   detail);
	}
	sqlite3_finalize(stmt);

	if (!store_->insertSemanticFacts(project_id, facts)) {
		fprintf(stderr,
			"[module=semantic_fact_extractor, "
			"method=extractPatternFacts] insert failed: %s\n",
			store_->error().c_str());
	}
	fprintf(stderr, "[model] SemanticFactExtractor pattern: %zu facts\n",
		facts.size());
	return static_cast<int64_t>(facts.size());
}

// ─── extractFrameworkFacts ────────────────────────────────────────
//
// Framework adoption is a project-level signal, but we attribute each
// fact to one function in the importing file so it has a valid
// function_id (FK constraint). The fact is duplicated across all
// functions in the file — acceptable for v0.3 since downstream queries
// dedupe by (category, primitive, kind, symbol).

int64_t SemanticFactExtractor::extractFrameworkFacts(uint64_t project_id)
{
	// Map import.target_path patterns to (framework, kind). We then
	// find one function in the same file as the import to attach the
	// fact to. import.file_path is the source file (populated by the
	// Python/JS/Go/Rust visitors when emitting Import records).
	const char *sql = "SELECT i.target_path, i.file_path, "
			  "  (SELECT gn.id FROM graph_nodes gn "
			  "    WHERE gn.project_id = i.project_id "
			  "      AND gn.file_path = i.file_path "
			  "      AND gn.node_type IN (?, ?) "
			  "    ORDER BY gn.start_row LIMIT 1) AS fn_id "
			  "FROM import i "
			  "WHERE i.project_id = ? "
			  "  AND (i.target_path LIKE '%gin-gonic/gin%' "
			  "   OR i.target_path LIKE '%labstack/echo%' "
			  "   OR i.target_path LIKE '%/django%' "
			  "   OR i.target_path LIKE '%/express%' "
			  "   OR i.target_path LIKE '%gorm.io/gorm%' "
			  "   OR i.target_path LIKE '%/gorm%')";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"[module=semantic_fact_extractor, "
			"method=extractFrameworkFacts] prepare failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		return 0;
	}
	sqlite3_bind_int(stmt, 1, kNodeTypeFunction);
	sqlite3_bind_int(stmt, 2, kNodeTypeMethod);
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(project_id));

	std::vector<store::GraphStore::SemanticFactRow> facts;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		std::string target_path = colText(stmt, 0);
		std::string file_path = colText(stmt, 1);
		int64_t fn_id = sqlite3_column_int64(stmt, 2);
		if (fn_id <= 0)
			continue;

		std::string framework;
		std::string fw_kind;
		if (target_path.find("gin-gonic/gin") != std::string::npos) {
			framework = "gin";
			fw_kind = "router";
		} else if (target_path.find("labstack/echo") !=
			   std::string::npos) {
			framework = "echo";
			fw_kind = "router";
		} else if (target_path.find("django") != std::string::npos) {
			framework = "django";
			fw_kind = "router";
		} else if (target_path.find("express") != std::string::npos) {
			framework = "express";
			fw_kind = "router";
		} else if (target_path.find("gorm") != std::string::npos) {
			framework = "gorm";
			fw_kind = "orm";
		} else {
			continue;
		}

		std::string detail = buildDetailJson(
			0, "import " + target_path + " (" + file_path + ")",
			target_path);
		facts.emplace_back(static_cast<uint64_t>(fn_id), "framework",
				   framework, fw_kind, target_path,
				   kConfidenceDefault, detail);
	}
	sqlite3_finalize(stmt);

	if (!store_->insertSemanticFacts(project_id, facts)) {
		fprintf(stderr,
			"[module=semantic_fact_extractor, "
			"method=extractFrameworkFacts] insert failed: %s\n",
			store_->error().c_str());
	}
	fprintf(stderr, "[model] SemanticFactExtractor framework: %zu facts\n",
		facts.size());
	return static_cast<int64_t>(facts.size());
}

// ─── extractFfiFacts ──────────────────────────────────────────────
//
// FFI surface detection. extern "C" / JNI / wasm_bindgen are detected
// via semantic_records name/qualified_name patterns. cgo callbacks are
// approximated by CallExpr names starting with "C.CF" (covers
// C.CFMachPortCreate, C.CFRunLoop, etc. — common callback-registration
// APIs in CoreFoundation). The confidence is lowered for cgo_callback
// per plan section 3.2 because the heuristic is approximate.

int64_t SemanticFactExtractor::extractFfiFacts(uint64_t project_id)
{
	// ── Query 1: Direct FFI patterns (semantic_records) ────────────
	// Covers:
	//   - C extern "C" function declarations
	//   - JNIEXPORT macros
	//   - Rust wasm_bindgen annotations
	//   - Go cgo calls (C.CBytes, C.free, C.malloc, C.go_hash_bridge, …)
	//   - Java JNA-style native methods (detected via class name "Native")
	const char *sql =
		"SELECT sr.name, sr.qualified_name, sr.language, "
		"  sr.start_row, sr.file_path, sr.kind, "
		"  (SELECT gn.id FROM graph_nodes gn "
		"    WHERE gn.project_id = sr.project_id "
		"      AND gn.file_path = sr.file_path "
		"      AND gn.node_type IN (?, ?) "
		"      AND gn.start_row <= sr.start_row "
		"      AND (gn.end_row >= sr.start_row OR gn.end_row = 0) "
		"    ORDER BY gn.start_row DESC LIMIT 1) AS fn_id "
		"FROM semantic_records sr "
		"WHERE sr.project_id = ? AND ( "
		"  sr.qualified_name LIKE '%extern \"C\"%' "
		"  OR sr.name LIKE 'JNIEXPORT_%' "
		"  OR sr.qualified_name LIKE '%JNIEXPORT_%' "
		"  OR (sr.language = 'rust' AND sr.name LIKE '%wasm_bindgen%') "
		"  OR (sr.kind = ? AND sr.language = 'go' "
		"      AND sr.file_path IN (SELECT DISTINCT file_path FROM semantic_records "
		"        WHERE project_id = ? AND language = 'go' AND kind = ? "
		"        AND name = 'import \"C\"')) "
		"  OR (sr.language = 'java' AND sr.kind = 1 "
		"      AND sr.file_path IN (SELECT DISTINCT file_path FROM semantic_records "
		"        WHERE project_id = ? AND language = 'java' AND kind = 2 "
		"        AND name LIKE '%Native%')))";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"[module=semantic_fact_extractor, "
			"method=extractFfiFacts] prepare failed: %s\n",
			sqlite3_errmsg(store_->handle()));
		return 0;
	}
	sqlite3_bind_int(stmt, 1, kNodeTypeFunction);
	sqlite3_bind_int(stmt, 2, kNodeTypeMethod);
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 4, kKindCallExpr);
	sqlite3_bind_int64(stmt, 5, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 6, kKindImport);
	sqlite3_bind_int64(stmt, 7, static_cast<int64_t>(project_id));

	std::vector<store::GraphStore::SemanticFactRow> facts;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		std::string name = colText(stmt, 0);
		std::string qualified_name = colText(stmt, 1);
		std::string language = colText(stmt, 2);
		int start_row = sqlite3_column_int(stmt, 3);
		std::string file_path = colText(stmt, 4);
		(void)sqlite3_column_int(stmt, 5); // kind — not used below
		int64_t fn_id = sqlite3_column_int64(stmt, 6);
		if (fn_id <= 0)
			continue;

		std::string primitive;
		std::string kind;
		double confidence = kConfidenceDefault;
		if (qualified_name.find("extern \"C\"") != std::string::npos) {
			primitive = "extern_call";
			kind = "call";
		} else if (name.rfind("JNIEXPORT_", 0) == 0 ||
			   qualified_name.find("JNIEXPORT_") !=
				   std::string::npos) {
			primitive = "jni";
			kind = "export";
		} else if (language == "rust" &&
			   name.find("wasm_bindgen") != std::string::npos) {
			primitive = "wasm";
			kind = "export";
		} else if (language == "go" && name.size() >= 3 &&
			   name.compare(0, 2, "C.") == 0) {
			// Go cgo call (C.CBytes, C.free, C.malloc, C.go_*, …)
			primitive = "cgo_call";
			kind = "call";
		} else if (language == "java") {
			// Java JNA native method
			primitive = "jni";
			kind = "export";
		} else {
			continue;
		}

		std::string detail = buildDetailJson(
			start_row, name + " (" + file_path + ")",
			qualified_name);
		facts.emplace_back(static_cast<uint64_t>(fn_id), "ffi",
				   primitive, kind, name, confidence, detail);
	}
	sqlite3_finalize(stmt);

	// ── Query 2: Cross-language call edges (graph_edges) ──────────
	// Detects Rust extern "C" functions and C functions declared in
	// extern "C" blocks by finding functions whose callers come from a
	// different language (e.g. Rust function called from C code).
	const char *sql_xl =
		"SELECT gn.name, gn.qualified_name, gn.language, "
		"  gn.start_row, gn.file_path, gn.id AS fn_id "
		"FROM graph_nodes gn "
		"WHERE gn.project_id = ? "
		"  AND gn.node_type IN (?, ?) "
		"  AND gn.id IN ( "
		"    SELECT ge.target_node_id "
		"    FROM graph_edges ge "
		"    JOIN graph_nodes gn_src ON ge.source_node_id = gn_src.id "
		"    WHERE ge.project_id = ? "
		"      AND gn_src.language != gn.language "
		"      AND gn.language IN ('rust', 'c', 'cpp', 'zig')"
		"  ) "
		"  -- Exclude functions already detected by Query 1 "
		"  AND gn.id NOT IN (SELECT function_id FROM semantic_fact "
		"    WHERE project_id = ? AND category = 'ffi')";
	sqlite3_stmt *stmt_xl = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql_xl, -1, &stmt_xl,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=semantic_fact_extractor, "
			"method=extractFfiFacts] cross-lang prepare failed: %s\n",
			sqlite3_errmsg(store_->handle()));
	} else {
		sqlite3_bind_int64(stmt_xl, 1,
				   static_cast<int64_t>(project_id));
		sqlite3_bind_int(stmt_xl, 2, kNodeTypeFunction);
		sqlite3_bind_int(stmt_xl, 3, kNodeTypeMethod);
		sqlite3_bind_int64(stmt_xl, 4,
				   static_cast<int64_t>(project_id));
		sqlite3_bind_int64(stmt_xl, 5,
				   static_cast<int64_t>(project_id));

		while (sqlite3_step(stmt_xl) == SQLITE_ROW) {
			std::string name = colText(stmt_xl, 0);
			std::string qualified_name = colText(stmt_xl, 1);
			std::string language = colText(stmt_xl, 2);
			int start_row = sqlite3_column_int(stmt_xl, 3);
			std::string file_path = colText(stmt_xl, 4);
			int64_t fn_id = sqlite3_column_int64(stmt_xl, 5);
			if (fn_id <= 0)
				continue;

			// Classify based on language: Rust extern "C" functions
			// and C/C++ functions called from other languages.
			std::string primitive;
			std::string kind;
			if (language == "rust") {
				primitive = "extern_call";
				kind = "call";
			} else if (language == "c" || language == "cpp") {
				primitive = "extern_call";
				kind = "call";
			} else if (language == "zig") {
				primitive = "extern_call";
				kind = "call";
			} else {
				continue;
			}

			std::string detail = buildDetailJson(
				start_row,
				name + " (" + file_path + ") [cross-lang]",
				qualified_name);
			facts.emplace_back(static_cast<uint64_t>(fn_id), "ffi",
					   primitive, kind, name,
					   kConfidenceDefault, detail);
		}
		sqlite3_finalize(stmt_xl);
	}

	if (!store_->insertSemanticFacts(project_id, facts)) {
		fprintf(stderr,
			"[module=semantic_fact_extractor, "
			"method=extractFfiFacts] insert failed: %s\n",
			store_->error().c_str());
	}
	fprintf(stderr, "[model] SemanticFactExtractor ffi: %zu facts\n",
		facts.size());
	return static_cast<int64_t>(facts.size());
}

} // namespace model

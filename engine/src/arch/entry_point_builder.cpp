// entry_point_builder.cpp — unified entry point detection and persistence.
//
// development_plan §9.4: the old entry_points table has a broken FK and no
// write path. The new arch_entry_point table (migration 14) distinguishes
// entry kinds and records confidence and source evidence.
//
// This file converges three previously inconsistent implementations:
// 1. query_analysis.cpp:253 — name whitelist (main/run/start/init/setup)
// 2. graph_builder.cpp:isEntryPointName — language-aware main/init matching
// 3. state_builder.cpp — SQL-based entity.name + entity.language matching
//
// The unified approach classifies entry points into four kinds and writes
// them to arch_entry_point with appropriate confidence and source metadata.

#include "entry_point_builder.h"

#include "store/store.h"

#include <sqlite3.h>
#include <string>
#include <unordered_set>
#include <vector>

namespace arch
{
namespace
{

/// Check if a function name is an explicit program entry point.
/// "main" is the explicit entry for C, C++, Go, and Rust.
/// "init" is an implicit entry for Go (executed at package load).
/// @return "explicit" if it is, empty otherwise.
std::string explicitEntryKind(const std::string &name,
			      const std::string &language)
{
	if (name == "main") {
		if (language == "c" || language == "cpp" || language == "c++" ||
		    language == "go" || language == "rust")
			return "explicit";
	}
	if (name == "init" && language == "go")
		return "explicit";
	return "";
}

/// Check if a function name is a framework entry candidate.
/// These are common framework-level entry names (run, start, setup, etc.)
/// that are often called by a framework runtime, not directly by user code.
/// @return true if the name matches a framework entry pattern.
bool isFrameworkEntryName(const std::string &name)
{
	// Exact match against common framework/runtime entry names.
	// Case-sensitive: "Main" (Go) and "Run" are different entry
	// patterns from "main" and "run".
	static const std::unordered_set<std::string> kFrameworkNames = {
		"run",	 "Run",	  "start", "Start",   "setup",
		"Setup", "serve", "Serve", "handler", "Handler"
	};
	return kFrameworkNames.count(name) > 0;
}

/// Check if a function is likely an exported API entry point.
/// In Go, exported functions start with an uppercase letter.
/// In Rust, #[no_mangle] functions are exported (we approximate by
/// checking if the name contains "ffi" or "export" as a heuristic).
/// @return true if the function appears to be exported.
bool isExportedAPI(const std::string &name, const std::string &language,
		   const std::string &qualified_name)
{
	if (language == "go") {
		// Go: capitalized function name = exported.
		if (!name.empty() && name[0] >= 'A' && name[0] <= 'Z')
			return true;
	}
	if (language == "rust") {
		// Rust: #[no_mangle] is not directly queryable from entity
		// data. We approximate by checking if the qualified name
		// contains "ffi" or "export" — this is a heuristic candidate.
		if (qualified_name.find("ffi") != std::string::npos ||
		    qualified_name.find("export") != std::string::npos)
			return true;
	}
	return false;
}

/// Check if a name is in the legacy heuristic whitelist.
/// These are the names from query_analysis.cpp:253 that were used as a
/// complete entry set (incorrectly). They are now only candidates.
bool isHeuristicCandidate(const std::string &name)
{
	static const std::unordered_set<std::string> kHeuristicNames = {
		"main",	 "Main", "run",	 "Run",	  "start",
		"Start", "init", "Init", "setup", "Setup"
	};
	return kHeuristicNames.count(name) > 0;
}

/// One detected entry point candidate.
struct EntryPointCandidate {
	int64_t entity_id;
	std::string kind;
	double confidence;
	std::string source; // "name_pattern" or "exported" or "explicit"
};

} // namespace

bool buildEntryPoints(store::GraphStore &store, uint64_t project_id,
		      std::string &error)
{
	sqlite3 *db = store.handle();
	if (!db) {
		error = "database not open [module=arch, "
			"method=buildEntryPoints]";
		return false;
	}

	// Clear existing entry points for this project.
	{
		std::string del =
			"DELETE FROM arch_entry_point WHERE project_id = " +
			std::to_string(project_id);
		if (sqlite3_exec(db, del.c_str(), nullptr, nullptr, nullptr) !=
		    SQLITE_OK) {
			error = "DELETE arch_entry_point failed: " +
				std::string(sqlite3_errmsg(db));
			return false;
		}
	}

	// Query all function/method entities for entry point classification.
	// kind IN (0,1) selects functions and methods from the entity table.
	const char *sql = "SELECT id, name, language, file_path, start_row, "
			  "qualified_name FROM entity "
			  "WHERE project_id=? AND kind IN (0,1)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error = std::string(
				"prepare entity for entry points failed: ") +
			sqlite3_errmsg(db);
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	// Insert statement (reused for all candidates).
	const char *insert_sql =
		"INSERT OR IGNORE INTO arch_entry_point "
		"(project_id, entity_id, kind, confidence, source, "
		"evidence_file, evidence_row) "
		"VALUES (?,?,?,?,?,?,?)";
	sqlite3_stmt *insert_stmt = nullptr;
	if (sqlite3_prepare_v2(db, insert_sql, -1, &insert_stmt, nullptr) !=
	    SQLITE_OK) {
		error = "prepare arch_entry_point insert failed: " +
			std::string(sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return false;
	}

	// Track which (entity_id, kind) pairs we've already inserted to
	// avoid duplicates when multiple patterns match.
	std::unordered_set<int64_t> seen_explicit;
	std::unordered_set<int64_t> seen_framework;
	std::unordered_set<int64_t> seen_exported;
	std::unordered_set<int64_t> seen_heuristic;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int64_t entity_id = sqlite3_column_int64(stmt, 0);
		const unsigned char *name_ptr = sqlite3_column_text(stmt, 1);
		const unsigned char *lang_ptr = sqlite3_column_text(stmt, 2);
		const unsigned char *file_ptr = sqlite3_column_text(stmt, 3);
		int start_row = sqlite3_column_int(stmt, 4);
		const unsigned char *qn_ptr = sqlite3_column_text(stmt, 5);

		std::string name =
			name_ptr ? reinterpret_cast<const char *>(name_ptr) :
				   "";
		std::string language =
			lang_ptr ? reinterpret_cast<const char *>(lang_ptr) :
				   "";
		std::string file_path =
			file_ptr ? reinterpret_cast<const char *>(file_ptr) :
				   "";
		std::string qualified_name =
			qn_ptr ? reinterpret_cast<const char *>(qn_ptr) : "";

		// Classify the entity into one or more entry point kinds.
		// Priority: explicit > framework > exported > heuristic.
		std::vector<EntryPointCandidate> candidates;

		// 1. Explicit entry points (main, init in Go).
		std::string explicit_kind = explicitEntryKind(name, language);
		if (!explicit_kind.empty() && !seen_explicit.count(entity_id)) {
			candidates.push_back({ entity_id, explicit_kind, 1.0,
					       "name_pattern" });
			seen_explicit.insert(entity_id);
		}

		// 2. Framework entry candidates.
		if (isFrameworkEntryName(name) &&
		    !seen_framework.count(entity_id)) {
			candidates.push_back({ entity_id, "framework", 0.5,
					       "name_pattern" });
			seen_framework.insert(entity_id);
		}

		// 3. Exported API entries.
		if (isExportedAPI(name, language, qualified_name) &&
		    !seen_exported.count(entity_id)) {
			candidates.push_back(
				{ entity_id, "exported", 0.7, "exported" });
			seen_exported.insert(entity_id);
		}

		// 4. Heuristic candidates from the legacy whitelist.
		// Only add as heuristic if not already classified above.
		if (candidates.empty() && isHeuristicCandidate(name) &&
		    !seen_heuristic.count(entity_id)) {
			candidates.push_back({ entity_id, "heuristic", 0.3,
					       "name_pattern" });
			seen_heuristic.insert(entity_id);
		}

		// Insert all candidates for this entity.
		for (const auto &c : candidates) {
			sqlite3_bind_int64(insert_stmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int64(insert_stmt, 2, c.entity_id);
			sqlite3_bind_text(insert_stmt, 3, c.kind.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_double(insert_stmt, 4, c.confidence);
			sqlite3_bind_text(insert_stmt, 5, c.source.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_text(insert_stmt, 6, file_path.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_int(insert_stmt, 7, start_row);
			sqlite3_step(insert_stmt);
			sqlite3_reset(insert_stmt);
		}
	}

	sqlite3_finalize(insert_stmt);
	sqlite3_finalize(stmt);
	return true;
}

} // namespace arch

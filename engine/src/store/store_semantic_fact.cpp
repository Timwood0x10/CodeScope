// store_semantic_fact.cpp — Semantic Facts (v0.3 Phase 1) persistence.
//
// Implements GraphStore::insertSemanticFacts (batch insert) and
// GraphStore::clearSemanticFacts (delete-by-project). Both methods
// deliberately do NOT manage their own transaction: callers wrap them
// in a single BEGIN...COMMIT so an entire extractAll() run commits
// atomically with one fsync. This matches the StateBuilder pattern
// (state_builder.cpp::buildAll wraps buildModuleSummaries etc.).
//
// All errors are logged to stderr with the [module=, method=] trace
// chain required by plan/rules/code_rules.md; no error is silently
// swallowed. String bindings use SQLITE_STATIC because the input
// tuples outlive the step() call (extractor holds them in a vector).

#include "store.h"

#include <cstdio>
#include <sqlite3.h>
#include <string>
#include <tuple>
#include <vector>

namespace store
{

// ─── insertSemanticFacts ──────────────────────────────────────────
//
// One prepared statement is reused for every row in `facts` (prepare
// once, bind/step/reset in a loop). Re-preparing per row would cost
// ~50us per call on a hot cache — at 10k facts that's 500ms of pure
// parser overhead. The cached statement lives in the per-thread
// stmt cache (getCachedStmt), so re-entry from the same thread is
// also O(1) prepare.
//
// On any step failure we log the offending row's function_id and
// category so the user can grep for the bad input, then return false.
// We do NOT abort mid-batch on the first error — we continue stepping
// so the caller sees the full set of failures in one log pass. The
// return value reflects whether any row failed.

bool GraphStore::insertSemanticFacts(uint64_t project_id,
				     const std::vector<SemanticFactRow> &facts)
{
	if (facts.empty())
		return true;

	const char *sql =
		"INSERT INTO semantic_fact "
		"(project_id, function_id, category, primitive, kind, "
		" symbol, confidence, detail_json) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		// error_ already set by getCachedStmt; full trace logged there.
		return false;
	}

	bool ok = true;
	const int64_t pid = static_cast<int64_t>(project_id);
	for (const auto &row : facts) {
		const auto &function_id = std::get<0>(row);
		const auto &category = std::get<1>(row);
		const auto &primitive = std::get<2>(row);
		const auto &kind = std::get<3>(row);
		const auto &symbol = std::get<4>(row);
		const auto &confidence = std::get<5>(row);
		const auto &detail_json = std::get<6>(row);

		sqlite3_bind_int64(stmt, 1, pid);
		sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(function_id));
		sqlite3_bind_text(stmt, 3, category.c_str(), -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 4, primitive.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_text(stmt, 5, kind.c_str(), -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 6, symbol.c_str(), -1, SQLITE_STATIC);
		sqlite3_bind_double(stmt, 7, confidence);
		// detail_json may be empty — bind NULL to keep the column
		// nullable and distinguish "no detail" from "empty detail".
		if (detail_json.empty())
			sqlite3_bind_null(stmt, 8);
		else
			sqlite3_bind_text(stmt, 8, detail_json.c_str(), -1,
					  SQLITE_STATIC);

		int rc = sqlite3_step(stmt);
		if (rc != SQLITE_DONE) {
			error_ = std::string("insertSemanticFacts: step "
					     "failed: ") +
				 sqlite3_errmsg(db_);
			fprintf(stderr,
				"[module=store, method=insertSemanticFacts] "
				"step failed (rc=%d): %s "
				"(function_id=%llu, category=%s, primitive=%s)\n",
				rc, sqlite3_errmsg(db_),
				(unsigned long long)function_id,
				category.c_str(), primitive.c_str());
			ok = false;
		}
		sqlite3_reset(stmt);
		// Clear bindings so a NULL detail_json from row N does not
		// leak into row N+1 when row N+1 also binds slot 8 (SQLite
		// keeps the prior binding across reset if not rebound, but
		// clearing is defensive and cheap).
		sqlite3_clear_bindings(stmt);
	}
	return ok;
}

// ─── clearSemanticFacts ───────────────────────────────────────────
//
// Single DELETE scoped by project_id. The semantic_fact(project_id)
// predicate is covered by idx_sf_category / idx_sf_primitive /
// idx_sf_category_primitive — all three lead with project_id, so the
// query planner can pick any of them for the scan. No transaction
// management here: the caller (SemanticFactExtractor::extractAll)
// wraps clear + re-extract in one BEGIN...COMMIT so a crash between
// clear and re-extract rolls back to the pre-clear state.

bool GraphStore::clearSemanticFacts(uint64_t project_id)
{
	const char *sql = "DELETE FROM semantic_fact WHERE project_id = ?";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		error_ = std::string("clearSemanticFacts: step failed: ") +
			 sqlite3_errmsg(db_);
		fprintf(stderr,
			"[module=store, method=clearSemanticFacts] "
			"step failed (rc=%d): %s (project_id=%llu)\n",
			rc, sqlite3_errmsg(db_),
			(unsigned long long)project_id);
		return false;
	}
	return true;
}

} // namespace store

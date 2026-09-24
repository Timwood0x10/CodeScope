#include "store.h"
#include "store_internal.h"

#include <cstdio>
#include <sqlite3.h>

// Complete Claim/Verdict definitions are needed to read claim.type and to
// cast Verdict to its underlying integer for storage.
#include "../verify/claim.h"

namespace store
{

// ─── Knowledge + Evidence Layer (v0.3) ────────────────────────────
//
// All inserts use the prepared-statement cache (getCachedStmt) so repeated
// calls within an index/verify batch reuse a single sqlite3_stmt. String
// bindings use SQLITE_STATIC: the std::string arguments outlive the
// step() call, so no copy is needed. This matches the store_graph.cpp /
// store_insert.cpp conventions.
//
// Error handling: every failure path sets error_ (via getCachedStmt or
// explicit assignment) and returns a falsy value (-1 / false). Callers
// must check error() for diagnostics; no error is silently swallowed.

// ── capability ──────────────────────────────────────────────────────

bool GraphStore::insertCapability(uint64_t project_id, const std::string &name,
				  const std::string &summary,
				  const std::string &source_kind,
				  const std::string &source_ref)
{
	// Idempotent: the capability pass runs on every index, and an unguarded
	// INSERT appended another copy of every capability each run — inflating
	// capability_state and the drift counts derived from it. The identity is
	// (project_id, name, source_kind, source_ref); a migration removes what
	// earlier runs left behind (store_schema_migrations.cpp).
	const char *sql =
		"INSERT INTO capability (project_id, name, summary, source_kind, "
		"source_ref) SELECT ?, ?, ?, ?, ? WHERE NOT EXISTS "
		"(SELECT 1 FROM capability WHERE project_id = ? AND name = ? "
		"AND source_kind = ? AND source_ref = ?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, summary.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 4, source_kind.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 5, source_ref.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 6, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 7, name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 8, source_kind.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 9, source_ref.c_str(), -1, SQLITE_STATIC);

	int rc = sqlite3_step(stmt);
	sqlite3_reset(stmt);
	if (rc != SQLITE_DONE) {
		error_ = std::string("insertCapability: step failed: ") +
			 sqlite3_errmsg(db_);
		fprintf(stderr,
			"insertCapability: step failed (rc=%d): %s "
			"[module=store, method=insertCapability]\n",
			rc, sqlite3_errmsg(db_));
		return false;
	}
	return true;
}

// ── contract ────────────────────────────────────────────────────────

bool GraphStore::insertContract(uint64_t project_id, const std::string &name,
				const std::string &origin,
				const std::string &claim_text,
				const std::string &source_file, int source_line)
{
	// Idempotent: ContractPlugin re-runs on every runModelIndexSync.
	// A bare INSERT appended a duplicate row per rebuild (the same defect
	// insertCapability had). WHERE NOT EXISTS keeps one row per
	// (project_id, name, origin, source_file, source_line) identity.
	const char *sql =
		"INSERT INTO contract (project_id, name, origin, claim_text, "
		"source_file, source_line) "
		"SELECT ?, ?, ?, ?, ?, ? "
		"WHERE NOT EXISTS ("
		"  SELECT 1 FROM contract "
		"  WHERE project_id=? AND name=? AND origin=? "
		"    AND source_file=? AND source_line=?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		error_ = "[module=store, method=insertContract] "
			 "prepare failed";
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, origin.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 4, claim_text.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 5, source_file.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 6, source_line);
	sqlite3_bind_int64(stmt, 7, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 8, name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 9, origin.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 10, source_file.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 11, source_line);

	int rc = sqlite3_step(stmt);
	sqlite3_reset(stmt);
	if (rc != SQLITE_DONE) {
		error_ = std::string("[module=store, method=insertContract] "
				     "step failed: ") +
			 sqlite3_errmsg(db_);
		fprintf(stderr,
			"insertContract: step failed (rc=%d): %s "
			"[module=store, method=insertContract]\n",
			rc, sqlite3_errmsg(db_));
		return false;
	}
	return true;
}

// ── claim ───────────────────────────────────────────────────────────

int64_t GraphStore::insertClaim(uint64_t project_id, const verify::Claim &claim)
{
	const char *sql =
		"INSERT INTO claim (project_id, claim_type, subject, predicate, "
		"object, scope, source_kind, source_ref) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		return -1;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 2,
			 static_cast<int>(static_cast<uint8_t>(claim.type)));
	sqlite3_bind_text(stmt, 3, claim.subject.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 4, claim.predicate.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 5, claim.object.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 6, claim.scope.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 7, claim.source_kind.c_str(), -1,
			  SQLITE_STATIC);
	sqlite3_bind_text(stmt, 8, claim.source_ref.c_str(), -1, SQLITE_STATIC);

	int rc = sqlite3_step(stmt);
	// Reset before returning so SQLITE_STATIC bindings (which point into
	// the caller's std::string) are released here, not at the next cache
	// reuse — the strings die with this frame.
	sqlite3_reset(stmt);
	if (rc != SQLITE_DONE) {
		error_ = std::string("insertClaim: step failed: ") +
			 sqlite3_errmsg(db_);
		fprintf(stderr,
			"insertClaim: step failed (rc=%d): %s "
			"[module=store, method=insertClaim]\n",
			rc, sqlite3_errmsg(db_));
		return -1;
	}
	return static_cast<int64_t>(sqlite3_last_insert_rowid(db_));
}

// ── evidence ───────────────────────────────────────────────────────

int64_t GraphStore::insertEvidence(int64_t claim_id, verify::Verdict verdict,
				   double confidence,
				   const std::string &verifier_name,
				   const std::string &detail)
{
	const char *sql =
		"INSERT INTO evidence (claim_id, verdict, confidence, "
		"verifier_name, detail) VALUES (?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		return -1;
	}
	sqlite3_bind_int64(stmt, 1, claim_id);
	sqlite3_bind_int(stmt, 2,
			 static_cast<int>(static_cast<uint8_t>(verdict)));
	sqlite3_bind_double(stmt, 3, confidence);
	sqlite3_bind_text(stmt, 4, verifier_name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 5, detail.c_str(), -1, SQLITE_STATIC);

	int rc = sqlite3_step(stmt);
	sqlite3_reset(stmt);
	if (rc != SQLITE_DONE) {
		error_ = std::string("insertEvidence: step failed: ") +
			 sqlite3_errmsg(db_);
		fprintf(stderr,
			"insertEvidence: step failed (rc=%d): %s "
			"[module=store, method=insertEvidence]\n",
			rc, sqlite3_errmsg(db_));
		return -1;
	}
	return static_cast<int64_t>(sqlite3_last_insert_rowid(db_));
}

// ── evidence_fact ───────────────────────────────────────────────────

bool GraphStore::insertEvidenceFact(int64_t evidence_id, int fact_kind,
				    int64_t fact_ref, const std::string &detail)
{
	const char *sql = "INSERT OR IGNORE INTO evidence_fact "
			  "(evidence_id, fact_kind, fact_ref, detail) "
			  "VALUES (?, ?, ?, ?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		return false;
	}
	sqlite3_bind_int64(stmt, 1, evidence_id);
	sqlite3_bind_int(stmt, 2, fact_kind);
	sqlite3_bind_int64(stmt, 3, fact_ref);
	sqlite3_bind_text(stmt, 4, detail.c_str(), -1, SQLITE_STATIC);

	int rc = sqlite3_step(stmt);
	sqlite3_reset(stmt);
	if (rc != SQLITE_DONE) {
		error_ = std::string("insertEvidenceFact: step failed: ") +
			 sqlite3_errmsg(db_);
		fprintf(stderr,
			"insertEvidenceFact: step failed (rc=%d): %s "
			"[module=store, method=insertEvidenceFact]\n",
			rc, sqlite3_errmsg(db_));
		return false;
	}
	return true;
}

// ── finding ─────────────────────────────────────────────────────────

int64_t GraphStore::insertFinding(uint64_t project_id, const std::string &rule,
				  int severity, int64_t claim_id,
				  const std::string &description,
				  double confidence)
{
	const char *sql =
		"INSERT INTO finding (project_id, rule, severity, claim_id, "
		"description, confidence) VALUES (?, ?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		return -1;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, rule.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 3, severity);
	// claim_id is nullable: bind NULL when 0 so manual findings have no
	// FK link, otherwise bind the integer.
	if (claim_id > 0)
		sqlite3_bind_int64(stmt, 4, claim_id);
	else
		sqlite3_bind_null(stmt, 4);
	sqlite3_bind_text(stmt, 5, description.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_double(stmt, 6, confidence);

	int rc = sqlite3_step(stmt);
	sqlite3_reset(stmt);
	if (rc != SQLITE_DONE) {
		error_ = std::string("insertFinding: step failed: ") +
			 sqlite3_errmsg(db_);
		fprintf(stderr,
			"insertFinding: step failed (rc=%d): %s "
			"[module=store, method=insertFinding]\n",
			rc, sqlite3_errmsg(db_));
		return -1;
	}
	return static_cast<int64_t>(sqlite3_last_insert_rowid(db_));
}
// ── listCapabilities / listContracts ───────────────────────────────

std::vector<std::pair<int64_t, std::string>>
GraphStore::listCapabilities(uint64_t project_id)
{
	std::vector<std::pair<int64_t, std::string>> out;
	const char *sql = "SELECT id, name FROM capability "
			  "WHERE project_id=? ORDER BY id";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt)
		return out;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int64_t id = sqlite3_column_int64(stmt, 0);
		const char *name = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		out.emplace_back(id, name ? name : "");
	}
	sqlite3_reset(stmt);
	return out;
}

std::vector<std::pair<int64_t, std::string>>
GraphStore::listContracts(uint64_t project_id)
{
	std::vector<std::pair<int64_t, std::string>> out;
	const char *sql = "SELECT id, name FROM contract "
			  "WHERE project_id=? ORDER BY id";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt)
		return out;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int64_t id = sqlite3_column_int64(stmt, 0);
		const char *name = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		out.emplace_back(id, name ? name : "");
	}
	sqlite3_reset(stmt);
	return out;
}

// ── document ──────────────────────────────────────────────────────

bool GraphStore::insertDocument(uint64_t project_id, int type,
				const std::string &file_path,
				const std::string &content, int start_line,
				int end_line)
{
	const char *sql = "INSERT INTO document "
			  "(project_id, type, file_path, content, "
			  " start_line, end_line) "
			  "VALUES (?,?,?,?,?,?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt)
		return false;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 2, type);
	sqlite3_bind_text(stmt, 3, file_path.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 4, content.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 5, start_line);
	sqlite3_bind_int(stmt, 6, end_line);
	int rc = sqlite3_step(stmt);
	sqlite3_reset(stmt);
	if (rc != SQLITE_DONE) {
		error_ = std::string("insertDocument: step failed: ") +
			 sqlite3_errmsg(db_);
		return false;
	}
	return true;
}

// ── workflow ──────────────────────────────────────────────────────

int64_t GraphStore::insertWorkflow(uint64_t project_id, const std::string &name)
{
	// Idempotent across model rebuilds (same identity as insertContract /
	// insertCapability): return the existing row instead of appending a
	// duplicate on every runModelIndexSync.
	const char *sql =
		"INSERT INTO workflow (project_id, name) "
		"SELECT ?,? WHERE NOT EXISTS ("
		"  SELECT 1 FROM workflow WHERE project_id=? AND name=?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		error_ = "[module=store, method=insertWorkflow] "
			 "prepare failed";
		return -1;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 4, name.c_str(), -1, SQLITE_STATIC);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		sqlite3_reset(stmt);
		error_ = "[module=store, method=insertWorkflow] step failed";
		return -1;
	}
	sqlite3_reset(stmt);
	// Return the (possibly pre-existing) row id so workflow_step rows
	// attach to a single workflow instead of a fresh duplicate.
	sqlite3_stmt *sel = getCachedStmt(
		"SELECT id FROM workflow WHERE project_id=? AND name=?");
	if (!sel) {
		error_ = "[module=store, method=insertWorkflow] "
			 "select failed";
		return -1;
	}
	sqlite3_bind_int64(sel, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(sel, 2, name.c_str(), -1, SQLITE_STATIC);
	int64_t id = -1;
	if (sqlite3_step(sel) == SQLITE_ROW)
		id = sqlite3_column_int64(sel, 0);
	sqlite3_reset(sel);
	return id;
}

bool GraphStore::insertWorkflowStep(int64_t workflow_id, int step_order,
				    int64_t entity_id, const std::string &label)
{
	const char *sql = "INSERT INTO workflow_step "
			  "(workflow_id, step_order, entity_id, label) "
			  "VALUES (?,?,?,?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt)
		return false;
	sqlite3_bind_int64(stmt, 1, workflow_id);
	sqlite3_bind_int(stmt, 2, step_order);
	sqlite3_bind_int64(stmt, 3, entity_id);
	sqlite3_bind_text(stmt, 4, label.c_str(), -1, SQLITE_STATIC);
	int rc = sqlite3_step(stmt);
	sqlite3_reset(stmt);
	if (rc != SQLITE_DONE) {
		error_ = "[module=store, method=insertWorkflowStep] "
			 "step failed";
		return false;
	}
	return true;
}

// ── architecture_edge ─────────────────────────────────────────────

bool GraphStore::insertArchitectureEdge(uint64_t project_id,
					const std::string &caller_module,
					const std::string &callee_module,
					int64_t entity_id)
{
	const char *sql =
		"INSERT INTO architecture_edge "
		"(project_id, caller_module, callee_module, entity_id) "
		"VALUES (?,?,?,?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt)
		return false;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, caller_module.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, callee_module.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 4, entity_id);
	int rc = sqlite3_step(stmt);
	sqlite3_reset(stmt);
	if (rc != SQLITE_DONE) {
		error_ = "[module=store, method=insertArchitectureEdge] "
			 "step failed";
		return false;
	}
	return true;
}

// ── reference ────────────────────────────────────────────────────

int64_t GraphStore::insertReference(uint64_t project_id, uint64_t caller_id,
				    const std::string &name, int64_t scope_id,
				    int arity, int start_row, int start_col,
				    int call_kind)
{
	const char *sql = "INSERT INTO reference "
			  "(project_id, caller_id, name, scope_id, arity, "
			  " start_row, start_col, call_kind) "
			  "VALUES (?,?,?,?,?,?,?,?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt)
		return -1;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(caller_id));
	sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 4, scope_id);
	sqlite3_bind_int(stmt, 5, arity);
	sqlite3_bind_int(stmt, 6, start_row);
	sqlite3_bind_int(stmt, 7, start_col);
	sqlite3_bind_int(stmt, 8, call_kind);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "[module=store, method=insertReference] "
			 "step failed";
		sqlite3_reset(stmt);
		return -1;
	}
	sqlite3_reset(stmt);
	return sqlite3_last_insert_rowid(db_);
}
// `insertResolvedReference` and the `resolved_reference` table are gone: the
// resolver writes relation.confidence + relation.reason instead. Kept as a note
// so it does not get reintroduced.

} // namespace store

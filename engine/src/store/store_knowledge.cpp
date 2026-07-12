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
	const char *sql = "INSERT INTO capability (project_id, name, summary, "
			  "source_kind, source_ref) VALUES (?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, summary.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 4, source_kind.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 5, source_ref.c_str(), -1, SQLITE_STATIC);

	int rc = sqlite3_step(stmt);
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
	const char *sql =
		"INSERT INTO contract (project_id, name, origin, claim_text, "
		"source_file, source_line) VALUES (?, ?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, origin.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 4, claim_text.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 5, source_file.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 6, source_line);

	int rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		error_ = std::string("insertContract: step failed: ") +
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

// ── clearProjectKnowledge ───────────────────────────────────────────
//
// Deletes in reverse FK dependency order so no FK violation can occur:
//   evidence_fact -> evidence -> finding -> claim -> contract -> capability
// evidence_fact and evidence have no project_id column, so they are scoped
// via a subquery on claim.project_id. finding/contract/capability carry
// project_id directly and use a bound parameter.

bool GraphStore::clearProjectKnowledge(uint64_t project_id)
{
	// Each DELETE uses a cached, parameterized statement so repeated calls
	// (e.g. KnowledgeBuilder::build() on re-index) reuse the same plan.
	static const char *const kDeleteSql[] = {
		// 1. evidence_fact (via evidence -> claim.project_id)
		"DELETE FROM evidence_fact WHERE evidence_id IN "
		"(SELECT id FROM evidence WHERE claim_id IN "
		" (SELECT id FROM claim WHERE project_id=?))",
		// 2. evidence (via claim.project_id)
		("DELETE FROM evidence WHERE claim_id IN "
		 " (SELECT id FROM claim WHERE project_id=?)"),
		// 3. finding (has project_id)
		"DELETE FROM finding WHERE project_id=?",
		// 4. claim
		"DELETE FROM claim WHERE project_id=?",
		// 5. contract
		"DELETE FROM contract WHERE project_id=?",
		// 6. capability
		"DELETE FROM capability WHERE project_id=?",
	};

	bool ok = true;
	for (const char *sql : kDeleteSql) {
		sqlite3_stmt *stmt = getCachedStmt(sql);
		if (!stmt) {
			ok = false;
			continue;
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		int rc = sqlite3_step(stmt);
		if (rc != SQLITE_DONE) {
			error_ = std::string("clearProjectKnowledge: ") +
				 sqlite3_errmsg(db_);
			fprintf(stderr,
				"clearProjectKnowledge: delete failed (rc=%d): "
				"%s [module=store, "
				"method=clearProjectKnowledge]\n",
				rc, sqlite3_errmsg(db_));
			ok = false;
		}
	}
	return ok;
}

// ── listCapabilities / listContracts ───────────────────────────────

std::vector<std::pair<int64_t, std::string> >
GraphStore::listCapabilities(uint64_t project_id)
{
	std::vector<std::pair<int64_t, std::string> > out;
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
	return out;
}

std::vector<std::pair<int64_t, std::string> >
GraphStore::listContracts(uint64_t project_id)
{
	std::vector<std::pair<int64_t, std::string> > out;
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
	const char *sql =
		"INSERT INTO workflow (project_id, name) VALUES (?,?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt)
		return -1;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_STATIC);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertWorkflow: step failed";
		return -1;
	}
	return sqlite3_last_insert_rowid(db_);
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
	if (rc != SQLITE_DONE) {
		error_ = "insertWorkflowStep: step failed";
		return false;
	}
	return true;
}

// ── architecture_edge ─────────────────────────────────────────────

bool GraphStore::insertArchitectureEdge(uint64_t project_id,
					const std::string &layer_upper,
					const std::string &layer_lower,
					int64_t entity_id)
{
	const char *sql = "INSERT INTO architecture_edge "
			  "(project_id, layer_upper, layer_lower, entity_id) "
			  "VALUES (?,?,?,?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt)
		return false;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, layer_upper.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, layer_lower.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 4, entity_id);
	int rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		error_ = "insertArchitectureEdge: step failed";
		return false;
	}
	return true;
}

// ── reference ────────────────────────────────────────────────────

int64_t GraphStore::insertReference(uint64_t project_id, uint64_t caller_id,
				    const std::string &name, int64_t scope_id,
				    int arity, int start_row, int start_col)
{
	const char *sql = "INSERT INTO reference "
			  "(project_id, caller_id, name, scope_id, arity, "
			  " start_row, start_col) "
			  "VALUES (?,?,?,?,?,?,?)";
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
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertReference: step failed";
		return -1;
	}
	return sqlite3_last_insert_rowid(db_);
}

// ── scope ────────────────────────────────────────────────────────

int64_t GraphStore::insertScope(uint64_t project_id, int64_t parent_id,
				int kind, const std::string &name,
				int start_row, int end_row)
{
	const char *sql =
		"INSERT INTO scope "
		"(project_id, parent_id, kind, name, start_row, end_row) "
		"VALUES (?,?,?,?,?,?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt)
		return -1;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, parent_id);
	sqlite3_bind_int(stmt, 3, kind);
	sqlite3_bind_text(stmt, 4, name.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 5, start_row);
	sqlite3_bind_int(stmt, 6, end_row);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertScope: step failed";
		return -1;
	}
	return sqlite3_last_insert_rowid(db_);
}

// ── import ───────────────────────────────────────────────────────

int64_t GraphStore::insertImport(uint64_t project_id, int64_t source_scope_id,
				 const std::string &target_path,
				 const std::string &alias, int is_pub)
{
	const char *sql =
		"INSERT INTO import "
		"(project_id, source_scope_id, target_path, alias, is_pub) "
		"VALUES (?,?,?,?,?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt)
		return -1;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, source_scope_id);
	sqlite3_bind_text(stmt, 3, target_path.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 4, alias.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 5, is_pub);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertImport: step failed";
		return -1;
	}
	return sqlite3_last_insert_rowid(db_);
}

// ── resolved_reference ──────────────────────────────────────────

// insertResolvedReference has been removed.
// The resolved_reference table was replaced by relation.confidence + reason.

} // namespace store

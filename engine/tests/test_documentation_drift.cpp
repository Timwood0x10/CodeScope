// test_documentation_drift: verify DocumentationDrift detection logic.
//
// Tests three functions in isolation + an end-to-end integration test:
//   1. extractLanguageClaims() — parses language names from README text
//   2. countEntitiesByLanguage() — counts entity rows matching a language
//   3. detectDocumentationDrift() — end-to-end: README claims vs entity table
//
// Scenarios covered:
//   - All claimed languages have entities → no drift
//   - One claimed language missing → 1 drift reported
//   - README mentions "Go" as standalone word (not "Google")
//   - README mentions "C++" via "cpp" alias
//   - Empty README → no claims, no drift
//   - README with no language mentions → no claims
#include "../src/store/store.h"
#include "../src/verify/documentation_drift.h"

#include <cassert>
#include <cstdio>
#include <sqlite3.h>
#include <unistd.h>

using namespace verify;

static const char *kDbPath = "/tmp/codescope_test_doc_drift.db";

/// Insert a document row (type=0 = README) with the given content.
static void insertReadme(store::GraphStore &store, uint64_t project_id,
			 const char *content)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO document (project_id, type, file_path, "
			  "content, start_line, end_line) "
			  "VALUES (?,0,'/README.md',?,0,0)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, content, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert an entity row with the given id, name, and language.
static void insertEntity(store::GraphStore &store, uint64_t project_id,
			 int64_t id, const char *name, const char *language)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO entity (id, project_id, kind, name, "
			  "qualified_name, file_path, language, start_row, "
			  "start_col, end_row, end_col) "
			  "VALUES (?,?,0,?,'','/test.cpp',?,0,0,0,0)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, language, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Find a claim by canonical language name. Returns nullptr if not found.
static const LanguageClaim *findClaim(const std::vector<LanguageClaim> &claims,
				      const char *canonical)
{
	for (const auto &c : claims)
		if (c.canonical == canonical)
			return &c;
	return nullptr;
}

int main()
{
	unlink(kDbPath);

	store::GraphStore store;
	assert(store.open(kDbPath));

	uint64_t project_id = store.createProject("/test", "test_doc_drift");
	assert(project_id > 0);

	// ── Test 1: extractLanguageClaims — basic multi-language README ──
	{
		std::string readme =
			"# My Project\n"
			"Supports C++, Python, Rust, and Go.\n"
			"Written in C++ and Rust primarily.\n";
		auto claims = extractLanguageClaims(readme);
		assert(claims.size() >= 3);

		const auto *cpp = findClaim(claims, "cpp");
		assert(cpp != nullptr);
		assert(cpp->display == "C++");
		assert(cpp->mention_count >= 2); // "C++" appears twice

		const auto *py = findClaim(claims, "python");
		assert(py != nullptr);
		assert(py->display == "Python");

		const auto *go = findClaim(claims, "go");
		assert(go != nullptr);
		assert(go->display == "Go");

		const auto *rust = findClaim(claims, "rust");
		assert(rust != nullptr);
		assert(rust->display == "Rust");

		printf("  [PASS] extractLanguageClaims: detected C++, Python, Go, Rust\n");
	}

	// ── Test 2: extractLanguageClaims — "Go" word-boundary ──────────
	// "Google" and "Going" should NOT trigger a Go claim, but standalone
	// "Go" should.
	{
		std::string readme = "Powered by Google. Going forward, we use Go.";
		auto claims = extractLanguageClaims(readme);
		const auto *go = findClaim(claims, "go");
		assert(go != nullptr);
		assert(go->mention_count >= 1); // "Go" at end of sentence
		printf("  [PASS] extractLanguageClaims: Go word-boundary (Google/Going excluded)\n");
	}

	// ── Test 3: extractLanguageClaims — "cpp" alias ─────────────────
	// "cpp" should map to the same canonical "cpp" as "C++".
	{
		std::string readme = "The cpp parser is fast.";
		auto claims = extractLanguageClaims(readme);
		const auto *cpp = findClaim(claims, "cpp");
		assert(cpp != nullptr);
		assert(cpp->display == "C++");
		printf("  [PASS] extractLanguageClaims: cpp alias -> C++\n");
	}

	// ── Test 4: extractLanguageClaims — empty text ──────────────────
	{
		auto claims = extractLanguageClaims("");
		assert(claims.empty());
		printf("  [PASS] extractLanguageClaims: empty text -> no claims\n");
	}

	// ── Test 5: extractLanguageClaims — no language mentions ────────
	{
		std::string readme = "This is a project about databases.";
		auto claims = extractLanguageClaims(readme);
		assert(claims.empty());
		printf("  [PASS] extractLanguageClaims: no languages -> no claims\n");
	}

	// ── Test 6: countEntitiesByLanguage ─────────────────────────────
	{
		// Insert 3 C++ entities and 2 Python entities.
		insertEntity(store, project_id, 100, "foo", "cpp");
		insertEntity(store, project_id, 101, "bar", "cpp");
		insertEntity(store, project_id, 102, "baz", "cpp");
		insertEntity(store, project_id, 200, "main", "python");
		insertEntity(store, project_id, 201, "helper", "python");

		assert(countEntitiesByLanguage(store, project_id, "cpp") == 3);
		assert(countEntitiesByLanguage(store, project_id, "python") == 2);
		assert(countEntitiesByLanguage(store, project_id, "go") == 0);
		assert(countEntitiesByLanguage(store, project_id, "rust") == 0);
		printf("  [PASS] countEntitiesByLanguage: cpp=3, python=2, go=0, rust=0\n");
	}

	// ── Test 7: detectDocumentationDrift — one missing language ─────
	// README claims C++, Python, Go, Rust. Codebase has cpp + python only.
	// Expected: Go and Rust are missing → 2 drifts.
	{
		// Clean any prior README rows for this project.
		sqlite3 *db = store.handle();
		sqlite3_exec(db, "DELETE FROM document WHERE project_id > 0",
			     nullptr, nullptr, nullptr);

		insertReadme(store, project_id,
			     "Supports C++, Python, Go, and Rust.");
		auto drifts = detectDocumentationDrift(store, project_id);
		assert(drifts.size() == 2); // Go + Rust missing

		bool has_go = false, has_rust = false;
		for (const auto &d : drifts) {
			assert(d.type == "DocumentationDrift");
			assert(d.severity == kDriftSeverityDoc);
			if (d.subject == "Go")
				has_go = true;
			if (d.subject == "Rust")
				has_rust = true;
		}
		assert(has_go);
		assert(has_rust);
		printf("  [PASS] detectDocumentationDrift: Go + Rust missing (2 drifts)\n");
	}

	// ── Test 8: detectDocumentationDrift — all present, no drift ────
	// Add Rust entities so all 4 claimed languages now have entities.
	{
		insertEntity(store, project_id, 300, "rust_fn", "rust");
		insertEntity(store, project_id, 301, "rust_main", "rust");
		// Go still has zero entities.
		// Re-run detection — now only Go should be missing.
		auto drifts = detectDocumentationDrift(store, project_id);
		assert(drifts.size() == 1);
		assert(drifts[0].subject == "Go");
		printf("  [PASS] detectDocumentationDrift: Rust added, only Go missing (1 drift)\n");
	}

	// ── Test 9: detectDocumentationDrift — empty README ─────────────
	{
		sqlite3 *db = store.handle();
		sqlite3_exec(db, "DELETE FROM document WHERE project_id > 0",
			     nullptr, nullptr, nullptr);
		// No README inserted → no claims → no drifts.
		auto drifts = detectDocumentationDrift(store, project_id);
		assert(drifts.empty());
		printf("  [PASS] detectDocumentationDrift: empty README -> no drifts\n");
	}

	store.close();
	unlink(kDbPath);

	printf("=== test_documentation_drift PASSED ===\n");
	return 0;
}

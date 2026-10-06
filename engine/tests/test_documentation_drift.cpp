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

#include "test_check.h"
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
	CHECK(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, content, -1, SQLITE_TRANSIENT);
	CHECK(sqlite3_step(stmt) == SQLITE_DONE);
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
	CHECK(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, language, -1, SQLITE_TRANSIENT);
	CHECK(sqlite3_step(stmt) == SQLITE_DONE);
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
	CHECK(store.open(kDbPath));

	uint64_t project_id = store.createProject("/test", "test_doc_drift");
	CHECK(project_id > 0);

	// ── Test 1: extractLanguageClaims — capability vs project claim ──
	// "Supports X" says the tool can handle X. Only "Written in X" claims this
	// project is written in X. Counting both made the detector report every
	// language the tool parses as documented-but-absent from its own code.
	{
		std::string readme = "# My Project\n"
				     "Supports C++, Python, Rust, and Go.\n"
				     "Written in C++ and Rust primarily.\n";
		auto claims = extractLanguageClaims(readme);
		CHECK(claims.size() == 2);

		const auto *cpp = findClaim(claims, "cpp");
		CHECK(cpp != nullptr);
		CHECK(cpp->display == "C++");
		// Only the "Written in" mention is a claim; the "Supports" one is a
		// capability statement about the tool.
		CHECK(cpp->mention_count == 1);

		const auto *rust = findClaim(claims, "rust");
		CHECK(rust != nullptr);
		CHECK(rust->display == "Rust");
		CHECK(rust->mention_count == 1);

		// Python and Go are mentioned only as things the tool supports.
		CHECK(findClaim(claims, "python") == nullptr);
		CHECK(findClaim(claims, "go") == nullptr);

		printf("  [PASS] extractLanguageClaims: capability list vetoed, project "
		       "claims (C++, Rust) kept\n");
	}

	// ── Test 2: extractLanguageClaims — "Go" word-boundary ──────────
	// "Google" and "Going" should NOT trigger a Go claim, but standalone
	// "Go" should.
	{
		std::string readme =
			"Powered by Google. Going forward, we use Go.";
		auto claims = extractLanguageClaims(readme);
		const auto *go = findClaim(claims, "go");
		CHECK(go != nullptr);
		CHECK(go->mention_count >= 1); // "Go" at end of sentence
		printf("  [PASS] extractLanguageClaims: Go word-boundary (Google/Going excluded)\n");
	}

	// ── Test 3: extractLanguageClaims — "cpp" alias ─────────────────
	// "cpp" should map to the same canonical "cpp" as "C++".
	{
		std::string readme = "The cpp parser is fast.";
		auto claims = extractLanguageClaims(readme);
		const auto *cpp = findClaim(claims, "cpp");
		CHECK(cpp != nullptr);
		CHECK(cpp->display == "C++");
		printf("  [PASS] extractLanguageClaims: cpp alias -> C++\n");
	}

	// ── Test 4: extractLanguageClaims — empty text ──────────────────
	{
		auto claims = extractLanguageClaims("");
		CHECK(claims.empty());
		printf("  [PASS] extractLanguageClaims: empty text -> no claims\n");
	}

	// ── Test 5: extractLanguageClaims — no language mentions ────────
	{
		std::string readme = "This is a project about databases.";
		auto claims = extractLanguageClaims(readme);
		CHECK(claims.empty());
		printf("  [PASS] extractLanguageClaims: no languages -> no claims\n");
	}

	// ── Test 5a: extractLanguageClaims — a capability matrix is not a claim ──
	// The shape this repository's own README has. Every row of a "Supported
	// Languages" table says the tool can parse that language; none of them says
	// THIS project is written in it.
	{
		std::string readme = "# Tool\n"
				     "### Supported Languages (3)\n"
				     "\n"
				     "| Language | Parser | Verified |\n"
				     "|----------|--------|----------|\n"
				     "| Python | yes | yes |\n"
				     "| Go | yes | yes |\n"
				     "| Rust | yes | yes |\n"
				     "\n"
				     "### Tech Stack\n"
				     "\n"
				     "| Layer | Technology |\n"
				     "|-------|------------|\n"
				     "| Core | C++ |\n";
		auto claims = extractLanguageClaims(readme);
		CHECK(findClaim(claims, "python") == nullptr);
		CHECK(findClaim(claims, "go") == nullptr);
		CHECK(findClaim(claims, "rust") == nullptr);
		// The Tech Stack row is a statement about this project, so it stays.
		CHECK(findClaim(claims, "cpp") != nullptr);
		printf("  [PASS] extractLanguageClaims: capability matrix vetoed, "
		       "tech-stack claim kept\n");
	}

	// ── Test 5b: extractLanguageClaims — a table about other projects ──
	// A benchmark table's `Project` column lists third-party projects, so its
	// Language column describes those projects, not this repository.
	{
		std::string readme = "# Benchmarks\n"
				     "\n"
				     "| Project | Language | Index Time |\n"
				     "|---------|----------|-----------:|\n"
				     "| tinygo | Go | 1.77 s |\n"
				     "| rustc | Rust | 38.9 s |\n";
		auto claims = extractLanguageClaims(readme);
		CHECK(findClaim(claims, "go") == nullptr);
		CHECK(findClaim(claims, "rust") == nullptr);
		printf("  [PASS] extractLanguageClaims: third-party benchmark table "
		       "vetoed\n");
	}

	// ── Test 5c: "parser" in prose is not a capability cue ──────────
	// The capability cues are for COLUMN NAMES and capability phrases. A
	// sentence about this project's own parser must still count, otherwise the
	// veto would be broad enough to hide real drift.
	{
		std::string readme = "The cpp parser is fast.";
		auto claims = extractLanguageClaims(readme);
		const auto *cpp = findClaim(claims, "cpp");
		CHECK(cpp != nullptr);
		CHECK(cpp->mention_count == 1);
		printf("  [PASS] extractLanguageClaims: prose 'parser' keeps the claim\n");
	}

	// ── Test 6: countEntitiesByLanguage ─────────────────────────────
	{
		// Insert 3 C++ entities and 2 Python entities.
		insertEntity(store, project_id, 100, "foo", "cpp");
		insertEntity(store, project_id, 101, "bar", "cpp");
		insertEntity(store, project_id, 102, "baz", "cpp");
		insertEntity(store, project_id, 200, "main", "python");
		insertEntity(store, project_id, 201, "helper", "python");

		CHECK(countEntitiesByLanguage(store, project_id, "cpp") == 3);
		CHECK(countEntitiesByLanguage(store, project_id, "python") ==
		      2);
		CHECK(countEntitiesByLanguage(store, project_id, "go") == 0);
		CHECK(countEntitiesByLanguage(store, project_id, "rust") == 0);
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

		// A project claim, not a capability list: "Written in" binds the
		// languages to this project. (A "Supports …" line would be vetoed and
		// correctly produce zero claims and zero drifts.)
		insertReadme(store, project_id,
			     "Written in C++, Python, Go, and Rust.");
		auto drifts = detectDocumentationDrift(store, project_id);
		CHECK(drifts.size() == 2); // Go + Rust missing

		bool has_go = false, has_rust = false;
		for (const auto &d : drifts) {
			CHECK(d.type == "DocumentationDrift");
			CHECK(d.severity == kDriftSeverityDoc);
			if (d.subject == "Go")
				has_go = true;
			if (d.subject == "Rust")
				has_rust = true;
		}
		CHECK(has_go);
		CHECK(has_rust);
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
		CHECK(drifts.size() == 1);
		CHECK(drifts[0].subject == "Go");
		printf("  [PASS] detectDocumentationDrift: Rust added, only Go missing (1 drift)\n");
	}

	// ── Test 9: detectDocumentationDrift — empty README ─────────────
	{
		sqlite3 *db = store.handle();
		sqlite3_exec(db, "DELETE FROM document WHERE project_id > 0",
			     nullptr, nullptr, nullptr);
		// No README inserted → no claims → no drifts.
		auto drifts = detectDocumentationDrift(store, project_id);
		CHECK(drifts.empty());
		printf("  [PASS] detectDocumentationDrift: empty README -> no drifts\n");
	}

	store.close();
	unlink(kDbPath);

	printf("=== test_documentation_drift PASSED ===\n");
	return checkFailures() ? 1 : 0;
}

// test_readme_ingestion.cpp — verifies the README→document ingestion fix.
//
// Before the fix: .md files were in skip_suffixes_ and never reached
// the indexer. insertDocument() existed but had zero callers, so the
// document table stayed empty → capability table empty → all
// verify_claim calls returned "Contradicted: not declared in knowledge
// layer".
//
// After the fix: engine_index_project intercepts README.md at project
// root BEFORE shouldSkipEntry drops it, reads its content, and calls
// g_store->insertDocument() with type=0 (kDocumentTypeReadme).
//
// This test:
//   1. Creates a temp project dir with a README.md containing
//      "Supports incremental indexing" capability line.
//   2. Calls engine_index_project on that dir.
//   3. Queries the SQLite db directly to assert document table has
//      at least one row with the README content.
//   4. Asserts the capability was extracted (capability table non-empty).
//
// Boundary cases:
//   - Nested README.md (in a subdir) is NOT ingested (only root).
//   - Empty README.md is skipped gracefully.

#include "../include/engine.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <thread>
#include <unistd.h>

static inline void check(bool cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		exit(1);
	}
}

int main()
{
	// Create a temp project dir
	std::string proj_dir = "/tmp/test_readme_ingest";
	std::filesystem::remove_all(proj_dir);
	std::filesystem::create_directories(proj_dir);

	// Write root README.md with capability lines
	std::string readme_path = proj_dir + "/README.md";
	FILE *f = fopen(readme_path.c_str(), "w");
	check(f != nullptr, "fopen README");
	const char *readme = R"(# Test Project

## Capabilities

- Supports incremental indexing
- Supports thread-safe search
- Supports FFI boundary detection

## Architecture

This project demonstrates README ingestion into the knowledge layer.
)";
	fwrite(readme, 1, strlen(readme), f);
	fclose(f);

	// Also write a small source file so indexer has something to do
	std::string src_path = proj_dir + "/main.c";
	f = fopen(src_path.c_str(), "w");
	check(f != nullptr, "fopen main.c");
	const char *src = "int main() { return 0; }";
	fwrite(src, 1, strlen(src), f);
	fclose(f);

	// Init engine
	char db_path[] = "/tmp/test_readme_ingest.db";
	unlink(db_path);
	int rc = engine_init(db_path);
	check(rc == 0, "engine_init");

	uint64_t pid = engine_create_project("/tmp", "readme-test");
	check(pid > 0, "create_project");

	// Index the project — this should ingest README.md
	char *result = engine_index_project(pid, proj_dir.c_str(), nullptr);
	check(result != nullptr, "index_project returns non-null");
	printf("--- index_project result ---\n%s\n", result);
	check(strstr(result, "\"ok\":true") != nullptr,
	      "index_project should succeed");
	engine_free_string(result);

	// Wait for async knowledge builder to finish so capability
	// table is populated before we query it.
	// The async builder runs model plugins (CapabilityPlugin) which
	// read from document table and insert into capability table.
	// We use a short sleep + direct DB query.
	std::this_thread::sleep_for(
		std::chrono::milliseconds(500));

	// Query document table directly via SQLite
	sqlite3 *db = nullptr;
	check(sqlite3_open(db_path, &db) == SQLITE_OK, "sqlite3_open");

	sqlite3_stmt *stmt = nullptr;
	const char *doc_sql = "SELECT COUNT(*) FROM document WHERE project_id = ?";
	check(sqlite3_prepare_v2(db, doc_sql, -1, &stmt, nullptr) == SQLITE_OK,
	      "prepare document count");
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(pid));
	int doc_count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		doc_count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);

	printf("--- document table row count: %d ---\n", doc_count);
	check(doc_count > 0,
	      "document table must have at least 1 row after README ingestion");

	// Verify the content matches what we wrote
	const char *content_sql =
		"SELECT content FROM document WHERE project_id = ? LIMIT 1";
	check(sqlite3_prepare_v2(db, content_sql, -1, &stmt, nullptr) == SQLITE_OK,
	      "prepare content select");
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(pid));
	bool content_matches = false;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *content =
			reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 0));
		if (content)
			content_matches = (strstr(content, "incremental indexing") != nullptr);
	}
	sqlite3_finalize(stmt);
	check(content_matches,
	      "document content must contain the README text");

	// Query capability table — CapabilityPlugin should have extracted
	// capabilities from the README lines matching "Supports ...".
	const char *cap_sql =
		"SELECT COUNT(*) FROM capability WHERE project_id = ?";
	check(sqlite3_prepare_v2(db, cap_sql, -1, &stmt, nullptr) == SQLITE_OK,
	      "prepare capability count");
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(pid));
	int cap_count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		cap_count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);

	printf("--- capability table row count: %d ---\n", cap_count);
	check(cap_count > 0,
	      "capability table must be non-empty after README ingestion + model build");

	sqlite3_close(db);
	engine_shutdown();

	// Cleanup
	std::filesystem::remove_all(proj_dir);
	unlink(db_path);

	printf("\n=== README ingestion test passed ===\n");
	return 0;
}

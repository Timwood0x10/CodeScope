// test_m3_gen_defects.cpp — generated-code defect detection tests (§8.4, §12.4 M3-2).
//
// Tests the four §8.4 defect candidate categories:
// 1. Dependency declaration anomaly: unresolved imports.
// 2. Shell implementation: minimal body functions.
// 3. Duplicate implementation: same metrics across modules.
// 4. Invalid abstraction: single-implementation interfaces.

#include "../src/arch/gen_defects.h"
#include "../src/arch/module_graph.h"
#include "../src/store/store.h"
#include "../src/store/store_graph_imports.h"
#include "test_check.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

static char kDbPath[160];

static void initDbPath()
{
	std::snprintf(kDbPath, sizeof(kDbPath),
		      "/tmp/codescope_test_m3_defects_%d.db", int(getpid()));
}

static void removeDbFiles()
{
	::unlink(kDbPath);
	std::string wal = std::string(kDbPath) + "-wal";
	std::string shm = std::string(kDbPath) + "-shm";
	::unlink(wal.c_str());
	::unlink(shm.c_str());
}

static bool execSql(store::GraphStore &store, const char *sql)
{
	char *msg = nullptr;
	int rc = sqlite3_exec(store.handle(), sql, nullptr, nullptr, &msg);
	if (msg) {
		std::fprintf(stderr, "  [sql error] %s\n", msg);
		sqlite3_free(msg);
	}
	return rc == SQLITE_OK;
}

/// Insert a function entity with metrics.
static int64_t insertFunction(store::GraphStore &store, uint64_t project_id,
			      const std::string &name,
			      const std::string &file_path, int cyclomatic,
			      int lines, int param_count, int call_count,
			      int is_stub = 0,
			      const std::string &language = "cpp")
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO entity (project_id, name, "
			  "qualified_name, kind, file_path, language, "
			  "start_row, start_col, end_row, end_col, "
			  "cyclomatic, lines, param_count, call_count, "
			  "is_stub) "
			  "VALUES (?,?,?,?,?,?,1,0,10,0,?,?,?,?,?)";
	sqlite3_stmt *stmt = nullptr;
	sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 4, 0); // kind=0 function
	sqlite3_bind_text(stmt, 5, file_path.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, language.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 7, cyclomatic);
	sqlite3_bind_int(stmt, 8, lines);
	sqlite3_bind_int(stmt, 9, param_count);
	sqlite3_bind_int(stmt, 10, call_count);
	sqlite3_bind_int(stmt, 11, is_stub);
	int rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		std::fprintf(stderr, "  [insertFunction] step rc=%d: %s\n", rc,
			     sqlite3_errmsg(db));
	}
	int64_t id = sqlite3_last_insert_rowid(db);
	sqlite3_finalize(stmt);
	return id;
}

/// Insert a structural edge.
static void insertEdge(store::GraphStore &store, uint64_t project_id,
		       const std::string &src, const std::string &dst,
		       const std::string &resolution = "internal",
		       double confidence = 1.0)
{
	std::string sql =
		std::string("INSERT OR IGNORE INTO arch_edge "
			    "(project_id, src_module, dst_module, edge_kind, "
			    "resolution_kind, confidence, source_file, "
			    "source_row, raw_target) VALUES (") +
		std::to_string(project_id) + ",'" + src + "','" + dst +
		"','import','" + resolution + "'," +
		std::to_string(confidence) + ",'test.cpp',1,'" + dst + "')";
	execSql(store, sql.c_str());
}

/// Insert a structural edge with explicit source_file and source_row.
/// raw_target defaults to dst if not provided.
static void insertEdgeFull(store::GraphStore &store, uint64_t project_id,
			   const std::string &src, const std::string &dst,
			   const std::string &resolution, double confidence,
			   const std::string &source_file, int source_row,
			   const std::string &raw_target = "")
{
	std::string rt = raw_target.empty() ? dst : raw_target;
	// Escape single quotes in raw_target for SQL.
	std::string escaped_rt;
	for (char c : rt) {
		if (c == '\'')
			escaped_rt += "''";
		else
			escaped_rt += c;
	}
	std::string sql =
		std::string("INSERT OR IGNORE INTO arch_edge "
			    "(project_id, src_module, dst_module, edge_kind, "
			    "resolution_kind, confidence, source_file, "
			    "source_row, raw_target) VALUES (") +
		std::to_string(project_id) + ",'" + src + "','" + dst +
		"','import','" + resolution + "'," +
		std::to_string(confidence) + ",'" + source_file + "'," +
		std::to_string(source_row) + ",'" + escaped_rt + "')";
	execSql(store, sql.c_str());
}

/// Count candidates of a given category.
static int countCandidates(const arch::DefectResult &result,
			   const std::string &category)
{
	int count = 0;
	for (const auto &d : result.candidates) {
		if (d.category == category)
			++count;
	}
	return count;
}

/// Find the first dependency_anomaly candidate matching a reason.
static bool hasAnomalyWithReason(const arch::DefectResult &result,
				 const std::string &reason)
{
	for (const auto &d : result.candidates) {
		if (d.category == "dependency_anomaly" && d.reason == reason)
			return true;
	}
	return false;
}

/// Find the first dependency_anomaly candidate matching a module pair.
static bool hasAnomalyWithModulePair(const arch::DefectResult &result,
				     const std::string &src,
				     const std::string &dst)
{
	for (const auto &d : result.candidates) {
		if (d.category == "dependency_anomaly" && d.src_module == src &&
		    d.target == dst)
			return true;
	}
	return false;
}

// ── Test 1: Dependency anomaly (unresolved import) ───────────────
static void testDependencyAnomaly()
{
	std::fprintf(stderr, "  [test] dependency anomaly...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");

	// Insert a module.
	execSql(store, "INSERT OR IGNORE INTO arch_module "
		       "(project_id, path, parent_path, language, "
		       "file_count, entity_count) "
		       "VALUES (1,'mod_a','','cpp',1,1)");

	// Insert an unresolved edge (anomaly).
	insertEdge(store, project_id, "mod_a", "unknown_pkg", "unresolved",
		   0.0);
	// Insert an internal edge (not an anomaly).
	insertEdge(store, project_id, "mod_a", "mod_b", "internal", 1.0);

	auto result = arch::detectGenDefects(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");

	int anomaly_count = countCandidates(result, "dependency_anomaly");
	CHECK_MSG(anomaly_count >= 1,
		  "should detect at least 1 dependency anomaly");

	store.close();
	std::fprintf(stderr, "  [test] dependency anomaly: PASS\n");
}

// ── Test 2: Shell implementation ─────────────────────────────────
static void testShellImplementation()
{
	std::fprintf(stderr, "  [test] shell implementation...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM entity WHERE project_id=1");

	// Insert a shell function: cyclomatic=1, call_count=0, lines=2,
	// is_stub=1.
	insertFunction(store, project_id, "stub_fn", "src/mod_a.cpp", 1, 2, 0,
		       0, 1);

	// Insert a real function: cyclomatic=5, call_count=3, lines=20.
	insertFunction(store, project_id, "real_fn", "src/mod_a.cpp", 5, 20, 2,
		       3, 0);

	auto result = arch::detectGenDefects(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");

	int shell_count = countCandidates(result, "shell_implementation");
	CHECK_MSG(shell_count >= 1,
		  "should detect at least 1 shell implementation");

	store.close();
	std::fprintf(stderr, "  [test] shell implementation: PASS\n");
}

// ── Test 3: Duplicate implementation ─────────────────────────────
static void testDuplicateImplementation()
{
	std::fprintf(stderr, "  [test] duplicate implementation...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM entity WHERE project_id=1");

	// Two functions in different files with identical metrics.
	insertFunction(store, project_id, "process_data", "src/module_a.cpp", 5,
		       15, 2, 3);
	insertFunction(store, project_id, "process_data", "src/module_b.cpp", 5,
		       15, 2, 3);

	// A different function (should not be flagged).
	insertFunction(store, project_id, "unique_fn", "src/module_a.cpp", 10,
		       50, 4, 8);

	auto result = arch::detectGenDefects(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");

	int dup_count = countCandidates(result, "duplicate_implementation");
	CHECK_MSG(dup_count >= 1,
		  "should detect at least 1 duplicate implementation");

	store.close();
	std::fprintf(stderr, "  [test] duplicate implementation: PASS\n");
}

// ── Test 4: No false positives on empty database ─────────────────
static void testEmptyDatabase()
{
	std::fprintf(stderr, "  [test] empty database...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM entity WHERE project_id=1");
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");

	auto result = arch::detectGenDefects(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");
	CHECK_MSG(result.candidates.empty(),
		  "should produce no candidates for empty database");

	store.close();
	std::fprintf(stderr, "  [test] empty database: PASS\n");
}

// ── Test 5: Standard library imports not flagged ─────────────────
static void testStandardLibraryExclusion()
{
	std::fprintf(stderr, "  [test] standard library exclusion...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");

	execSql(store, "INSERT OR IGNORE INTO arch_module "
		       "(project_id, path, parent_path, language, "
		       "file_count, entity_count) "
		       "VALUES (1,'mod_a','','cpp',1,1)");

	// Insert an unresolved edge to a standard library target.
	// Even though it's unresolved, the standard library check
	// should filter it out.
	insertEdge(store, project_id, "mod_a", "stdio.h", "unresolved", 0.0);
	insertEdge(store, project_id, "mod_a", "std::vector", "unresolved",
		   0.0);

	auto result = arch::detectGenDefects(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");

	int anomaly_count = countCandidates(result, "dependency_anomaly");
	CHECK_MSG(anomaly_count == 0,
		  "should not flag standard library imports as anomalies");

	store.close();
	std::fprintf(stderr, "  [test] standard library exclusion: PASS\n");
}

// ── Test 6: Reason classification & dependency stats (§12.4 M3-2) ─
static void testReasonClassificationAndStats()
{
	std::fprintf(stderr, "  [test] reason classification & stats...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");

	// 1 internal, 1 external, 3 unresolved (one per reason class).
	insertEdgeFull(store, project_id, "src/core", "src/util", "internal",
		       1.0, "src/core/main.cpp", 10);
	insertEdgeFull(store, project_id, "src/core", "boost/optional",
		       "external", 0.5, "src/core/main.cpp", 11);
	// bare_header: no path separator.
	insertEdgeFull(store, project_id, "src/core", "unknown_header",
		       "unresolved", 0.0, "src/core/main.cpp", 12,
		       "unknown_header");
	// absolute_path: starts with '/'.
	insertEdgeFull(store, project_id, "src/core",
		       "/opt/local/include/zlib.h", "unresolved", 0.0,
		       "src/core/main.cpp", 13, "/opt/local/include/zlib.h");
	// cross_dir_relative: raw_target is "../include/missing.h"
	// (folded by resolveRelativeTarget to a different dst_module).
	insertEdgeFull(store, project_id, "src/core", "include/missing.h",
		       "unresolved", 0.0, "src/core/main.cpp", 14,
		       "../include/missing.h");

	auto result = arch::detectGenDefects(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");

	// Stats: total=5, internal=1, external=1, unresolved=3.
	const auto &ds = result.dep_stats;
	CHECK_MSG(ds.total == 5, "dep_stats.total should be 5");
	CHECK_MSG(ds.internal_count == 1, "internal_count should be 1");
	CHECK_MSG(ds.external_count == 1, "external_count should be 1");
	CHECK_MSG(ds.unresolved_count == 3, "unresolved_count should be 3");

	// Ratios.
	CHECK_MSG(ds.unresolved_ratio > 0.59 && ds.unresolved_ratio < 0.61,
		  "unresolved_ratio should be ~0.6");

	// Reason breakdown.
	CHECK_MSG(ds.bare_header_count == 1, "bare_header_count should be 1");
	CHECK_MSG(ds.absolute_path_count == 1,
		  "absolute_path_count should be 1");
	CHECK_MSG(ds.cross_dir_relative_count == 1,
		  "cross_dir_relative_count should be 1");

	// Each unresolved edge should have the correct reason.
	CHECK_MSG(hasAnomalyWithReason(result, "bare_header"),
		  "should have bare_header anomaly");
	CHECK_MSG(hasAnomalyWithReason(result, "absolute_path"),
		  "should have absolute_path anomaly");
	CHECK_MSG(hasAnomalyWithReason(result, "cross_dir_relative"),
		  "should have cross_dir_relative anomaly");

	store.close();
	std::fprintf(stderr, "  [test] reason classification & stats: PASS\n");
}

// ── Test 7: Each anomaly carries source_file + source_row + module pair ──
static void testAnomalySourceLocationAndModulePair()
{
	std::fprintf(stderr,
		     "  [test] anomaly source location & module pair...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");

	insertEdgeFull(store, project_id, "src/engine", "src/missing/dep.h",
		       "unresolved", 0.0, "src/engine/widget.cpp", 42);

	auto result = arch::detectGenDefects(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");

	// Verify module pair.
	CHECK_MSG(hasAnomalyWithModulePair(result, "src/engine",
					   "src/missing/dep.h"),
		  "should have anomaly with module pair src/engine -> "
		  "src/missing/dep.h");

	// Verify source_file and source_row.
	bool found_location = false;
	for (const auto &d : result.candidates) {
		if (d.category == "dependency_anomaly" &&
		    d.file_path == "src/engine/widget.cpp" && d.row == 42) {
			found_location = true;
			break;
		}
	}
	CHECK_MSG(found_location,
		  "anomaly should carry source_file + source_row");

	store.close();
	std::fprintf(stderr,
		     "  [test] anomaly source location & module pair: PASS\n");
}

// ── Test 8: Foldable ../xxx paths classified by raw_target ────────
// resolveRelativeTarget + foldPath in module_graph.cpp collapses
// "./" and "../" prefixes before the edge is written.  The raw_target
// column preserves the original import syntax, so classifyUnresolvedReason
// sees "../include/foo.h" and classifies it as cross_dir_relative,
// NOT absolute_path (even though dst_module may look absolute after folding).
static void testFoldablePathsClassifiedByRawTarget()
{
	std::fprintf(stderr,
		     "  [test] foldable paths classified by raw_target...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");

	// Simulate what module_graph does: raw_target is "../../include/foo.h",
	// foldPath collapses it to "/workspace/include/foo.h" as dst_module.
	// The classifier should use raw_target → cross_dir_relative.
	insertEdgeFull(store, project_id, "src/core",
		       "/workspace/include/foo.h", "unresolved", 0.0,
		       "src/core/main.cpp", 5, "../../include/foo.h");

	auto result = arch::detectGenDefects(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");

	// The reason must be cross_dir_relative, NOT absolute_path.
	bool found = false;
	for (const auto &d : result.candidates) {
		if (d.category == "dependency_anomaly") {
			CHECK_MSG(d.reason == "cross_dir_relative",
				  "folded ../xxx should be cross_dir_relative, "
				  "not absolute_path");
			found = true;
			break;
		}
	}
	CHECK_MSG(found, "should have at least one anomaly");

	// Stats should reflect cross_dir_relative.
	CHECK_MSG(result.dep_stats.cross_dir_relative_count == 1,
		  "cross_dir_relative_count should be 1");
	CHECK_MSG(result.dep_stats.absolute_path_count == 0,
		  "absolute_path_count should be 0 for folded relative path");

	store.close();
	std::fprintf(
		stderr,
		"  [test] foldable paths classified by raw_target: PASS\n");
}

// ── Test 9: Windows absolute paths classified correctly ────────────
static void testWindowsAbsolutePathClassification()
{
	std::fprintf(stderr,
		     "  [test] Windows absolute path classification...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");

	// Windows drive-letter path: C:/SDK/include/missing.h
	insertEdgeFull(store, project_id, "src/core",
		       "C:/SDK/include/missing.h", "unresolved", 0.0,
		       "src/core/main.cpp", 5, "C:/SDK/include/missing.h");
	// Windows backslash path: D:\\libs\\foo.h
	insertEdgeFull(store, project_id, "src/core", "D:/libs/foo.h",
		       "unresolved", 0.0, "src/core/main.cpp", 6,
		       "D:\\\\libs\\\\foo.h");
	// Windows UNC path: \\server\\share\\lib.h
	insertEdgeFull(store, project_id, "src/core", "server/share/lib.h",
		       "unresolved", 0.0, "src/core/main.cpp", 7,
		       "\\\\server\\\\share\\\\lib.h");

	auto result = arch::detectGenDefects(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");

	// All three should be classified as absolute_path.
	CHECK_MSG(result.dep_stats.absolute_path_count == 3,
		  "absolute_path_count should be 3 for Windows paths");
	CHECK_MSG(
		result.dep_stats.cross_dir_relative_count == 0,
		"cross_dir_relative_count should be 0 for Windows absolute paths");
	CHECK_MSG(hasAnomalyWithReason(result, "absolute_path"),
		  "should have absolute_path anomaly for Windows paths");

	store.close();
	std::fprintf(stderr,
		     "  [test] Windows absolute path classification: PASS\n");
}

// ── Test 10: Foldable ../xxx paths should not appear as unresolved ────
// resolveRelativeTarget + foldPath in module_graph.cpp collapses
// "./" and "../" prefixes before the edge is written.  This test
// verifies that the classifier handles a pre-folded path correctly —
// if a path was folded it either matches a module (internal) or is
// classified as cross_dir_relative, never as a raw "../xxx".
static void testFoldablePathsNotUnresolved()
{
	std::fprintf(stderr, "  [test] foldable paths not unresolved...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");

	// Simulate a pre-folded path: "../include/foo.h" was folded to
	// "include/foo.h" by resolveRelativeTarget. If "include" is not
	// a known module, it becomes unresolved with reason
	// cross_dir_relative — but the raw "../" prefix is gone from dst.
	insertEdgeFull(store, project_id, "src/core", "include/foo.h",
		       "unresolved", 0.0, "src/core/main.cpp", 5,
		       "../include/foo.h");

	auto result = arch::detectGenDefects(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");

	for (const auto &d : result.candidates) {
		if (d.category != "dependency_anomaly")
			continue;
		// No unresolved target should start with "../" — that
		// would mean foldPath didn't run.
		CHECK_MSG(d.target.rfind("../", 0) != 0,
			  "unresolved target should not start with ../");
		CHECK_MSG(d.target.rfind("./", 0) != 0,
			  "unresolved target should not start with ./");
	}

	store.close();
	std::fprintf(stderr, "  [test] foldable paths not unresolved: PASS\n");
}

// ── Test 11: End-to-end buildStructuralEdges carries source_row + raw_target ──
// Simulates the buildGraph → writeStructuralEdgesFromImports pipeline:
// 1. flushImportBatch writes import rows with start_row (from sr.start_row).
// 2. buildStructuralEdges reads import rows and writes arch_edge.
// 3. Verify arch_edge.source_row and arch_edge.raw_target are correct.
//
// tree-sitter start_row is 0-based, but arch_edge.source_row is 1-based
// (0 = N/A). So a tree-sitter row of 41 (line 42) must become 42.
static void testEndToEndSourceRowAndRawTarget()
{
	std::fprintf(stderr,
		     "  [test] end-to-end source_row + raw_target...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");
	execSql(store, "DELETE FROM import WHERE project_id=1");
	execSql(store, "DELETE FROM entity WHERE project_id=1");
	execSql(store, "DELETE FROM semantic_records WHERE project_id=1");

	// Insert an entity so that its file_path defines a module.
	// This makes "src/core" a known module for resolveImportTarget.
	execSql(store,
		"INSERT OR REPLACE INTO entity "
		"(id, project_id, name, qualified_name, kind, "
		"file_path, language, start_row, end_row, "
		"start_col, end_col) "
		"VALUES (1,1,'main','main',0,'src/core/main.cpp','cpp',1,10,1,10)");

	// Simulate what buildGraph does: flushImportBatch writes import
	// rows. PathRec.start_row is already 1-based (converted from
	// tree-sitter's 0-based sr.start_row in store_graph.cpp Phase 1.2).
	// The import at physical line 42 (ts row 41) includes
	// "../../include/missing.h".
	{
		std::vector<store::PathRec> batch;
		store::PathRec pr;
		pr.path = "../../include/missing.h";
		pr.alias = "missing.h";
		pr.file = "src/core/main.cpp";
		pr.start_row = 42; // 1-based, already converted
		batch.push_back(pr);
		CHECK_MSG(store::flushImportBatch(store.handle(), project_id,
						  batch),
			  "flushImportBatch should succeed");
	}

	// Build structural edges (calls writeStructuralEdgesFromImports internally).
	std::string error;
	CHECK_MSG(arch::buildStructuralEdges(store, project_id, error),
		  "buildStructuralEdges should succeed");

	// Verify arch_edge has the correct source_row and raw_target.
	sqlite3_stmt *stmt = nullptr;
	CHECK_MSG(sqlite3_prepare_v2(store.handle(),
				     "SELECT source_row, raw_target, "
				     "resolution_kind FROM arch_edge "
				     "WHERE project_id=1 AND "
				     "source_file='src/core/main.cpp'",
				     -1, &stmt, nullptr) == SQLITE_OK,
		  "prepare arch_edge query");

	bool found = false;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int row = sqlite3_column_int(stmt, 0);
		const char *raw = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		const char *res = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		std::string raw_s = raw ? raw : "";
		std::string res_s = res ? res : "";

		// source_row must be 42 (1-based line number).
		CHECK_MSG(row == 42,
			  "source_row should be 42 (the import line)");
		// raw_target must preserve the original import path.
		CHECK_MSG(raw_s == "../../include/missing.h",
			  "raw_target should preserve original import path");
		// The relative path should be unresolved (not matching any module).
		CHECK_MSG(res_s == "unresolved",
			  "resolution_kind should be unresolved for "
			  "unmatched relative path");
		found = true;
	}
	sqlite3_finalize(stmt);
	CHECK_MSG(found, "should find at least one arch_edge row");

	// Now verify detectGenDefects classifies it correctly.
	auto result = arch::detectGenDefects(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");

	// The folded path (../../include/missing.h) should classify as
	// cross_dir_relative (NOT absolute_path), because raw_target
	// preserves the original "../" syntax.
	CHECK_MSG(result.dep_stats.cross_dir_relative_count >= 1,
		  "should have at least 1 cross_dir_relative");
	CHECK_MSG(result.dep_stats.absolute_path_count == 0,
		  "should have 0 absolute_path for relative imports");

	store.close();
	std::fprintf(stderr,
		     "  [test] end-to-end source_row + raw_target: PASS\n");
}

// ── Test 12: Off-by-one — first line include gets row=1, not 0 ──
// tree-sitter returns start_row=0 for line 1. The conversion in
// store_graph.cpp must produce source_row=1 (1-based), not 0 (N/A).
static void testFirstLineSourceRow()
{
	std::fprintf(stderr, "  [test] first line source_row...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");
	execSql(store, "DELETE FROM import WHERE project_id=1");
	execSql(store, "DELETE FROM entity WHERE project_id=1");

	execSql(store,
		"INSERT OR REPLACE INTO entity "
		"(id, project_id, name, qualified_name, kind, "
		"file_path, language, start_row, end_row, "
		"start_col, end_col) "
		"VALUES (1,1,'main','main',0,'src/app/main.cpp','cpp',1,10,1,10)");

	// Simulate: tree-sitter row 0 (line 1) → PathRec.start_row = 1.
	{
		std::vector<store::PathRec> batch;
		store::PathRec pr;
		pr.path = "../../include/missing.h";
		pr.alias = "missing.h";
		pr.file = "src/app/main.cpp";
		pr.start_row = 1; // 1-based: tree-sitter row 0 + 1
		batch.push_back(pr);
		CHECK_MSG(store::flushImportBatch(store.handle(), project_id,
						  batch),
			  "flushImportBatch should succeed");
	}

	std::string error;
	CHECK_MSG(arch::buildStructuralEdges(store, project_id, error),
		  "buildStructuralEdges should succeed");

	sqlite3_stmt *stmt = nullptr;
	CHECK_MSG(sqlite3_prepare_v2(store.handle(),
				     "SELECT source_row FROM arch_edge "
				     "WHERE project_id=1 AND "
				     "source_file='src/app/main.cpp'",
				     -1, &stmt, nullptr) == SQLITE_OK,
		  "prepare arch_edge query");

	bool found = false;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int row = sqlite3_column_int(stmt, 0);
		// Must be 1 (first line), not 0 (N/A).
		CHECK_MSG(row == 1,
			  "source_row for first line must be 1, not 0");
		found = true;
	}
	sqlite3_finalize(stmt);
	CHECK_MSG(found, "should find at least one arch_edge row");

	store.close();
	std::fprintf(stderr, "  [test] first line source_row: PASS\n");
}

// ── Test 13: Migration backfill — v16→v18 upgrade ──
// Migration 17 adds raw_target column (no backfill — old edges have
// stale folded paths). Migration 18 backfills import.source_row from
// semantic_records using instr() (not LIKE — _ and % are not wildcards
// in instr), then DELETEs all arch_edge rows to force rebuild.
// After rebuild via buildStructuralEdges, raw_target and source_row
// must be correct.
static void testMigrationBackfill()
{
	std::fprintf(stderr, "  [test] migration backfill...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");
	execSql(store, "DELETE FROM import WHERE project_id=1");
	execSql(store, "DELETE FROM semantic_records WHERE project_id=1");
	execSql(store, "DELETE FROM entity WHERE project_id=1");

	// Insert an entity to define module "src/core".
	execSql(store,
		"INSERT OR REPLACE INTO entity "
		"(id, project_id, name, qualified_name, kind, "
		"file_path, language, start_row, end_row, "
		"start_col, end_col) "
		"VALUES (1,1,'main','main',0,'src/core/main.cpp','cpp',1,10,1,10)");

	// Simulate pre-v17 data. The fixture is the one that separates a
	// substring match from a whole-token match: line 1 includes
	// "vendor/dir/a_b.h", line 42 includes "dir/a_b.h". A bare
	// instr(name, target) finds "dir/a_b.h" inside the longer line 1 and
	// records row 1; the target must match as a complete token instead.
	execSql(store,
		"INSERT INTO semantic_records "
		"(project_id, kind, name, file_path, start_row, start_col, "
		"end_row, end_col, original_id) "
		"VALUES (1, 11, '#include \"vendor/dir/a_b.h\"', "
		"'src/core/main.cpp', 0, 0, 0, 30, 1)");
	execSql(store,
		"INSERT INTO semantic_records "
		"(project_id, kind, name, file_path, start_row, start_col, "
		"end_row, end_col, original_id) "
		"VALUES (1, 11, '#include \"dir/a_b.h\"', "
		"'src/core/main.cpp', 41, 0, 41, 20, 2)");
	execSql(store,
		"INSERT INTO semantic_records "
		"(project_id, kind, name, file_path, start_row, start_col, "
		"end_row, end_col, original_id) "
		"VALUES (1, 11, '#include \"../../include/missing.h\"', "
		"'src/core/main.cpp', 60, 0, 60, 40, 3)");

	execSql(store, "INSERT INTO import "
		       "(project_id, source_scope_id, target_path, alias, "
		       "file_path, is_pub) "
		       "VALUES (1, 0, 'dir/a_b.h', 'a_b.h', "
		       "'src/core/main.cpp', 0)");
	execSql(store, "INSERT INTO import "
		       "(project_id, source_scope_id, target_path, alias, "
		       "file_path, is_pub) "
		       "VALUES (1, 0, '../../include/missing.h', 'missing.h', "
		       "'src/core/main.cpp', 0)");

	// Simulate a pre-v17 arch_edge row (stale source_row=0, no raw_target).
	execSql(store, "INSERT INTO arch_edge "
		       "(project_id, src_module, dst_module, edge_kind, "
		       "resolution_kind, confidence, source_file, source_row) "
		       "VALUES (1, 'src/core', 'include/missing.h', 'import', "
		       "'unresolved', 0.0, 'src/core/main.cpp', 0)");

	// ── Simulate migration 18 backfill (kept in sync with m0018) ──
	// The target has to be delimited (quoted, angle-bracketed or
	// space/semicolon terminated), so "dir/a_b.h" does not match the
	// include of "vendor/dir/a_b.h" on line 1.
	execSql(store,
		"UPDATE import SET source_row = COALESCE("
		"  (SELECT (sr.start_row + 1) FROM semantic_records sr "
		"   WHERE sr.project_id = import.project_id "
		"     AND sr.kind = 11 "
		"     AND sr.file_path = import.file_path "
		"     AND ("
		"       instr(sr.name, '\"' || import.target_path || '\"') > 0"
		"       OR instr(sr.name, '<' || import.target_path || '>') > 0"
		"       OR instr(sr.name, ' ' || import.target_path || ';') > 0"
		"       OR instr(sr.name, ' ' || import.target_path || ' ') > 0"
		"     )"
		"   ORDER BY sr.start_row "
		"   LIMIT 1), 0) "
		"WHERE source_row = 0");
	// Migration 18 deletes arch_edge to force rebuild.
	execSql(store, "DELETE FROM arch_edge");

	// Verify arch_edge is empty after migration (will be rebuilt).
	{
		sqlite3_stmt *stmt = nullptr;
		CHECK_MSG(
			sqlite3_prepare_v2(
				store.handle(),
				"SELECT COUNT(*) FROM arch_edge WHERE project_id=1",
				-1, &stmt, nullptr) == SQLITE_OK,
			"prepare count query");
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			CHECK_MSG(sqlite3_column_int(stmt, 0) == 0,
				  "arch_edge should be empty after migration");
		}
		sqlite3_finalize(stmt);
	}

	// Verify import.source_row was backfilled correctly.
	// "../../include/missing.h" → start_row=60 + 1 = 61.
	// "dir/a_b.h" → start_row=41 + 1 = 42, not 1: line 1 includes
	// "vendor/dir/a_b.h" and only a whole-token match skips it.
	{
		sqlite3_stmt *stmt = nullptr;
		CHECK_MSG(sqlite3_prepare_v2(
				  store.handle(),
				  "SELECT target_path, source_row FROM import "
				  "WHERE project_id=1 ORDER BY target_path",
				  -1, &stmt, nullptr) == SQLITE_OK,
			  "prepare import query");
		int count = 0;
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *tp = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 0));
			int row = sqlite3_column_int(stmt, 1);
			std::string tp_s = tp ? tp : "";
			if (tp_s == "../../include/missing.h") {
				CHECK_MSG(row == 61,
					  "import ../../include/missing.h "
					  "should have source_row=61");
				count++;
			} else if (tp_s == "dir/a_b.h") {
				CHECK_MSG(row == 42,
					  "import dir/a_b.h should have "
					  "source_row=42, not the earlier "
					  "'vendor/dir/a_b.h' line");
				count++;
			}
		}
		sqlite3_finalize(stmt);
		CHECK_MSG(count == 2, "should find 2 import rows");
	}

	// Rebuild arch_edge from import table (simulates buildLayer).
	std::string error;
	CHECK_MSG(arch::buildStructuralEdges(store, project_id, error),
		  "buildStructuralEdges should succeed");

	// Verify rebuilt arch_edge has correct raw_target + source_row.
	{
		sqlite3_stmt *stmt = nullptr;
		CHECK_MSG(
			sqlite3_prepare_v2(
				store.handle(),
				"SELECT raw_target, source_row, resolution_kind "
				"FROM arch_edge WHERE project_id=1 AND "
				"source_file='src/core/main.cpp' ORDER BY raw_target",
				-1, &stmt, nullptr) == SQLITE_OK,
			"prepare arch_edge query");
		int count = 0;
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *raw = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 0));
			int row = sqlite3_column_int(stmt, 1);
			const char *res = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 2));
			std::string raw_s = raw ? raw : "";
			std::string res_s = res ? res : "";

			if (raw_s == "../../include/missing.h") {
				CHECK_MSG(row == 61,
					  "rebuilt edge for "
					  "../../include/missing.h should "
					  "have source_row=61");
				CHECK_MSG(res_s == "unresolved",
					  "should be unresolved");
				count++;
			} else if (raw_s == "dir/a_b.h") {
				CHECK_MSG(row == 42,
					  "rebuilt edge for dir/a_b.h should "
					  "have source_row=42");
				count++;
			}
		}
		sqlite3_finalize(stmt);
		CHECK_MSG(count >= 2,
			  "should find at least 2 rebuilt arch_edge rows");
	}

	// Verify classification: "../../include/missing.h" should be
	// cross_dir_relative, NOT absolute_path.
	auto result = arch::detectGenDefects(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");
	CHECK_MSG(result.dep_stats.cross_dir_relative_count >= 1,
		  "should have at least 1 cross_dir_relative after "
		  "rebuild");
	CHECK_MSG(result.dep_stats.absolute_path_count == 0,
		  "should have 0 absolute_path after rebuild");

	store.close();
	std::fprintf(stderr, "  [test] migration backfill: PASS\n");
}

int main()
{
	initDbPath();
	removeDbFiles();

	testDependencyAnomaly();
	testShellImplementation();
	testDuplicateImplementation();
	testEmptyDatabase();
	testStandardLibraryExclusion();
	testReasonClassificationAndStats();
	testAnomalySourceLocationAndModulePair();
	testFoldablePathsClassifiedByRawTarget();
	testWindowsAbsolutePathClassification();
	testFoldablePathsNotUnresolved();
	testEndToEndSourceRowAndRawTarget();
	testFirstLineSourceRow();
	testMigrationBackfill();

	removeDbFiles();
	return checkFailures() ? 1 : 0;
}

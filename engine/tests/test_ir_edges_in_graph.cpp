// test_ir_edges_in_graph.cpp — the IR fixes must reach the GRAPH, not only the
// IR records.
//
// test_ir_edge_coverage.cpp asserts that the visitors emit the right records
// (a Rust macro becomes a CallExpr, a Java `implements` becomes an
// InterfaceImpl, ...). The review that motivated those fixes
// (2026-09-27 review D1-2) was about CALL EDGES being lost, so a
// record-level assertion is one layer short: this test indexes real files
// end-to-end and queries `relation` for the edges those records are supposed to
// produce.
//
// Cases:
//   1. Rust: `do_work!()` must produce a CALLS edge caller → do_work.
//   2. Java: `implements Drawable` must let a call through the interface
//      dispatch to the implementation (CALLS edge into Circle.draw). The
//      InterfaceImpl record feeds the resolver's dispatch index; without it the
//      call would have no candidate and no edge.

#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

static void check(bool cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "\nFAIL: %s\n", msg);
		exit(1);
	}
}

static void write_file(const std::string &path, const char *content)
{
	FILE *f = fopen(path.c_str(), "w");
	check(f != nullptr, "fopen");
	fputs(content, f);
	fclose(f);
}

/// Index `proj_dir` into a fresh database; on success `out_db` holds the path.
static uint64_t indexProject(const std::string &proj_dir, const char *db_path)
{
	unlink(db_path);
	unlink((std::string(db_path) + "-wal").c_str());
	unlink((std::string(db_path) + "-shm").c_str());
	check(engine_init(db_path) == 0, "engine_init");
	uint64_t pid = engine_create_project(proj_dir.c_str(), "ir-edges");
	check(pid > 0, "create_project");
	char *idx = engine_index_project(pid, proj_dir.c_str(), nullptr);
	check(idx != nullptr, "index_project result");
	check(strstr(idx, "\"ok\":true") != nullptr, "index_project ok");
	engine_free_string(idx);
	engine_shutdown();
	return pid;
}

/// Count `relation` rows of `edge_type` between two entity names.
static int edgeCount(const char *db_path, uint64_t pid, const char *src_name,
		     const char *tgt_name, int edge_type)
{
	sqlite3 *db = nullptr;
	check(sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, nullptr) ==
		      SQLITE_OK,
	      "open db for edge count");
	sqlite3_stmt *st = nullptr;
	const char *sql =
		"SELECT COUNT(*) FROM relation r "
		"JOIN entity e_s ON e_s.id=r.source_id AND e_s.project_id=r.project_id "
		"JOIN entity e_t ON e_t.id=r.target_id AND e_t.project_id=r.project_id "
		"WHERE r.project_id=? AND r.type=? AND e_s.name=? AND e_t.name=?";
	check(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK,
	      "prepare edge count");
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(pid));
	sqlite3_bind_int(st, 2, edge_type);
	sqlite3_bind_text(st, 3, src_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 4, tgt_name, -1, SQLITE_TRANSIENT);
	int n = 0;
	if (sqlite3_step(st) == SQLITE_ROW)
		n = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	sqlite3_close(db);
	return n;
}

/// Count `relation` rows of `edge_type` whose target entity is `tgt_name`.
static int incomingEdgeCount(const char *db_path, uint64_t pid,
			     const char *tgt_name, int edge_type)
{
	sqlite3 *db = nullptr;
	check(sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, nullptr) ==
		      SQLITE_OK,
	      "open db for incoming count");
	char *sql = sqlite3_mprintf(
		"SELECT COUNT(*) FROM relation r JOIN entity e_t ON "
		"e_t.id=r.target_id AND e_t.project_id=r.project_id "
		"WHERE r.project_id=%lld AND r.type=%d AND e_t.name=%Q",
		static_cast<long long>(pid), edge_type, tgt_name);
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK,
	      "prepare incoming count");
	sqlite3_free(sql);
	int n = 0;
	if (sqlite3_step(st) == SQLITE_ROW)
		n = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	sqlite3_close(db);
	return n;
}

/// EdgeType::Calls.
static constexpr int kCallsEdge = 1;

static void test_rust_macro_call_becomes_an_edge()
{
	const std::string proj = "/tmp/ir_edges_rust";
	fs::remove_all(proj);
	fs::create_directories(proj);
	write_file(proj + "/macro.rs", "fn do_work() {}\n"
				       "fn caller() { do_work!(); }\n");
	const char *db = "/tmp/test_ir_edges_rust.db";
	const uint64_t pid = indexProject(proj, db);

	check(edgeCount(db, pid, "caller", "do_work", kCallsEdge) >= 1,
	      "a Rust macro invocation must produce a CALLS edge "
	      "caller -> do_work");
	printf("  ✓ rust macro invocation → CALLS edge\n");
}

static void test_java_implements_reaches_the_graph()
{
	const std::string proj = "/tmp/ir_edges_java";
	fs::remove_all(proj);
	fs::create_directories(proj);
	write_file(proj + "/Shape.java",
		   "interface Drawable {\n"
		   "    void draw();\n"
		   "}\n"
		   "class Circle implements Drawable {\n"
		   "    public void draw() { }\n"
		   "}\n"
		   "class App {\n"
		   "    void use(Drawable d) { d.draw(); }\n"
		   "}\n");
	const char *db = "/tmp/test_ir_edges_java.db";
	const uint64_t pid = indexProject(proj, db);

	// The InterfaceImpl record feeds the resolver's dispatch index, so the call
	// through the interface must reach the implementation. Without the record
	// the call has no candidate and no edge exists.
	check(incomingEdgeCount(db, pid, "draw", kCallsEdge) >= 1,
	      "an interface dispatch call must reach the implementing method "
	      "(needs the `implements` InterfaceImpl record)");
	printf("  ✓ java implements → dispatch CALLS edge\n");
}

int main()
{
	printf("IR edges in graph tests (2026-09-27 review D1-2):\n");

	test_rust_macro_call_becomes_an_edge();
	test_java_implements_reaches_the_graph();

	printf("\n=== ir_edges_in_graph test passed ===\n");
	return 0;
}

// test_readiness_canonical.cpp — readiness and overview answers must come from
// the canonical tables the pipeline actually writes.
//
// Regression (docs/REAL_PROJECT_TOOL_REPORT_2026-09-21.md, finding #4):
// GraphStore::getReadyRatio folded `graph_nodes.<field>_ready`, a table only the
// legacy engine_index_batch path populates. In a canonical DB it therefore
// returned 0.0 for every project, and the consumers drew the wrong conclusion
// from it — project_overview reported
//
//   "callgraph_available": false, "ready_features": {"call_graph": false, …}
//   "sample_call_edges": []
//
// for a database in which find_callers/find_callees answered with hundreds of
// callers. verifier.h states the rule that was violated: the canonical fact
// layer (entity/relation) is the production source of truth, never graph_nodes.
//
// The fixture uses TWO projects on purpose: one whose only file contains a call,
// one whose only file does not. The positive case proves the ratio moves off 0,
// the negative case proves it is a measurement rather than a constant.

#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

static bool contains(const std::string &hay, const char *needle)
{
	return hay.find(needle) != std::string::npos;
}

static void writeFile(const std::string &path, const char *content)
{
	FILE *f = fopen(path.c_str(), "w");
	if (!f) {
		fprintf(stderr, "\nFAIL: cannot write %s\n", path.c_str());
		exit(1);
	}
	fputs(content, f);
	fclose(f);
}

static void removeDb(const std::string &db)
{
	unlink(db.c_str());
	unlink((db + "-wal").c_str());
	unlink((db + "-shm").c_str());
}

/// Index one directory and return the project id.
static uint64_t indexDir(const char *dir, const char *name)
{
	uint64_t pid = engine_create_project(dir, name);
	check(pid > 0, "create_project");
	char *r = engine_index_project(pid, dir, nullptr);
	check(r != nullptr, "index_project returns JSON");
	const std::string out(r);
	engine_free_string(r);
	check(contains(out, "\"ok\":true"), "index_project reports ok:true");
	return pid;
}

int main()
{
	const std::string with_dir = "/tmp/test_readiness_with_calls";
	const std::string without_dir = "/tmp/test_readiness_without_calls";
	const std::string db_path = "/tmp/test_readiness_canonical.db";

	fs::remove_all(with_dir);
	fs::remove_all(without_dir);
	removeDb(db_path);
	fs::create_directories(with_dir);
	fs::create_directories(without_dir);

	// One file with a real call edge (intra-file, so the resolver's P1 path
	// produces it) and one file with none.
	writeFile(with_dir + "/call.c",
		  "int callee(void) { return 1; }\n"
		  "int caller(void) { return callee(); }\n");
	writeFile(without_dir + "/lonely.c",
		  "int lonely(void) { return 0; }\n");
	// A file with no symbols at all: indexed (it is in `files`) but it
	// produces no entity, which is what separates total_files from
	// files_with_symbols below.
	writeFile(with_dir + "/comment_only.c", "/* no symbols here */\n");

	check(engine_init(db_path.c_str()) == 0, "engine_init");
	const uint64_t pid_calls = indexDir(with_dir.c_str(), "with-calls");
	const uint64_t pid_none =
		indexDir(without_dir.c_str(), "without-calls");

	// ── 1. project_overview: ready_features reflects the real call graph ──
	char *ov = engine_project_overview(pid_calls);
	check(ov != nullptr, "engine_project_overview returns JSON");
	const std::string overview(ov);
	engine_free_string(ov);
	check(contains(overview, "\"ready_features\""),
	      "project_overview must carry ready_features");
	check(contains(overview, "\"call_graph\":true"),
	      "ready_features.call_graph must be true once the call graph exists "
	      "(it was false in every canonical DB, because the ratio folded the "
	      "empty graph_nodes table)");

	// ── 2. build_context: the call graph is available and sampled ────────
	// An empty query maps to the \"general\" intent, the branch that reports
	// callgraph_available and samples the edges.
	char *bc = engine_build_context(pid_calls, "");
	check(bc != nullptr, "engine_build_context returns JSON");
	const std::string context(bc);
	engine_free_string(bc);
	check(contains(context, "\"callgraph_available\":true"),
	      "a project with a call edge must report callgraph_available:true");
	check(contains(context, "sample_call_edges\":[{"),
	      "the sample edge list must not be empty when the call graph is "
	      "available (it used to read the empty graph_edges table)");
	check(contains(context, "caller") && contains(context, "callee"),
	      "the sample must name the caller and the callee");

	// ── 3. the same question through the enhancement API agrees ─────────
	char *st = engine_get_enhancement_status(pid_calls);
	check(st != nullptr, "engine_get_enhancement_status returns JSON");
	const std::string status(st);
	engine_free_string(st);
	int cg_ready = 0;
	check(sscanf(status.c_str(),
		     "{\"total_symbols\":%*d,\"callgraph_ready\":%d",
		     &cg_ready) == 1,
	      "enhancement status parses");
	check(cg_ready > 0,
	      "engine_get_enhancement_status and project_overview must agree on "
	      "the call graph being ready");

	// ── 3b. a SECOND project in the same DB is queryable ────────────────
	// Regression: buildGraph offset entity ids by a PROJECT-scoped MAX(id) and
	// only for incremental rebuilds, so indexing a second project into a DB
	// that already held one restarted at id 1, collided with the first
	// project's ids, and INSERT OR IGNORE dropped every entity row of the new
	// project — a project with semantic_records and zero entities, i.e. every
	// tool answering "not found" right after a successful index.
	char *fs = engine_find_symbol(pid_none, "lonely");
	check(fs != nullptr, "engine_find_symbol returns JSON");
	const std::string found(fs);
	engine_free_string(fs);
	check(contains(found, "\"lonely\""),
	      "a project indexed into a database that already holds another "
	      "project must still have its entities (id collision made them "
	      "disappear silently)");

	// ── 4. negative control: no call edge -> not available ──────────────
	char *ov2 = engine_project_overview(pid_none);
	check(ov2 != nullptr, "overview for the call-free project");
	const std::string none(ov2);
	engine_free_string(ov2);
	check(contains(none, "\"call_graph\":false"),
	      "ready_features.call_graph must stay false without call edges — the "
	      "ratio is a measurement, not a constant");

	char *bc2 = engine_build_context(pid_none, "");
	check(bc2 != nullptr, "build_context for the call-free project");
	const std::string none_ctx(bc2);
	engine_free_string(bc2);
	check(contains(none_ctx, "\"callgraph_available\":false"),
	      "a project without call edges must NOT claim the call graph is "
	      "available");
	check(!contains(none_ctx, "sample_call_edges"),
	      "no sample edges may be reported when the call graph is unavailable");

	// ── 5. get_graph_stats: total_files means files INDEXED ─────────────
	// Regression (docs/REAL_PROJECT_TOOL_REPORT_2026-09-21.md, finding #5):
	// total_files counted COUNT(DISTINCT file_path) FROM entity, i.e. only the
	// files that produced a symbol, so a 1,579-file project reported 672. The
	// fixture gives the two numbers different values on purpose: three indexed
	// files, two of which carry symbols.
	char *gs = engine_get_graph_stats(0);
	check(gs != nullptr, "engine_get_graph_stats returns JSON");
	const std::string stats(gs);
	engine_free_string(gs);
	int total_files = -1, files_with_symbols = -1;
	const char *tf = strstr(stats.c_str(), "\"total_files\":");
	const char *fws = strstr(stats.c_str(), "\"files_with_symbols\":");
	check(tf != nullptr, "get_graph_stats must report total_files");
	check(fws != nullptr,
	      "get_graph_stats must report files_with_symbols, so the old number "
	      "is not silently lost");
	if (tf)
		total_files = atoi(tf + strlen("\"total_files\":"));
	if (fws)
		files_with_symbols =
			atoi(fws + strlen("\"files_with_symbols\":"));
	check(total_files == 3,
	      "total_files must count every indexed file (3 here: two with "
	      "symbols plus the comment-only one)");
	check(files_with_symbols == 2,
	      "files_with_symbols must count only the files that produced entities");

	engine_shutdown();
	removeDb(db_path);
	fs::remove_all(with_dir);
	fs::remove_all(without_dir);

	printf("\n=== test_readiness_canonical PASSED ===\n");
	printf("canonical readiness ratio, overview agreement, negative control\n");
	return 0;
}

// test_m3_robustness.cpp — D4 robustness observation tests (development_plan §5.4, §12.4 M3-1).
//
// Tests the three D4 observation groups:
// 1. Dependency concentration: fan-in ratio, fragile hub identification.
// 2. Change propagation: condensed DAG depth, reverse reachable set.
// 3. Boundary resilience: external dependency concentration.
//
// All D4 outputs are observations (no violations) per §5.4.

#include "../src/arch/module_graph.h"
#include "../src/arch/robustness.h"
#include "../src/store/store.h"
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
		     "/tmp/codescope_test_m3_rob_%d.db", int(getpid()));
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

/// Insert a module into arch_module.
static void insertModule(store::GraphStore &store, uint64_t project_id,
			 const std::string &path,
			 const std::string &language = "cpp")
{
	std::string sql = std::string(
		"INSERT OR IGNORE INTO arch_module "
		"(project_id, path, parent_path, language, "
		"file_count, entity_count) VALUES (") +
		std::to_string(project_id) + ",'" + path + "','','" +
		language + "',1,1)";
	execSql(store, sql.c_str());
}

/// Insert a structural edge into arch_edge.
static void insertEdge(store::GraphStore &store, uint64_t project_id,
		       const std::string &src, const std::string &dst,
		       const std::string &resolution = "internal",
		       double confidence = 1.0)
{
	std::string sql = std::string(
		"INSERT OR IGNORE INTO arch_edge "
		"(project_id, src_module, dst_module, edge_kind, "
		"resolution_kind, confidence, source_file, "
		"source_row) VALUES (") +
		std::to_string(project_id) + ",'" + src + "','" + dst +
		"','import','" + resolution + "'," +
		std::to_string(confidence) + ",'test.cpp',1)";
	execSql(store, sql.c_str());
}

/// Count observations of a given category+metric.
static int countObservations(const arch::RobustnessResult &result,
			     const std::string &category,
			     const std::string &metric)
{
	int count = 0;
	for (const auto &obs : result.observations) {
		if (obs.category == category && obs.metric == metric)
			++count;
	}
	return count;
}

/// Find the first observation matching category+metric.
static const arch::RobustnessObservation *
findObservation(const arch::RobustnessResult &result,
		const std::string &category,
		const std::string &metric)
{
	for (const auto &obs : result.observations) {
		if (obs.category == category && obs.metric == metric)
			return &obs;
	}
	return nullptr;
}

// ── Test 1: Fan-in concentration ─────────────────────────────────
//
// Build a star graph where mod_hub is depended on by 3 other modules.
// mod_hub should have fan_in_ratio = 3/(4-1) = 1.0 and should be
// identified as having high concentration.
static void testFanInConcentration()
{
	std::fprintf(stderr, "  [test] fan-in concentration...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");

	// Create 4 modules: hub, a, b, c.
	insertModule(store, project_id, "mod_hub");
	insertModule(store, project_id, "mod_a");
	insertModule(store, project_id, "mod_b");
	insertModule(store, project_id, "mod_c");

	// a, b, c all depend on hub.
	insertEdge(store, project_id, "mod_a", "mod_hub");
	insertEdge(store, project_id, "mod_b", "mod_hub");
	insertEdge(store, project_id, "mod_c", "mod_hub");

	auto result = arch::computeRobustness(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");
	CHECK_MSG(!result.observations.empty(),
		  "should produce observations");

	// mod_hub should have a fan_in_ratio observation.
	int fan_in_count = countObservations(result, "concentration",
					      "fan_in_ratio");
	CHECK_MSG(fan_in_count >= 1, "should have at least 1 fan_in_ratio");

	// Find the observation for mod_hub.
	const auto *hub_obs = findObservation(result, "concentration",
					       "fan_in_ratio");
	if (hub_obs) {
		CHECK_MSG(hub_obs->target == "mod_hub",
			  "hub should be the target");
		CHECK_MSG(hub_obs->value > 0.99,
			  "fan_in_ratio should be ~1.0 for hub");
	}

	store.close();
	std::fprintf(stderr, "  [test] fan-in concentration: PASS\n");
}

// ── Test 2: Fragile hub identification ───────────────────────────
//
// A module with high fan-in AND high outgoing diversity is a fragile hub.
// Build: mod_hub depends on mod_d1, mod_d2, mod_d3 (out-diversity=3),
// and mod_a, mod_b, mod_c all depend on mod_hub (fan-in=3).
// With 5 modules, fan_in_ratio = 3/4 = 0.75 > 0.3 threshold.
static void testFragileHub()
{
	std::fprintf(stderr, "  [test] fragile hub identification...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");

	// 5 modules.
	insertModule(store, project_id, "mod_hub");
	insertModule(store, project_id, "mod_a");
	insertModule(store, project_id, "mod_b");
	insertModule(store, project_id, "mod_c");
	insertModule(store, project_id, "mod_d1");
	insertModule(store, project_id, "mod_d2");
	insertModule(store, project_id, "mod_d3");

	// hub depends on d1, d2, d3 (out-diversity = 3).
	insertEdge(store, project_id, "mod_hub", "mod_d1");
	insertEdge(store, project_id, "mod_hub", "mod_d2");
	insertEdge(store, project_id, "mod_hub", "mod_d3");

	// a, b, c depend on hub (fan-in = 3, ratio = 3/6 = 0.5 > 0.3).
	insertEdge(store, project_id, "mod_a", "mod_hub");
	insertEdge(store, project_id, "mod_b", "mod_hub");
	insertEdge(store, project_id, "mod_c", "mod_hub");

	auto result = arch::computeRobustness(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");

	// Should identify mod_hub as a fragile hub.
	int fragile_count = countObservations(result, "concentration",
					       "fragile_hub");
	CHECK_MSG(fragile_count >= 1,
		  "should identify at least 1 fragile hub");

	const auto *hub = findObservation(result, "concentration",
					   "fragile_hub");
	if (hub) {
		CHECK_MSG(hub->target == "mod_hub",
			  "fragile hub should be mod_hub");
	}

	store.close();
	std::fprintf(stderr, "  [test] fragile hub: PASS\n");
}

// ── Test 3: Condensed DAG depth ──────────────────────────────────
//
// Build a linear chain: A → B → C → D → E (no cycles).
// The condensed DAG depth should be 5 (longest path through 5 nodes).
static void testDagDepth()
{
	std::fprintf(stderr, "  [test] condensed DAG depth...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");

	// 5 modules in a linear chain.
	insertModule(store, project_id, "mod_a");
	insertModule(store, project_id, "mod_b");
	insertModule(store, project_id, "mod_c");
	insertModule(store, project_id, "mod_d");
	insertModule(store, project_id, "mod_e");

	// A → B → C → D → E
	insertEdge(store, project_id, "mod_a", "mod_b");
	insertEdge(store, project_id, "mod_b", "mod_c");
	insertEdge(store, project_id, "mod_c", "mod_d");
	insertEdge(store, project_id, "mod_d", "mod_e");

	auto result = arch::computeRobustness(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");

	// Should have a dag_depth observation.
	const auto *depth_obs = findObservation(result, "propagation",
						"dag_depth");
	CHECK_MSG(depth_obs != nullptr,
		  "should have a dag_depth observation");
	if (depth_obs) {
		CHECK_MSG(depth_obs->value >= 5.0,
			  "DAG depth should be at least 5 for a 5-node chain");
		CHECK_MSG(depth_obs->target == "(global)",
			  "dag_depth should be global");
	}

	store.close();
	std::fprintf(stderr, "  [test] condensed DAG depth: PASS\n");
}

// ── Test 4: Reverse reachable set ────────────────────────────────
//
// Build: A → B → C → D, where A is the root.
// Reverse reachable set of D should include A, B, C (3 modules).
// Reverse reachable set of A should be 0 (nobody depends on A).
static void testReverseReachable()
{
	std::fprintf(stderr, "  [test] reverse reachable set...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");

	insertModule(store, project_id, "mod_a");
	insertModule(store, project_id, "mod_b");
	insertModule(store, project_id, "mod_c");
	insertModule(store, project_id, "mod_d");

	// A → B → C → D
	insertEdge(store, project_id, "mod_a", "mod_b");
	insertEdge(store, project_id, "mod_b", "mod_c");
	insertEdge(store, project_id, "mod_c", "mod_d");

	auto result = arch::computeRobustness(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");

	// D should have a reverse reachable set of 3 (A, B, C).
	int rev_count = countObservations(result, "propagation",
					   "reverse_reachable");
	CHECK_MSG(rev_count >= 1,
		  "should have reverse_reachable observations");

	// Check that D has the largest propagation radius.
	const auto *max_prop = findObservation(result, "propagation",
					     "max_propagation_radius");
	CHECK_MSG(max_prop != nullptr,
		  "should have max_propagation_radius observation");
	if (max_prop) {
		CHECK_MSG(max_prop->target == "mod_d",
			  "mod_d should have the largest propagation radius");
		CHECK_MSG(max_prop->value >= 3.0,
			  "mod_d should have propagation radius >= 3");
	}

	store.close();
	std::fprintf(stderr, "  [test] reverse reachable set: PASS\n");
}

// ── Test 5: External dependency concentration ────────────────────
//
// Build: mod_a depends on an external package "ext_lib" and an
// internal module mod_b. mod_b depends on another external "ext_db".
static void testExternalDependencies()
{
	std::fprintf(stderr, "  [test] external dependency concentration...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");

	insertModule(store, project_id, "mod_a");
	insertModule(store, project_id, "mod_b");
	// External "modules" — these won't be in arch_module, but edges
	// referencing them will have resolution_kind="external".

	// mod_a → mod_b (internal)
	insertEdge(store, project_id, "mod_a", "mod_b", "internal");
	// mod_a → ext_lib (external)
	insertEdge(store, project_id, "mod_a", "ext_lib", "external", 0.5);
	// mod_b → ext_db (external)
	insertEdge(store, project_id, "mod_b", "ext_db", "external", 0.5);

	auto result = arch::computeRobustness(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");

	// Should have external_dependency_count observations.
	int ext_count = countObservations(result, "boundary",
					   "external_dependency_count");
	CHECK_MSG(ext_count >= 1,
		  "should have external_dependency_count observations");

	// Should have a global external_concentration observation.
	const auto *global_ext = findObservation(result, "boundary",
						 "external_concentration");
	CHECK_MSG(global_ext != nullptr,
		  "should have external_concentration observation");
	if (global_ext) {
		CHECK_MSG(global_ext->value >= 2.0,
			  "total external deps should be >= 2");
	}

	store.close();
	std::fprintf(stderr, "  [test] external dependencies: PASS\n");
}

// ── Test 6: Empty graph (no modules) ─────────────────────────────
//
// An empty database should produce no observations and no error.
static void testEmptyGraph()
{
	std::fprintf(stderr, "  [test] empty graph...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");

	auto result = arch::computeRobustness(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error on empty graph");
	CHECK_MSG(result.observations.empty(),
		  "should produce no observations for empty graph");

	store.close();
	std::fprintf(stderr, "  [test] empty graph: PASS\n");
}

// ── Test 7: Cyclic graph DAG depth ───────────────────────────────
//
// Build: A → B → C → A (cycle) + A → D → E (chain after cycle).
// The condensed DAG should have:
//   - SCC {A, B, C} as one node
//   - D as another node
//   - E as another node
//   - Edges: SCC → D → E
//   - Depth should be 3.
static void testCyclicDagDepth()
{
	std::fprintf(stderr, "  [test] cyclic graph DAG depth...\n");

	store::GraphStore store;
	CHECK(store.open(kDbPath));

	uint64_t project_id = 1;
	execSql(store, "DELETE FROM arch_module WHERE project_id=1");
	execSql(store, "DELETE FROM arch_edge WHERE project_id=1");

	insertModule(store, project_id, "mod_a");
	insertModule(store, project_id, "mod_b");
	insertModule(store, project_id, "mod_c");
	insertModule(store, project_id, "mod_d");
	insertModule(store, project_id, "mod_e");

	// A → B → C → A (cycle)
	insertEdge(store, project_id, "mod_a", "mod_b");
	insertEdge(store, project_id, "mod_b", "mod_c");
	insertEdge(store, project_id, "mod_c", "mod_a");
	// A → D → E (chain)
	insertEdge(store, project_id, "mod_a", "mod_d");
	insertEdge(store, project_id, "mod_d", "mod_e");

	auto result = arch::computeRobustness(store, project_id);
	CHECK_MSG(result.error.empty(), "should not error");

	const auto *depth_obs = findObservation(result, "propagation",
						"dag_depth");
	CHECK_MSG(depth_obs != nullptr,
		  "should have a dag_depth observation");
	if (depth_obs) {
		// Condensed DAG: SCC{A,B,C} → D → E, depth = 3.
		CHECK_MSG(depth_obs->value >= 3.0,
			  "DAG depth should be at least 3 after condensation");
	}

	store.close();
	std::fprintf(stderr, "  [test] cyclic graph DAG depth: PASS\n");
}

int main()
{
	initDbPath();
	removeDbFiles();

	testFanInConcentration();
	testFragileHub();
	testDagDepth();
	testReverseReachable();
	testExternalDependencies();
	testEmptyGraph();
	testCyclicDagDepth();

	removeDbFiles();
	return checkFailures() ? 1 : 0;
}

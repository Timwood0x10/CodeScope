// robustness.cpp — D4 robustness observations (development_plan §5.4, §12.4 M3-1).
//
// All D4 outputs are observations in 0.3.x — no violations unless a declared
// threshold exists (§5.4). The function reads the module graph from arch_module
// and arch_edge, computes three groups of metrics:
//
// 1. **Dependency concentration**: fan-in ratio per module, fragile hub
//    identification (modules with high fan-in AND high outgoing edge diversity).
// 2. **Change propagation**: condensed DAG depth (after SCC condensation),
//    reverse reachable set size per module.
// 3. **Boundary resilience**: external dependency concentration (observation
//    only, no runtime evidence).
//
// The condensed DAG is built by:
//   - Running Tarjan's SCC (reusing computeSccs from module_graph.h)
//   - Mapping each original node to its SCC ID
//   - Creating inter-SCC edges (deduplicated)
//   - Computing the longest path (DAG depth) via topological sort + DP

#include "robustness.h"

#include "module_graph.h"
#include "store/store.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <queue>
#include <sqlite3.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace arch
{
namespace
{

/// Threshold for fragile hub identification: a module is a fragile hub
/// if its fan-in exceeds this fraction of all modules AND its outgoing
/// edge diversity (number of distinct dependees) also exceeds this.
/// The value is deliberately conservative — D4 is observation-only.
constexpr double kFragileHubFanInRatio = 0.3;
constexpr int kFragileHubMinOutDegree = 3;

/// Build a mapping from module index → SCC ID.
/// Modules not in any cycle (size-1 SCCs) each get their own unique ID.
/// Returns the number of SCCs (including trivial ones).
int buildSccMapping(const ModuleGraph &graph,
		    const std::vector<SccResult> &cycles,
		    std::vector<int> &node_to_scc)
{
	int n = graph.nodeCount();
	node_to_scc.assign(n, -1);

	// First, assign IDs to cycle members.
	int next_scc_id = 0;
	for (const auto &scc : cycles) {
		for (int member : scc.members) {
			node_to_scc[member] = next_scc_id;
		}
		++next_scc_id;
	}

	// Then, assign unique IDs to non-cycle nodes.
	for (int i = 0; i < n; ++i) {
		if (node_to_scc[i] == -1) {
			node_to_scc[i] = next_scc_id;
			++next_scc_id;
		}
	}

	return next_scc_id;
}

/// Build the condensed DAG: one edge per unique (scc_src, scc_dst) pair
/// where scc_src != scc_dst. Returns adjacency list + in-degree.
void buildCondensedDag(const ModuleGraph &graph,
		       const std::vector<int> &node_to_scc, int scc_count,
		       std::vector<std::vector<int>> &dag_adj,
		       std::vector<int> &dag_in_degree)
{
	dag_adj.assign(scc_count, {});
	dag_in_degree.assign(scc_count, 0);

	std::unordered_set<long long> seen_edges;
	for (int i = 0; i < static_cast<int>(graph.edges().size()); ++i) {
		const ModuleEdge &e = graph.edges()[i];
		int src_scc = node_to_scc[e.src_index];
		int dst_scc = node_to_scc[e.dst_index];
		if (src_scc == dst_scc)
			continue; // Intra-SCC edge, not a DAG edge.

		long long key = (static_cast<long long>(src_scc) << 32) |
				static_cast<unsigned int>(dst_scc);
		if (seen_edges.count(key))
			continue;
		seen_edges.insert(key);

		dag_adj[src_scc].push_back(dst_scc);
		++dag_in_degree[dst_scc];
	}
}

/// Compute the longest path (depth) of the condensed DAG via topological
/// sort + DP. Returns the maximum depth (number of nodes in the longest path).
int computeDagDepth(const std::vector<std::vector<int>> &dag_adj,
		    const std::vector<int> &dag_in_degree, int scc_count)
{
	if (scc_count == 0)
		return 0;

	std::vector<int> depth(scc_count, 1);
	std::vector<int> in_deg = dag_in_degree;

	std::queue<int> q;
	for (int i = 0; i < scc_count; ++i) {
		if (in_deg[i] == 0)
			q.push(i);
	}

	int max_depth = 1;
	int processed = 0;
	while (!q.empty()) {
		int u = q.front();
		q.pop();
		++processed;
		max_depth = std::max(max_depth, depth[u]);
		for (int v : dag_adj[u]) {
			depth[v] = std::max(depth[v], depth[u] + 1);
			if (--in_deg[v] == 0)
				q.push(v);
		}
	}

	// If not all nodes processed, there's a cycle in the DAG — which
	// shouldn't happen after SCC condensation. Report the depth we have.
	(void)processed;
	return max_depth;
}

/// Compute the reverse reachable set size for each module via BFS on
/// the reverse adjacency (following incoming edges = "who depends on me").
/// Returns a vector of set sizes, indexed by module index.
std::vector<int> computeReverseReachable(const ModuleGraph &graph)
{
	int n = graph.nodeCount();
	std::vector<int> sizes(n, 0);

	for (int i = 0; i < n; ++i) {
		// BFS following incoming edges (reverse direction).
		std::unordered_set<int> visited;
		std::vector<int> queue;
		queue.push_back(i);
		visited.insert(i);

		for (size_t qi = 0; qi < queue.size(); ++qi) {
			int u = queue[qi];
			for (int edge_idx : graph.inAdj(u)) {
				int w = graph.edges()[edge_idx].src_index;
				if (visited.insert(w).second)
					queue.push_back(w);
			}
		}
		sizes[i] = static_cast<int>(visited.size()) - 1; // Exclude self
	}

	return sizes;
}

/// Count distinct outgoing dependees for a module (out-degree diversity).
int computeOutDegreeDiversity(const ModuleGraph &graph, int node_idx)
{
	std::unordered_set<int> targets;
	for (int edge_idx : graph.outAdj(node_idx)) {
		targets.insert(graph.edges()[edge_idx].dst_index);
	}
	return static_cast<int>(targets.size());
}

/// Count distinct incoming dependers for a module (fan-in).
int computeFanIn(const ModuleGraph &graph, int node_idx)
{
	std::unordered_set<int> sources;
	for (int edge_idx : graph.inAdj(node_idx)) {
		sources.insert(graph.edges()[edge_idx].src_index);
	}
	return static_cast<int>(sources.size());
}

/// Count distinct outgoing dependees for a module (fan-out).
int computeFanOut(const ModuleGraph &graph, int node_idx)
{
	std::unordered_set<int> targets;
	for (int edge_idx : graph.outAdj(node_idx)) {
		targets.insert(graph.edges()[edge_idx].dst_index);
	}
	return static_cast<int>(targets.size());
}

/// Count external dependencies for a module by querying arch_edge
/// directly. The module graph only contains edges where both endpoints
/// are registered modules, so external edges (to unregistered modules)
/// are not in the graph. We must query the database instead.
/// @param db     The SQLite database handle.
/// @param project_id The project ID.
/// @param module_path The source module path to count external deps for.
/// @return The number of external dependency edges from this module.
int computeExternalDepsFromDb(sqlite3 *db, uint64_t project_id,
			      const std::string &module_path)
{
	const char *sql = "SELECT COUNT(*) FROM arch_edge "
			  "WHERE project_id=? AND src_module=? "
			  "AND resolution_kind='external'";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, module_path.c_str(), -1, SQLITE_TRANSIENT);
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

/// Format a double to a string with 2 decimal places.
std::string fmt(double v)
{
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.2f", v);
	return buf;
}

/// Phase 1: Dependency Concentration — fan-in ratio and fragile hub
/// identification for each module.
void computeConcentration(const ModuleGraph &graph, int n,
			  RobustnessResult &result)
{
	int max_fan_in = 0;
	std::string max_fan_in_module;
	for (int i = 0; i < n; ++i) {
		int fan_in = computeFanIn(graph, i);
		int fan_out = computeFanOut(graph, i);
		int out_div = computeOutDegreeDiversity(graph, i);

		if (fan_in > max_fan_in) {
			max_fan_in = fan_in;
			max_fan_in_module = graph.node(i).path;
		}

		double fan_in_ratio =
			(n > 1) ? static_cast<double>(fan_in) / (n - 1) : 0.0;

		if (fan_in > 0) {
			RobustnessObservation obs;
			obs.category = "concentration";
			obs.metric = "fan_in_ratio";
			obs.value = fan_in_ratio;
			obs.target = graph.node(i).path;
			obs.description =
				"Module '" + graph.node(i).path +
				"' has fan-in " + std::to_string(fan_in) +
				" (" + fmt(fan_in_ratio * 100.0) + "% of " +
				std::to_string(n - 1) + " possible dependers)";
			result.observations.push_back(std::move(obs));
		}

		if (fan_in_ratio >= kFragileHubFanInRatio &&
		    out_div >= kFragileHubMinOutDegree) {
			RobustnessObservation obs;
			obs.category = "concentration";
			obs.metric = "fragile_hub";
			obs.value = fan_in_ratio;
			obs.target = graph.node(i).path;
			obs.description =
				"Module '" + graph.node(i).path +
				"' is a fragile hub: fan-in " +
				std::to_string(fan_in) + " (" +
				fmt(fan_in_ratio * 100.0) +
				"% of modules depend on it) AND outgoing "
				"diversity " +
				std::to_string(out_div) + " distinct dependees";
			result.observations.push_back(std::move(obs));
		}

		(void)fan_out;
	}
	(void)max_fan_in;
	(void)max_fan_in_module;
}

/// Phase 2: Change Propagation — condensed DAG depth and reverse
/// reachable set per module.
void computePropagation(const ModuleGraph &graph, int n,
			RobustnessResult &result)
{
	auto sccs = computeSccs(graph);
	std::vector<int> node_to_scc;
	int scc_count = buildSccMapping(graph, sccs, node_to_scc);

	std::vector<std::vector<int>> dag_adj;
	std::vector<int> dag_in_degree;
	buildCondensedDag(graph, node_to_scc, scc_count, dag_adj,
			  dag_in_degree);

	int dag_depth = computeDagDepth(dag_adj, dag_in_degree, scc_count);

	if (dag_depth > 0) {
		RobustnessObservation obs;
		obs.category = "propagation";
		obs.metric = "dag_depth";
		obs.value = static_cast<double>(dag_depth);
		obs.target = "(global)";
		obs.description =
			"Condensed DAG depth (longest dependency chain after "
			"SCC condensation) is " +
			std::to_string(dag_depth) + " across " +
			std::to_string(scc_count) + " condensed nodes (" +
			std::to_string(sccs.size()) + " cycles detected)";
		result.observations.push_back(std::move(obs));
	}

	auto rev_reach = computeReverseReachable(graph);
	int max_rev_reach = 0;
	std::string max_rev_reach_module;
	for (int i = 0; i < n; ++i) {
		if (rev_reach[i] > 0) {
			RobustnessObservation obs;
			obs.category = "propagation";
			obs.metric = "reverse_reachable";
			obs.value = static_cast<double>(rev_reach[i]);
			obs.target = graph.node(i).path;
			obs.description =
				"Module '" + graph.node(i).path +
				"' has a reverse reachable set of " +
				std::to_string(rev_reach[i]) +
				" modules (change propagation radius)";
			result.observations.push_back(std::move(obs));

			if (rev_reach[i] > max_rev_reach) {
				max_rev_reach = rev_reach[i];
				max_rev_reach_module = graph.node(i).path;
			}
		}
	}

	if (max_rev_reach > 0) {
		RobustnessObservation obs;
		obs.category = "propagation";
		obs.metric = "max_propagation_radius";
		obs.value = static_cast<double>(max_rev_reach);
		obs.target = max_rev_reach_module;
		obs.description =
			"Module '" + max_rev_reach_module +
			"' has the largest change propagation radius: " +
			std::to_string(max_rev_reach) +
			" modules transitively depend on it";
		result.observations.push_back(std::move(obs));
	}
}

/// Phase 3: Boundary Resilience — external dependency concentration.
void computeBoundaryResilience(store::GraphStore &store, uint64_t project_id,
			       const ModuleGraph &graph, int n,
			       RobustnessResult &result)
{
	sqlite3 *db = store.handle();
	int total_external = 0;
	int max_external = 0;
	std::string max_external_module;
	for (int i = 0; i < n; ++i) {
		int ext = computeExternalDepsFromDb(db, project_id,
						    graph.node(i).path);
		if (ext > 0) {
			total_external += ext;

			RobustnessObservation obs;
			obs.category = "boundary";
			obs.metric = "external_dependency_count";
			obs.value = static_cast<double>(ext);
			obs.target = graph.node(i).path;
			obs.description = "Module '" + graph.node(i).path +
					  "' depends on " +
					  std::to_string(ext) +
					  " external packages";
			result.observations.push_back(std::move(obs));

			if (ext > max_external) {
				max_external = ext;
				max_external_module = graph.node(i).path;
			}
		}
	}

	if (total_external > 0) {
		RobustnessObservation obs;
		obs.category = "boundary";
		obs.metric = "external_concentration";
		obs.value = static_cast<double>(total_external);
		obs.target = "(global)";
		obs.description =
			"Total external dependencies across all modules: " +
			std::to_string(total_external) +
			", concentrated in module '" + max_external_module +
			"' (" + std::to_string(max_external) +
			" external deps)";
		result.observations.push_back(std::move(obs));
	}
}

} // namespace

RobustnessResult computeRobustness(store::GraphStore &store,
				   uint64_t project_id)
{
	RobustnessResult result;

	ModuleGraph graph;
	std::string graph_error;
	if (!graph.build(store, project_id, graph_error)) {
		result.error = std::move(graph_error);
		return result;
	}

	if (graph.nodeCount() == 0)
		return result;

	int n = graph.nodeCount();
	computeConcentration(graph, n, result);
	computePropagation(graph, n, result);
	computeBoundaryResilience(store, project_id, graph, n, result);

	return result;
}

} // namespace arch

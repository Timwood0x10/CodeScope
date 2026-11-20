#ifndef CODESCOPE_ARCH_MODULE_GRAPH_H
#define CODESCOPE_ARCH_MODULE_GRAPH_H

#include <cstdint>
#include <string>
#include <vector>

namespace store
{
class GraphStore;
}

namespace arch
{

/// One edge in the module dependency graph.
struct ModuleEdge {
	int src_index;
	int dst_index;
	std::string edge_kind;
	std::string resolution_kind;
	double confidence;
	std::string source_file;
	int source_row;
};

/// A module node in the graph.
struct ModuleNode {
	std::string path;
	std::string language;
	int file_count = 0;
	int entity_count = 0;
};

/// ModuleGraph: an in-memory adjacency representation of module-level
/// structural dependencies (development_plan §9.3).
///
/// The graph is small (tens to hundreds of nodes) and rebuilt each
/// analysis cycle. It uses `vector<vector<int>>` adjacency lists as
/// specified in the plan — not a CSR or a persisted BLOB.
///
/// Edges come from arch_edge (structural edges, not CALLS relations).
/// The graph supports SCC detection, cycle witness extraction, and
/// candidate edge removal simulation.
class ModuleGraph {
    public:
	ModuleGraph() = default;

	/// Build the graph from the arch_edge and arch_module tables for
	/// the given project. Clears any previous state.
	/// @return true on success; false on database error (error set).
	bool build(store::GraphStore &store, uint64_t project_id,
		   std::string &error);

	/// Number of modules (nodes) in the graph.
	int nodeCount() const { return static_cast<int>(nodes_.size()); }

	/// Number of edges.
	int edgeCount() const { return static_cast<int>(edges_.size()); }

	/// Get a module node by index.
	const ModuleNode &node(int index) const { return nodes_[index]; }

	/// Get all edges.
	const std::vector<ModuleEdge> &edges() const { return edges_; }

	/// Get adjacency list (outgoing edges) for a node index.
	const std::vector<int> &outAdj(int index) const;

	/// Get reverse adjacency list (incoming edges) for a node index.
	const std::vector<int> &inAdj(int index) const;

	/// Find the index of a module by path. Returns -1 if not found.
	int indexOf(const std::string &path) const;

    private:
	std::vector<ModuleNode> nodes_;
	std::vector<ModuleEdge> edges_;
	std::vector<std::vector<int>> out_adj_;
	std::vector<std::vector<int>> in_adj_;
	// path → index lookup.
	std::vector<std::pair<std::string, int>> path_index_;
};

/// One strongly connected component (SCC) found in the module graph.
struct SccResult {
	/// Module indices in this SCC.
	std::vector<int> members;
	/// Cycle witness: a path through the SCC that forms a cycle.
	/// Each entry is a module index. The path visits members in order
	/// and returns to the start (so path[0] == path.back() conceptually,
	/// but the witness stores [a, b, c, a] explicitly).
	std::vector<int> witness;
};

/// One candidate edge for breaking an SCC cycle.
struct CandidateEdge {
	int edge_index;
	int src_index;
	int dst_index;
	/// Weight for ranking (lower = better candidate).
	double weight;
	/// After simulating removal of this edge: how many SCCs the original
	/// SCC splits into.
	int resulting_sccs = 0;
	/// After simulating removal: remaining cycle count.
	int remaining_cycles = 0;
};

/// SCC analysis result for the entire graph.
struct SccAnalysis {
	/// All SCCs with size > 1 (actual cycles).
	std::vector<SccResult> cycles;
	/// Candidate edges for each cycle, sorted by weight.
	std::vector<std::vector<CandidateEdge>> candidates_per_cycle;
};

/// Compute SCCs using Tarjan's algorithm (development_plan §9.2).
/// Returns all SCCs with size > 1 (i.e., actual cycles).
std::vector<SccResult> computeSccs(const ModuleGraph &graph);

/// Extract a cycle witness for an SCC: a concrete path that demonstrates
/// the cycle, visitable as file:line through the edge metadata.
std::vector<int> extractCycleWitness(const ModuleGraph &graph,
				     const std::vector<int> &scc_members);

/// Find candidate edges for breaking an SCC, sorted by weight.
/// Each candidate includes the result of simulating its removal:
/// how many SCCs the original splits into and how many cycles remain.
std::vector<CandidateEdge>
findCandidateEdges(const ModuleGraph &graph,
		   const std::vector<int> &scc_members);

/// Build structural edges from import/include facts and persist them
/// to the arch_edge table (development_plan §9.2).
///
/// The chain is:
///   1. Read import rows from the `import` table.
///   2. For each import, resolve the target path to a module:
///      - Internal module: matches an entity's file_path prefix.
///      - External package: does not match any file in the project.
///      - Unresolved: the target path cannot be parsed.
///   3. Write a structural edge to arch_edge with:
///      - edge_kind: "import" (or "include" for C/C++)
///      - resolution_kind: "internal" / "external" / "unresolved"
///      - confidence: 1.0 for internal, 0.5 for external, 0.0 for unresolved
///      - source_file and source_row from the import row
///
/// Also builds arch_module from the file/entity directory structure and
/// populates arch_module_member.
///
/// @return true on success; false on error (error set).
bool buildStructuralEdges(store::GraphStore &store, uint64_t project_id,
			  std::string &error);

} // namespace arch

#endif // CODESCOPE_ARCH_MODULE_GRAPH_H

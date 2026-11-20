// module_graph.cpp — ModuleGraph, structural edges, and SCC analysis.
//
// development_plan §9.2–§9.3: the module graph is an in-memory adjacency
// structure rebuilt each analysis cycle from the arch_edge table. It is
// NOT the entity-level CSR (which only stores CALLS relations). The SCC
// algorithm is Tarjan's, with cycle witness extraction and candidate
// edge removal simulation.
//
// Structural edges are built from the `import` table: each import
// statement is resolved to a target module (internal/external/unresolved)
// and written to arch_edge with its source location.

#include "module_graph.h"

#include "store/store.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace arch
{
namespace
{

/// Normalize a module path: remove trailing slashes, ensure consistent
/// separators.
std::string normalizePath(const std::string &path)
{
	std::string result = path;
	while (!result.empty() && result.back() == '/')
		result.pop_back();
	return result;
}

/// Get the directory portion of a file path (project-relative).
std::string dirOf(const std::string &file_path)
{
	size_t pos = file_path.find_last_of('/');
	if (pos == std::string::npos)
		return "";
	return file_path.substr(0, pos);
}

/// Check if a path starts with a prefix (component-wise).
/// "src/api" matches "src/api" but "src/api2" does not match "src/api".
bool pathStartsWith(const std::string &path, const std::string &prefix)
{
	if (prefix.empty())
		return true;
	if (path.size() < prefix.size())
		return false;
	if (path.substr(0, prefix.size()) != prefix)
		return false;
	if (path.size() == prefix.size())
		return true;
	// The next char must be a path separator.
	return path[prefix.size()] == '/';
}

} // namespace

// ─── ModuleGraph ──────────────────────────────────────────────

bool ModuleGraph::build(store::GraphStore &store, uint64_t project_id,
			std::string &error)
{
	nodes_.clear();
	edges_.clear();
	out_adj_.clear();
	in_adj_.clear();
	path_index_.clear();

	sqlite3 *db = store.handle();
	if (!db) {
		error = "database not open [module=arch, method=ModuleGraph::build]";
		return false;
	}

	// Load modules from arch_module.
	{
		const char *sql =
			"SELECT path, language, file_count, entity_count "
			"FROM arch_module WHERE project_id = ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			error = std::string("prepare arch_module failed: ") +
				sqlite3_errmsg(db);
			return false;
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			ModuleNode node;
			const unsigned char *path =
				sqlite3_column_text(stmt, 0);
			const unsigned char *lang =
				sqlite3_column_text(stmt, 1);
			node.file_count = sqlite3_column_int(stmt, 2);
			node.entity_count = sqlite3_column_int(stmt, 3);
			if (path)
				node.path =
					reinterpret_cast<const char *>(path);
			if (lang)
				node.language =
					reinterpret_cast<const char *>(lang);
			node.path = normalizePath(node.path);
			int idx = static_cast<int>(nodes_.size());
			nodes_.push_back(std::move(node));
			path_index_.push_back({ nodes_.back().path, idx });
		}
		sqlite3_finalize(stmt);
	}

	// If no modules exist, try to derive them from entity file paths.
	// This is a fallback for projects that haven't been indexed with
	// the M2 module builder yet.
	if (nodes_.empty()) {
		const char *sql = "SELECT DISTINCT file_path FROM entity "
				  "WHERE project_id = ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			error = std::string("prepare entity failed: ") +
				sqlite3_errmsg(db);
			return false;
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		std::unordered_set<std::string> seen_dirs;
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *fp = sqlite3_column_text(stmt, 0);
			if (!fp)
				continue;
			std::string file_path =
				reinterpret_cast<const char *>(fp);
			std::string dir = normalizePath(dirOf(file_path));
			if (dir.empty() || seen_dirs.count(dir))
				continue;
			seen_dirs.insert(dir);
			int idx = static_cast<int>(nodes_.size());
			nodes_.push_back({ dir, "", 0, 0 });
			path_index_.push_back({ dir, idx });
		}
		sqlite3_finalize(stmt);
	}

	// Load edges from arch_edge.
	{
		const char *sql =
			"SELECT src_module, dst_module, edge_kind, "
			"resolution_kind, confidence, source_file, "
			"source_row FROM arch_edge WHERE project_id = ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			error = std::string("prepare arch_edge failed: ") +
				sqlite3_errmsg(db);
			return false;
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *src = sqlite3_column_text(stmt, 0);
			const unsigned char *dst = sqlite3_column_text(stmt, 1);
			const unsigned char *kind =
				sqlite3_column_text(stmt, 2);
			const unsigned char *res = sqlite3_column_text(stmt, 3);
			double conf = sqlite3_column_double(stmt, 4);
			const unsigned char *sfile =
				sqlite3_column_text(stmt, 5);
			int srow = sqlite3_column_int(stmt, 6);

			ModuleEdge edge;
			edge.src_index = -1;
			edge.dst_index = -1;
			if (src)
				edge.src_index = indexOf(
					reinterpret_cast<const char *>(src));
			if (dst)
				edge.dst_index = indexOf(
					reinterpret_cast<const char *>(dst));
			if (kind)
				edge.edge_kind =
					reinterpret_cast<const char *>(kind);
			if (res)
				edge.resolution_kind =
					reinterpret_cast<const char *>(res);
			edge.confidence = conf;
			if (sfile)
				edge.source_file =
					reinterpret_cast<const char *>(sfile);
			edge.source_row = srow;

			if (edge.src_index < 0 || edge.dst_index < 0)
				continue;
			edges_.push_back(std::move(edge));
		}
		sqlite3_finalize(stmt);
	}

	// Build adjacency lists.
	out_adj_.resize(nodes_.size());
	in_adj_.resize(nodes_.size());
	for (int i = 0; i < static_cast<int>(edges_.size()); ++i) {
		out_adj_[edges_[i].src_index].push_back(i);
		in_adj_[edges_[i].dst_index].push_back(i);
	}

	return true;
}

const std::vector<int> &ModuleGraph::outAdj(int index) const
{
	static const std::vector<int> empty;
	if (index < 0 || index >= static_cast<int>(out_adj_.size()))
		return empty;
	return out_adj_[index];
}

const std::vector<int> &ModuleGraph::inAdj(int index) const
{
	static const std::vector<int> empty;
	if (index < 0 || index >= static_cast<int>(in_adj_.size()))
		return empty;
	return in_adj_[index];
}

int ModuleGraph::indexOf(const std::string &path) const
{
	std::string norm = normalizePath(path);
	for (const auto &p : path_index_) {
		if (p.first == norm)
			return p.second;
	}
	return -1;
}

// ─── SCC (Tarjan's algorithm) ─────────────────────────────────

namespace
{

/// Tarjan's SCC algorithm implementation.
/// Uses an explicit stack to avoid stack overflow on large graphs.
struct TarjanState {
	const ModuleGraph &graph;
	std::vector<int> index_arr;
	std::vector<int> lowlink;
	std::vector<bool> on_stack;
	std::vector<int> stack;
	int current_index = 0;
	std::vector<SccResult> sccs;

	explicit TarjanState(const ModuleGraph &g)
		: graph(g)
		, index_arr(g.nodeCount(), -1)
		, lowlink(g.nodeCount(), 0)
		, on_stack(g.nodeCount(), false)
	{
	}
};

/// Strongly connected component via iterative Tarjan.
void strongConnect(TarjanState &ts, int v)
{
	// Iterative Tarjan using a work stack.
	struct Frame {
		int node;
		int edge_pos;
	};
	std::vector<Frame> work;
	work.push_back({ v, 0 });

	ts.index_arr[v] = ts.current_index;
	ts.lowlink[v] = ts.current_index;
	++ts.current_index;
	ts.stack.push_back(v);
	ts.on_stack[v] = true;

	while (!work.empty()) {
		Frame &top = work.back();
		int node = top.node;
		const std::vector<int> &adj = ts.graph.outAdj(node);

		if (top.edge_pos < static_cast<int>(adj.size())) {
			int edge_idx = adj[top.edge_pos];
			++top.edge_pos;
			int w = ts.graph.edges()[edge_idx].dst_index;

			if (ts.index_arr[w] == -1) {
				ts.index_arr[w] = ts.current_index;
				ts.lowlink[w] = ts.current_index;
				++ts.current_index;
				ts.stack.push_back(w);
				ts.on_stack[w] = true;
				work.push_back({ w, 0 });
			} else if (ts.on_stack[w]) {
				ts.lowlink[node] = std::min(ts.lowlink[node],
							    ts.index_arr[w]);
			}
		} else {
			// All neighbors processed.
			if (ts.lowlink[node] == ts.index_arr[node]) {
				// Root of an SCC.
				SccResult scc;
				int w;
				do {
					w = ts.stack.back();
					ts.stack.pop_back();
					ts.on_stack[w] = false;
					scc.members.push_back(w);
				} while (w != node);
				if (scc.members.size() > 1) {
					scc.witness = extractCycleWitness(
						ts.graph, scc.members);
					ts.sccs.push_back(std::move(scc));
				}
			}
			work.pop_back();
			if (!work.empty()) {
				int parent = work.back().node;
				ts.lowlink[parent] = std::min(
					ts.lowlink[parent], ts.lowlink[node]);
			}
		}
	}
}

} // namespace

std::vector<SccResult> computeSccs(const ModuleGraph &graph)
{
	TarjanState ts(graph);
	for (int v = 0; v < graph.nodeCount(); ++v) {
		if (ts.index_arr[v] == -1)
			strongConnect(ts, v);
	}
	return ts.sccs;
}

std::vector<int> extractCycleWitness(const ModuleGraph &graph,
				     const std::vector<int> &scc_members)
{
	// BFS to find a cycle within the SCC: start from members[0],
	// find a path back to members[0] through SCC members only.
	if (scc_members.size() < 2)
		return {};

	std::unordered_set<int> member_set(scc_members.begin(),
					   scc_members.end());
	int start = scc_members[0];

	// BFS from start, tracking parent.
	std::vector<int> parent(graph.nodeCount(), -1);
	std::vector<bool> visited(graph.nodeCount(), false);
	std::vector<int> queue;
	queue.push_back(start);
	visited[start] = true;
	bool found = false;
	int found_node = -1;

	for (size_t qi = 0; qi < queue.size() && !found; ++qi) {
		int u = queue[qi];
		for (int edge_idx : graph.outAdj(u)) {
			int w = graph.edges()[edge_idx].dst_index;
			if (member_set.count(w) == 0)
				continue;
			if (w == start && u != start) {
				// Found a cycle back to start.
				parent[w] = u;
				found = true;
				found_node = w;
				break;
			}
			if (!visited[w]) {
				visited[w] = true;
				parent[w] = u;
				queue.push_back(w);
			}
		}
	}

	if (!found)
		return {};

	// Reconstruct the cycle path.
	std::vector<int> path;
	int cur = found_node;
	do {
		path.push_back(cur);
		cur = parent[cur];
	} while (cur != -1 && cur != start);
	path.push_back(start);
	std::reverse(path.begin(), path.end());
	return path;
}

std::vector<CandidateEdge>
findCandidateEdges(const ModuleGraph &graph,
		   const std::vector<int> &scc_members)
{
	std::vector<CandidateEdge> candidates;
	std::unordered_set<int> member_set(scc_members.begin(),
					   scc_members.end());

	// Collect all edges within the SCC.
	for (int i = 0; i < static_cast<int>(graph.edges().size()); ++i) {
		const ModuleEdge &e = graph.edges()[i];
		if (member_set.count(e.src_index) == 0)
			continue;
		if (member_set.count(e.dst_index) == 0)
			continue;

		CandidateEdge ce;
		ce.edge_index = i;
		ce.src_index = e.src_index;
		ce.dst_index = e.dst_index;
		// Weight: prefer edges with lower confidence (less certain
		// = better candidate for investigation).
		ce.weight = e.confidence;

		// Simulate removal: remove this edge and recompute SCCs.
		// Build a subgraph without this edge.
		// For efficiency, we only check if the SCC splits.
		// A simple approach: check if dst is still reachable from src
		// without this edge.
		// Full recompute would be O(V+E) per candidate; for small
		// SCCs this is fine.
		std::unordered_set<int> visited;
		std::vector<int> bfs_queue;
		bfs_queue.push_back(e.src_index);
		visited.insert(e.src_index);
		bool still_reachable = false;
		for (size_t qi = 0; qi < bfs_queue.size(); ++qi) {
			int u = bfs_queue[qi];
			for (int ei : graph.outAdj(u)) {
				if (ei == i)
					continue; // Skip removed edge.
				int w = graph.edges()[ei].dst_index;
				if (member_set.count(w) == 0)
					continue;
				if (w == e.dst_index) {
					still_reachable = true;
					break;
				}
				if (visited.insert(w).second)
					bfs_queue.push_back(w);
			}
			if (still_reachable)
				break;
		}

		ce.resulting_sccs = still_reachable ? 1 : 2;
		ce.remaining_cycles = still_reachable ? 1 : 0;
		candidates.push_back(ce);
	}

	// Sort by: 1) fewer remaining cycles, 2) lower weight.
	std::sort(candidates.begin(), candidates.end(),
		  [](const CandidateEdge &a, const CandidateEdge &b) {
			  if (a.remaining_cycles != b.remaining_cycles)
				  return a.remaining_cycles <
					 b.remaining_cycles;
			  return a.weight < b.weight;
		  });

	return candidates;
}

// ─── Structural edge builder ──────────────────────────────────

bool buildStructuralEdges(store::GraphStore &store, uint64_t project_id,
			  std::string &error)
{
	sqlite3 *db = store.handle();
	if (!db) {
		error = "database not open [module=arch, "
			"method=buildStructuralEdges]";
		return false;
	}

	// Step 1: Build module tree from entity file paths.
	// Each unique directory prefix is a module.
	std::unordered_map<std::string, int> module_file_counts;
	std::unordered_map<std::string, int> module_entity_counts;
	std::unordered_map<std::string, std::string> module_languages;

	{
		const char *sql = "SELECT file_path, language FROM entity "
				  "WHERE project_id = ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			error = std::string("prepare entity failed: ") +
				sqlite3_errmsg(db);
			return false;
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *fp = sqlite3_column_text(stmt, 0);
			const unsigned char *lang =
				sqlite3_column_text(stmt, 1);
			if (!fp)
				continue;
			std::string file_path =
				reinterpret_cast<const char *>(fp);
			std::string language =
				lang ? reinterpret_cast<const char *>(lang) :
				       "";
			std::string dir = normalizePath(dirOf(file_path));
			if (dir.empty())
				continue;
			++module_file_counts[dir];
			++module_entity_counts[dir];
			if (!language.empty())
				module_languages[dir] = language;
		}
		sqlite3_finalize(stmt);
	}

	// Step 2: Clear and write arch_module.
	{
		std::string del =
			"DELETE FROM arch_module WHERE project_id = " +
			std::to_string(project_id);
		if (sqlite3_exec(db, del.c_str(), nullptr, nullptr, nullptr) !=
		    SQLITE_OK) {
			error = "DELETE arch_module failed: " +
				std::string(sqlite3_errmsg(db));
			return false;
		}
	}

	// Collect all module paths for internal resolution.
	std::unordered_set<std::string> known_modules;
	for (const auto &kv : module_file_counts)
		known_modules.insert(kv.first);

	// Write arch_module rows.
	{
		const char *sql =
			"INSERT OR IGNORE INTO arch_module "
			"(project_id, path, parent_path, language, "
			"file_count, entity_count) VALUES (?,?,?,?,?,?)";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			error = "prepare arch_module insert failed: " +
				std::string(sqlite3_errmsg(db));
			return false;
		}
		for (const auto &kv : module_file_counts) {
			const std::string &path = kv.first;
			std::string parent = normalizePath(dirOf(path));
			std::string lang = module_languages.count(path) ?
						   module_languages[path] :
						   "";
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(stmt, 2, path.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, 3, parent.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, 4, lang.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_int(stmt, 5, kv.second);
			sqlite3_bind_int(stmt, 6, module_entity_counts[path]);
			sqlite3_step(stmt);
			sqlite3_reset(stmt);
		}
		sqlite3_finalize(stmt);
	}

	// Step 3: Write arch_module_member (entity → module mapping).
	{
		std::string del =
			"DELETE FROM arch_module_member WHERE project_id = " +
			std::to_string(project_id);
		if (sqlite3_exec(db, del.c_str(), nullptr, nullptr, nullptr) !=
		    SQLITE_OK) {
			error = "DELETE arch_module_member failed: " +
				std::string(sqlite3_errmsg(db));
			return false;
		}
		const char *sql =
			"INSERT OR IGNORE INTO arch_module_member "
			"(project_id, module_path, entity_id) "
			"SELECT ?, ?, e.id FROM entity e "
			"WHERE e.project_id = ? "
			"AND substr(e.file_path, 1, length(?)) = ? "
			"AND (length(e.file_path) = length(?) "
			"     OR substr(e.file_path, length(?) + 1, 1) = '/')";

		// For each module, write members.
		// This is O(modules * entities) which is fine for
		// small-to-medium projects.
		for (const auto &kv : module_file_counts) {
			const std::string &mod_path = kv.first;
			sqlite3_stmt *stmt = nullptr;
			if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) !=
			    SQLITE_OK) {
				error = "prepare arch_module_member failed: " +
					std::string(sqlite3_errmsg(db));
				return false;
			}
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(stmt, 2, mod_path.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_int64(stmt, 3,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(stmt, 4, mod_path.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, 5, mod_path.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, 6, mod_path.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, 7, mod_path.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_step(stmt);
			sqlite3_finalize(stmt);
		}
	}

	// Step 4: Build structural edges from the import table.
	{
		std::string del = "DELETE FROM arch_edge WHERE project_id = " +
				  std::to_string(project_id);
		if (sqlite3_exec(db, del.c_str(), nullptr, nullptr, nullptr) !=
		    SQLITE_OK) {
			error = "DELETE arch_edge failed: " +
				std::string(sqlite3_errmsg(db));
			return false;
		}

		const char *sql =
			"SELECT target_path, file_path, source_scope_id "
			"FROM import WHERE project_id = ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			error = std::string("prepare import failed: ") +
				sqlite3_errmsg(db);
			return false;
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

		// Edge insert statement (reused).
		const char *insert_sql =
			"INSERT OR IGNORE INTO arch_edge "
			"(project_id, src_module, dst_module, "
			"edge_kind, resolution_kind, confidence, "
			"source_file, source_row) "
			"VALUES (?,?,?,?,?,?,?,?)";
		sqlite3_stmt *insert_stmt = nullptr;
		if (sqlite3_prepare_v2(db, insert_sql, -1, &insert_stmt,
				       nullptr) != SQLITE_OK) {
			error = "prepare arch_edge insert failed: " +
				std::string(sqlite3_errmsg(db));
			sqlite3_finalize(stmt);
			return false;
		}

		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const unsigned char *target =
				sqlite3_column_text(stmt, 0);
			const unsigned char *sfile =
				sqlite3_column_text(stmt, 1);
			if (!target || !sfile)
				continue;
			std::string target_path =
				reinterpret_cast<const char *>(target);
			std::string source_file =
				reinterpret_cast<const char *>(sfile);
			std::string src_module =
				normalizePath(dirOf(source_file));

			// Resolve the target: is it an internal module?
			std::string dst_module;
			std::string resolution_kind;
			double confidence = 0.0;

			// Try to match target_path against known modules.
			// The import target may be a path like
			// "crate::module" or "src/api/handler" — try
			// both the full string and path-based matching.
			std::string normalized_target =
				normalizePath(target_path);

			bool found_internal = false;
			for (const auto &mod : known_modules) {
				if (pathStartsWith(normalized_target, mod) ||
				    pathStartsWith(mod, normalized_target)) {
					dst_module = mod;
					resolution_kind = "internal";
					confidence = 1.0;
					found_internal = true;
					break;
				}
			}

			if (!found_internal) {
				// Check if the target looks like an external
				// package (contains :: or is a known crate
				// name pattern).
				if (target_path.find("::") !=
					    std::string::npos ||
				    target_path.find('/') ==
					    std::string::npos) {
					dst_module = normalized_target;
					resolution_kind = "external";
					confidence = 0.5;
				} else {
					dst_module = normalized_target;
					resolution_kind = "unresolved";
					confidence = 0.0;
				}
			}

			if (src_module.empty() || dst_module.empty())
				continue;
			if (src_module == dst_module)
				continue; // Intra-module, not a structural edge.

			sqlite3_bind_int64(insert_stmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(insert_stmt, 2, src_module.c_str(),
					  -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(insert_stmt, 3, dst_module.c_str(),
					  -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(insert_stmt, 4, "import", -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_text(insert_stmt, 5,
					  resolution_kind.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_double(insert_stmt, 6, confidence);
			sqlite3_bind_text(insert_stmt, 7, source_file.c_str(),
					  -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(insert_stmt, 8, 0);
			sqlite3_step(insert_stmt);
			sqlite3_reset(insert_stmt);
		}
		sqlite3_finalize(insert_stmt);
		sqlite3_finalize(stmt);
	}

	return true;
}

} // namespace arch

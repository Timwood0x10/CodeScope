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

/// Collapse `.` and `..` in a slash-separated path.
///
/// The include a C/C++ file writes is relative to that file
/// (`../include/engine.h`), so the raw target never matches a module path;
/// measured on this repository before folding, all 1229 edges were
/// unresolved or external and none was internal.
///
/// @param path Absolute or relative path; leading '/' is preserved.
/// @return The folded path without duplicated or trailing separators.
std::string foldPath(const std::string &path)
{
	const bool absolute = !path.empty() && path[0] == '/';
	std::vector<std::string> parts;
	size_t begin = 0;
	while (begin <= path.size()) {
		const size_t end = path.find('/', begin);
		const std::string seg = path.substr(
			begin, end == std::string::npos ? std::string::npos :
							  end - begin);
		if (seg.empty() || seg == ".") {
			// nothing to add
		} else if (seg == "..") {
			if (!parts.empty())
				parts.pop_back();
		} else {
			parts.push_back(seg);
		}
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}
	std::string out = absolute ? "/" : "";
	for (size_t i = 0; i < parts.size(); ++i) {
		if (i)
			out += '/';
		out += parts[i];
	}
	return out;
}

/// Resolve an include that is relative to the including file.
///
/// Only `./` and `../` are file-relative; a bare `"Vector.h"` is resolved by
/// the compiler's include path, which this layer does not model, so it stays
/// as written and is reported unresolved rather than guessed.
///
/// @param target Import target as recorded.
/// @param source_file File that contains the import.
/// @return Folded path, or the target unchanged when it is not file-relative.
std::string resolveRelativeTarget(const std::string &target,
				  const std::string &source_file)
{
	if (target.empty())
		return target;
	const bool dotted = target.rfind("./", 0) == 0 ||
			    target.rfind("../", 0) == 0;
	if (!dotted)
		return target;
	return foldPath(dirOf(source_file) + "/" + target);
}

} // namespace

// ─── ModuleGraph ──────────────────────────────────────────────

namespace
{

/// Load modules from arch_module into the graph's node list.
/// Returns false on SQL error.
bool loadModules(sqlite3 *db, uint64_t project_id,
		 std::vector<ModuleNode> &nodes,
		 std::vector<std::pair<std::string, int>> &path_index,
		 std::string &error)
{
	const char *sql = "SELECT path, language, file_count, entity_count "
			  "FROM arch_module WHERE project_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error = std::string("prepare arch_module failed: ") +
			sqlite3_errmsg(db);
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		ModuleNode node;
		const unsigned char *path = sqlite3_column_text(stmt, 0);
		const unsigned char *lang = sqlite3_column_text(stmt, 1);
		node.file_count = sqlite3_column_int(stmt, 2);
		node.entity_count = sqlite3_column_int(stmt, 3);
		if (path)
			node.path = reinterpret_cast<const char *>(path);
		if (lang)
			node.language = reinterpret_cast<const char *>(lang);
		node.path = normalizePath(node.path);
		int idx = static_cast<int>(nodes.size());
		nodes.push_back(std::move(node));
		path_index.push_back({ nodes.back().path, idx });
	}
	sqlite3_finalize(stmt);
	return true;
}

/// Fallback: derive modules from entity file paths when arch_module
/// is empty (pre-M2 databases).
bool loadFallbackModules(sqlite3 *db, uint64_t project_id,
			 std::vector<ModuleNode> &nodes,
			 std::vector<std::pair<std::string, int>> &path_index,
			 std::string &error)
{
	const char *sql = "SELECT DISTINCT file_path FROM entity "
			  "WHERE project_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
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
		std::string file_path = reinterpret_cast<const char *>(fp);
		std::string dir = normalizePath(dirOf(file_path));
		if (dir.empty() || seen_dirs.count(dir))
			continue;
		seen_dirs.insert(dir);
		int idx = static_cast<int>(nodes.size());
		nodes.push_back({ dir, "", 0, 0 });
		path_index.push_back({ dir, idx });
	}
	sqlite3_finalize(stmt);
	return true;
}

/// Load edges from arch_edge and resolve them to node indices.
bool loadEdges(sqlite3 *db, uint64_t project_id, const ModuleGraph &graph,
	       std::vector<ModuleEdge> &edges, std::string &error)
{
	const char *sql = "SELECT src_module, dst_module, edge_kind, "
			  "resolution_kind, confidence, source_file, "
			  "source_row FROM arch_edge WHERE project_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error = std::string("prepare arch_edge failed: ") +
			sqlite3_errmsg(db);
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char *src = sqlite3_column_text(stmt, 0);
		const unsigned char *dst = sqlite3_column_text(stmt, 1);
		const unsigned char *kind = sqlite3_column_text(stmt, 2);
		const unsigned char *res = sqlite3_column_text(stmt, 3);
		double conf = sqlite3_column_double(stmt, 4);
		const unsigned char *sfile = sqlite3_column_text(stmt, 5);
		int srow = sqlite3_column_int(stmt, 6);

		ModuleEdge edge;
		edge.src_index = -1;
		edge.dst_index = -1;
		if (src)
			edge.src_index = graph.indexOf(
				reinterpret_cast<const char *>(src));
		if (dst)
			edge.dst_index = graph.indexOf(
				reinterpret_cast<const char *>(dst));
		if (kind)
			edge.edge_kind = reinterpret_cast<const char *>(kind);
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
		edges.push_back(std::move(edge));
	}
	sqlite3_finalize(stmt);
	return true;
}

} // namespace

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

	if (!loadModules(db, project_id, nodes_, path_index_, error))
		return false;

	if (nodes_.empty()) {
		if (!loadFallbackModules(db, project_id, nodes_, path_index_,
					 error))
			return false;
	}

	if (!loadEdges(db, project_id, *this, edges_, error))
		return false;

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

namespace
{

/// Step 1: Build module tree from entity file paths.
/// Each unique directory prefix is a module.
bool collectModuleTree(sqlite3 *db, uint64_t project_id,
		       std::unordered_map<std::string, int> &file_counts,
		       std::unordered_map<std::string, int> &entity_counts,
		       std::unordered_map<std::string, std::string> &languages,
		       std::string &error)
{
	const char *sql = "SELECT file_path, language FROM entity "
			  "WHERE project_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error = std::string("prepare entity failed: ") +
			sqlite3_errmsg(db);
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char *fp = sqlite3_column_text(stmt, 0);
		const unsigned char *lang = sqlite3_column_text(stmt, 1);
		if (!fp)
			continue;
		std::string file_path = reinterpret_cast<const char *>(fp);
		std::string language =
			lang ? reinterpret_cast<const char *>(lang) : "";
		std::string dir = normalizePath(dirOf(file_path));
		if (dir.empty())
			continue;
		++file_counts[dir];
		++entity_counts[dir];
		if (!language.empty())
			languages[dir] = language;
	}
	sqlite3_finalize(stmt);
	return true;
}

/// Step 2: Clear and write arch_module rows.
bool writeArchModules(
	sqlite3 *db, uint64_t project_id,
	const std::unordered_map<std::string, int> &file_counts,
	const std::unordered_map<std::string, int> &entity_counts,
	const std::unordered_map<std::string, std::string> &languages,
	std::string &error)
{
	std::string del = "DELETE FROM arch_module WHERE project_id = " +
			  std::to_string(project_id);
	if (sqlite3_exec(db, del.c_str(), nullptr, nullptr, nullptr) !=
	    SQLITE_OK) {
		error = "DELETE arch_module failed: " +
			std::string(sqlite3_errmsg(db));
		return false;
	}
	const char *sql = "INSERT OR IGNORE INTO arch_module "
			  "(project_id, path, parent_path, language, "
			  "file_count, entity_count) VALUES (?,?,?,?,?,?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error = "prepare arch_module insert failed: " +
			std::string(sqlite3_errmsg(db));
		return false;
	}
	for (const auto &kv : file_counts) {
		const std::string &path = kv.first;
		std::string parent = normalizePath(dirOf(path));
		std::string lang = languages.count(path) ? languages.at(path) :
							   "";
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, parent.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, lang.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 5, kv.second);
		auto it = entity_counts.find(path);
		sqlite3_bind_int(stmt, 6,
				 it != entity_counts.end() ? it->second : 0);
		int rc = sqlite3_step(stmt);
		sqlite3_reset(stmt);
		if (rc != SQLITE_DONE) {
			error = "arch_module insert step failed: " +
				std::string(sqlite3_errmsg(db));
			sqlite3_finalize(stmt);
			return false;
		}
	}
	sqlite3_finalize(stmt);
	return true;
}

/// Step 3: Write arch_module_member (entity → module mapping).
bool writeModuleMembers(sqlite3 *db, uint64_t project_id,
			const std::unordered_map<std::string, int> &file_counts,
			std::string &error)
{
	std::string del = "DELETE FROM arch_module_member WHERE project_id = " +
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
	for (const auto &kv : file_counts) {
		const std::string &mod_path = kv.first;
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			error = "prepare arch_module_member failed: " +
				std::string(sqlite3_errmsg(db));
			return false;
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, mod_path.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 4, mod_path.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 5, mod_path.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 6, mod_path.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 7, mod_path.c_str(), -1,
				  SQLITE_TRANSIENT);
		int rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		if (rc != SQLITE_DONE) {
			error = "arch_module_member insert failed: " +
				std::string(sqlite3_errmsg(db));
			return false;
		}
	}
	return true;
}

/// Resolve a target import path against known modules.
/// Sets dst_module, resolution_kind, and confidence.
void resolveImportTarget(const std::string &target_path,
			 const std::unordered_set<std::string> &known_modules,
			 std::string &dst_module, std::string &resolution_kind,
			 double &confidence)
{
	std::string normalized_target = normalizePath(target_path);
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
		if (target_path.find("::") != std::string::npos ||
		    target_path.find('/') == std::string::npos) {
			dst_module = normalized_target;
			resolution_kind = "external";
			confidence = 0.5;
		} else {
			dst_module = normalized_target;
			resolution_kind = "unresolved";
			confidence = 0.0;
		}
	}
}

/// Step 4: Build structural edges from the import table.
bool writeStructuralEdgesFromImports(
	sqlite3 *db, uint64_t project_id,
	const std::unordered_set<std::string> &known_modules,
	std::string &error)
{
	std::string del = "DELETE FROM arch_edge WHERE project_id = " +
			  std::to_string(project_id);
	if (sqlite3_exec(db, del.c_str(), nullptr, nullptr, nullptr) !=
	    SQLITE_OK) {
		error = "DELETE arch_edge failed: " +
			std::string(sqlite3_errmsg(db));
		return false;
	}
	const char *sql = "SELECT target_path, file_path, source_row "
			  "FROM import WHERE project_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error = std::string("prepare import failed: ") +
			sqlite3_errmsg(db);
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	const char *insert_sql = "INSERT OR IGNORE INTO arch_edge "
				 "(project_id, src_module, dst_module, "
				 "edge_kind, resolution_kind, confidence, "
				 "source_file, source_row, raw_target) "
				 "VALUES (?,?,?,?,?,?,?,?,?)";
	sqlite3_stmt *insert_stmt = nullptr;
	if (sqlite3_prepare_v2(db, insert_sql, -1, &insert_stmt, nullptr) !=
	    SQLITE_OK) {
		error = "prepare arch_edge insert failed: " +
			std::string(sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return false;
	}

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char *target = sqlite3_column_text(stmt, 0);
		const unsigned char *sfile = sqlite3_column_text(stmt, 1);
		int import_row = sqlite3_column_int(stmt, 2);
		if (!target || !sfile)
			continue;
		std::string target_path =
			reinterpret_cast<const char *>(target);
		std::string source_file = reinterpret_cast<const char *>(sfile);
		std::string src_module = normalizePath(dirOf(source_file));

		std::string dst_module;
		std::string resolution_kind;
		double confidence = 0.0;
		resolveImportTarget(
			resolveRelativeTarget(target_path, source_file),
			known_modules, dst_module, resolution_kind, confidence);

		if (src_module.empty() || dst_module.empty())
			continue;
		if (src_module == dst_module)
			continue;

		sqlite3_bind_int64(insert_stmt, 1,
				   static_cast<int64_t>(project_id));
		sqlite3_bind_text(insert_stmt, 2, src_module.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(insert_stmt, 3, dst_module.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(insert_stmt, 4, "import", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(insert_stmt, 5, resolution_kind.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_double(insert_stmt, 6, confidence);
		sqlite3_bind_text(insert_stmt, 7, source_file.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int(insert_stmt, 8, import_row);
		sqlite3_bind_text(insert_stmt, 9, target_path.c_str(), -1,
				  SQLITE_TRANSIENT);
		int ins_rc = sqlite3_step(insert_stmt);
		sqlite3_reset(insert_stmt);
		if (ins_rc != SQLITE_DONE) {
			error = "arch_edge insert failed: " +
				std::string(sqlite3_errmsg(db));
			sqlite3_finalize(insert_stmt);
			sqlite3_finalize(stmt);
			return false;
		}
	}
	sqlite3_finalize(insert_stmt);
	sqlite3_finalize(stmt);
	return true;
}

} // namespace

bool buildStructuralEdges(store::GraphStore &store, uint64_t project_id,
			  std::string &error)
{
	sqlite3 *db = store.handle();
	if (!db) {
		error = "database not open [module=arch, "
			"method=buildStructuralEdges]";
		return false;
	}

	// Wrap all writes (DELETE + INSERT) in a SAVEPOINT so a mid-way
	// failure rolls back to the pre-build state instead of leaving the
	// module/edge tables empty (DELETE ran, INSERT failed).
	//
	// A SAVEPOINT works whether or not the caller already holds a
	// transaction: when the caller has its own BEGIN, a ROLLBACK TO
	// savepoint undoes only the local writes without touching the outer
	// transaction; when there is no outer transaction, the SAVEPOINT is
	// the transaction boundary.  This replaces the old own_txn pattern
	// whose rollback was a no-op inside an outer transaction.
	const char *sp_name = "cs_build_edges";
	if (sqlite3_exec(db, "SAVEPOINT cs_build_edges", nullptr, nullptr,
			 nullptr) != SQLITE_OK) {
		error = std::string("SAVEPOINT failed: ") + sqlite3_errmsg(db);
		return false;
	}
	auto rollback = [&]() {
		std::string rl = std::string("ROLLBACK TO ") + sp_name;
		sqlite3_exec(db, rl.c_str(), nullptr, nullptr, nullptr);
		sqlite3_exec(db, "RELEASE cs_build_edges", nullptr, nullptr,
			     nullptr);
	};

	// Step 1: Build module tree from entity file paths.
	std::unordered_map<std::string, int> module_file_counts;
	std::unordered_map<std::string, int> module_entity_counts;
	std::unordered_map<std::string, std::string> module_languages;
	if (!collectModuleTree(db, project_id, module_file_counts,
			       module_entity_counts, module_languages, error)) {
		rollback();
		return false;
	}

	// Step 2: Write arch_module rows.
	if (!writeArchModules(db, project_id, module_file_counts,
			      module_entity_counts, module_languages, error)) {
		rollback();
		return false;
	}

	// Collect all module paths for internal resolution.
	std::unordered_set<std::string> known_modules;
	for (const auto &kv : module_file_counts)
		known_modules.insert(kv.first);

	// Step 3: Write arch_module_member.
	if (!writeModuleMembers(db, project_id, module_file_counts, error)) {
		rollback();
		return false;
	}

	// Step 4: Build structural edges from the import table.
	if (!writeStructuralEdgesFromImports(db, project_id, known_modules,
					     error)) {
		rollback();
		return false;
	}

	// Release the SAVEPOINT — commits the local writes.
	if (sqlite3_exec(db, "RELEASE cs_build_edges", nullptr, nullptr,
			 nullptr) != SQLITE_OK) {
		error = std::string("RELEASE failed: ") + sqlite3_errmsg(db);
		rollback();
		return false;
	}

	return true;
}

} // namespace arch

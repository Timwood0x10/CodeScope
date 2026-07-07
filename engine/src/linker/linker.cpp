#include "linker.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <sqlite3.h>

#include "../graph/graph_builder.h"

namespace linker
{

// ─── Linker ──────────────────────────────────────────────────────

void Linker::addPass(std::unique_ptr<LinkPass> pass)
{
	if (pass)
		passes_.push_back(std::move(pass));
}

int Linker::run(uint64_t project_id,
		std::vector<std::unique_ptr<ir::TranslationUnit> > &units,
		const std::vector<std::string> &file_paths,
		store::GraphStore *store)
{
	// Shared symbol index built by the first pass and used by subsequent passes
	resolver::ProjectSymbolIndex symbol_index;

	return run(project_id, units, file_paths, symbol_index, store);
}

int Linker::run(uint64_t project_id,
		std::vector<std::unique_ptr<ir::TranslationUnit> > &units,
		const std::vector<std::string> &file_paths,
		resolver::ProjectSymbolIndex &symbol_index,
		store::GraphStore *store)
{
	int passed = 0;
	for (auto &p : passes_) {
		if (p->run(project_id, units, file_paths, symbol_index, store))
			passed++;
		else
			fprintf(stderr, "LINKER: pass '%s' failed\n",
				p->name());
	}
	return passed;
}

// ─── BuildSymbolIndexPass ────────────────────────────────────────

bool BuildSymbolIndexPass::run(
	uint64_t project_id,
	std::vector<std::unique_ptr<ir::TranslationUnit> > &units,
	const std::vector<std::string> &file_paths,
	resolver::ProjectSymbolIndex &symbol_index, store::GraphStore *store)
{
	for (auto &u : units) {
		if (!u)
			continue;
		for (auto *n : u->all_nodes) {
			if (n->name.empty())
				continue;
			resolver::IndexEntry ie;
			ie.name = n->name;
			ie.file_path = n->file_path;
			ie.kind = n->kind;
			ie.loc = n->loc;
			switch (n->kind) {
			case ir::NodeKind::FunctionDecl:
			case ir::NodeKind::MethodDecl:
			case ir::NodeKind::ClassDecl:
			case ir::NodeKind::MacroDecl:
				symbol_index.addEntry(ie);
				break;
			default:
				break;
			}
		}
	}
	fprintf(stderr, "LINKER: %s — %zu symbols indexed\n", name(),
		symbol_index.size());
	return true;
}

// ─── ResolveCallPass ─────────────────────────────────────────────

bool ResolveCallPass::run(
	uint64_t project_id,
	std::vector<std::unique_ptr<ir::TranslationUnit> > &units,
	const std::vector<std::string> &file_paths,
	resolver::ProjectSymbolIndex &symbol_index, store::GraphStore *store)
{
	int cross_file_count = 0;
	for (size_t fi = 0; fi < units.size(); fi++) {
		auto &u = units[fi];
		if (!u)
			continue;
		auto &caller_file = file_paths[fi];
		fprintf(stderr, "RESOLVE_FILE[%zu]: %s (%zu nodes)\n", fi,
			caller_file.c_str(), u->all_nodes.size());
		std::vector<ir::Node *> stubs;
		int call_exprs = 0, resolved_local = 0, resolved_cross = 0,
		    no_name = 0;

		int node_count = 0;
		fprintf(stderr, "  FILE_NODES: ");
		for (auto *node : u->all_nodes) {
			node_count++;
			if (node_count <= 15)
				fprintf(stderr, "%s(%s) ",
					ir::kindName(node->kind),
					node->name.c_str());
		}
		fprintf(stderr, "\n");

		for (auto *node : u->all_nodes) {
			if (node->kind != ir::NodeKind::CallExpr)
				continue;

			// Find function name from child identifier
			std::string fname;
			for (auto *c : node->children) {
				if (c->kind == ir::NodeKind::IdentifierExpr &&
				    !c->name.empty()) {
					fname = c->name;
					break;
				}
			}
			if (fname.empty())
				continue;

			// Already has a local CallTarget — skip (intra-file resolved)
			bool has_local = false;
			for (auto &e : node->semantic_edges) {
				if (e.relation == ir::Relation::CallTarget) {
					has_local = true;
					break;
				}
			}
			if (has_local) {
				resolved_local++;
				continue;
			}

			// Look up in global symbol index
			resolved_cross++;
			auto *candidates = symbol_index.lookup(fname);
			if (!candidates || candidates->empty()) {
				fprintf(stderr,
					"  RESOLVE[%s]: '%s' not in index (index has %zu entries)\n",
					caller_file.c_str(), fname.c_str(),
					symbol_index.size());
				continue;
			}

			fprintf(stderr,
				"  RESOLVE[%s]: '%s' has %zu candidates\n",
				caller_file.c_str(), fname.c_str(),
				candidates->size());
			for (auto &c : *candidates)
				fprintf(stderr, "    candidate: %s in %s\n",
					c.name.c_str(), c.file_path.c_str());

			// Find best candidate from a different file
			const resolver::IndexEntry *best = nullptr;
			int best_score = -1;
			for (auto &c : *candidates) {
				if (c.file_path == caller_file)
					continue; // same file
				int score = rankCandidate(c, caller_file);
				if (score > best_score) {
					best_score = score;
					best = &c;
				}
			}
			if (!best)
				continue;

			// Create stub and add CallTarget edge (NOT as a child node,
			// to prevent the GraphBuilder from traversing into the stub
			// and creating spurious edges within the target file).
			auto *stub = new ir::Node();
			stub->kind = ir::NodeKind::FunctionDecl;
			stub->name = best->name;
			stub->file_path = best->file_path;
			stub->loc = best->loc;
			node->semantic_edges.push_back(
				{ stub, ir::Relation::CallTarget });
			stubs.push_back(stub);
			cross_file_count++;
		}
		// Add cross-file stubs to all_nodes after the iteration loop
		for (auto *s : stubs)
			u->all_nodes.push_back(s);
	}
	fprintf(stderr, "LINKER: %s — %d cross-file calls resolved\n", name(),
		cross_file_count);
	return true;
}

// ─── RankCandidate score weights ──────────────────────────────────
static constexpr int kScoreImplExt = 5; // .c/.cpp over .h
static constexpr int kScoreSameDir = 3; // same directory
static constexpr int kScoreParentDir = 1; // parent directory
static constexpr int kScoreShallowDepth = 2; // ≤2 segment difference
static constexpr int kMaxDepthDiff = 2; // max path depth difference for bonus

int ResolveCallPass::rankCandidate(const resolver::IndexEntry &candidate,
				   const std::string &caller_file) const
{
	int score = 0;
	try {
		std::filesystem::path cp(caller_file);
		std::filesystem::path dp(candidate.file_path);

		// Prefer definitions (.c/.cpp) over prototypes (.h)
		std::string ext = dp.extension().string();
		if (ext == ".c" || ext == ".cpp" || ext == ".cc" ||
		    ext == ".cxx")
			score += kScoreImplExt;

		// Same directory = most likely
		if (cp.parent_path() == dp.parent_path())
			score += kScoreSameDir;
		else if (cp.parent_path().parent_path() ==
			 dp.parent_path().parent_path())
			score += kScoreParentDir;

		// Shorter path depth difference = more related
		auto ci = cp.begin(), di = dp.begin();
		while (ci != cp.end() && di != dp.end() && *ci == *di) {
			++ci;
			++di;
		}
		int remaining = 0;
		for (; ci != cp.end(); ++ci)
			remaining++;
		for (; di != dp.end(); ++di)
			remaining++;
		if (remaining <= kMaxDepthDiff)
			score += kScoreShallowDepth;
	} catch (const std::exception &e) {
		fprintf(stderr, "LINKER: rankCandidate exception: %s\n",
			e.what());
	}
	return score;
}

// ─── EmitGraphPass ──────────────────────────────────────────────

bool EmitGraphPass::run(
	uint64_t project_id,
	std::vector<std::unique_ptr<ir::TranslationUnit> > &units,
	const std::vector<std::string> &file_paths,
	resolver::ProjectSymbolIndex &symbol_index, store::GraphStore *store)
{
	for (size_t fi = 0; fi < units.size(); fi++) {
		auto &u = units[fi];
		if (!u)
			continue;
		auto &fp = file_paths[fi];

		// Determine next graph node ID
		uint64_t start_id = 1;
		{
			sqlite3_stmt *s = nullptr;
			const char *q =
				"SELECT COALESCE(MAX(id), 0) + 1 FROM graph_nodes";
			if (sqlite3_prepare_v2(store->handle(), q, -1, &s,
					       nullptr) == SQLITE_OK) {
				if (sqlite3_step(s) == SQLITE_ROW)
					start_id = static_cast<uint64_t>(
						sqlite3_column_int64(s, 0));
				sqlite3_finalize(s);
			}
		}

		// Reassign IDs for any stubs added by earlier passes
		u->assignIds();

		// Build graph
		graph::GraphBuilder builder(project_id, start_id);
		auto sym_g = builder.buildSymbolGraph(u.get());
		auto call_g = builder.buildCallGraph(u.get());

		// Persist
		store->upsertFile(project_id, fp.c_str(),
				  (fp.size() > 2) ?
					  (fp.substr(fp.size() - 1) == "h" ?
						   "c" :
						   "unknown") :
					  "unknown",
				  "");
		// Batch insert — use prepared-statement reuse APIs
		store->insertGraphNodes(project_id, sym_g.nodes);
		store->insertGraphEdges(project_id, sym_g.edges);
		store->insertGraphEdges(project_id, call_g.edges);
	}
	fprintf(stderr, "LINKER: %s — %zu files emitted\n", name(),
		units.size());
	return true;
}

} // namespace linker

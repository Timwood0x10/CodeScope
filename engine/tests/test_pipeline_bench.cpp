/**
 * Pipeline benchmark: compare old (Translator→TranslationUnit/Node*) vs
 * new (JsVisitor→SemanticUnit/flat records) on a large JS file.
 *
 * Metrics:
 *   - CPU time (translate, symbol graph, call graph)
 *   - Memory estimate (vector capacity bytes)
 *   - Record / node counts
 *
 * Usage: ./test_pipeline_bench <grammars_dir>
 */

#include "../src/ir/ir.h"
#include "../src/ir/ir_translator.h"
#include "../src/ir/semantic_unit.h"
#include "../src/ir/semantic_emitter.h"
#include "../src/ir/translators/js_visitor.h"
#include "../src/graph/graph_builder.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <sys/resource.h>
#include <vector>

#include <tree_sitter/api.h>

using Clock = std::chrono::steady_clock;

static double elapsed(Clock::time_point start)
{
	auto end = Clock::now();
	return std::chrono::duration<double, std::milli>(end - start).count();
}

/** Get approximate RSS in KB (macOS). */
static size_t currentRSS()
{
	struct rusage usage;
	if (getrusage(RUSAGE_SELF, &usage) == 0)
		return static_cast<size_t>(usage.ru_maxrss) / 1024;
	return 0;
}

static const TSLanguage *loadGrammar(const char *grammars_dir,
				     const char *name)
{
	std::string path =
		std::string(grammars_dir) + "/tree-sitter-" + name + ".so";
	void *handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
	if (!handle)
		return nullptr;
	std::string sym = "tree_sitter_" + std::string(name);
	auto *fn = reinterpret_cast<const TSLanguage *(*)()>(
		dlsym(handle, sym.c_str()));
	if (!fn)
		return nullptr;
	return fn();
}

/** Generate a synthetic JS file with N functions and M calls per function. */
static std::string generateJS(int num_functions, int calls_per_function)
{
	std::string code;
	code += "// Synthetic benchmark file\n";
	code += "const global = 42;\n\n";

	for (int i = 0; i < num_functions; i++) {
		char buf[256];
		snprintf(buf, sizeof(buf),
			 "function func_%d(a, b) {\n"
			 "    let x = a + b;\n"
			 "    let y = a * b;\n",
			 i);
		code += buf;

		for (int j = 0; j < calls_per_function; j++) {
			snprintf(buf, sizeof(buf),
				 "    const r%d = helper(%d + j, x);\n"
				 "    if (r%d > 0) {\n"
				 "        return r%d + y;\n"
				 "    }\n",
				 j, j, j, j);
			code += buf;
		}
		code += "    return x + y;\n"
			"}\n\n";
	}

	// Add a few import/export statements
	code += "import { helper } from './helper';\n"
		"export { func_0 };\n";

	return code;
}

static size_t vectorCapacityBytes(const std::vector<ir::Record> &records)
{
	return records.capacity() * sizeof(ir::Record);
}

static size_t estimateNodeTreeMemory(ir::TranslationUnit *unit)
{
	size_t total = 0;
	for (auto *n : unit->all_nodes) {
		total += sizeof(ir::Node);
		total += n->children.capacity() * sizeof(ir::Node *);
		total += n->semantic_edges.capacity() *
			 sizeof(ir::SemanticEdge);
		total += n->name.capacity();
		total += n->qualified_name.capacity();
		total += n->file_path.capacity();
		total += n->language.capacity();
	}
	return total + unit->all_nodes.capacity() * sizeof(ir::Node *);
}

static size_t estimateSemanticUnitMemory(const ir::SemanticUnit *unit)
{
	size_t total = unit->allRecords().capacity() * sizeof(ir::Record);
	// Rough estimate for string allocations
	for (auto &r : unit->allRecords()) {
		total += r.name.capacity();
		total += r.qualified_name.capacity();
		total += r.file_path.capacity();
		total += r.language.capacity();
	}
	return total;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <grammars_dir>\n", argv[0]);
		return 1;
	}

	const char *grammars_dir = argv[1];

	const TSLanguage *js_lang = loadGrammar(grammars_dir, "javascript");
	if (!js_lang) {
		fprintf(stderr, "FAIL: cannot load JavaScript grammar from %s\n",
			grammars_dir);
		return 1;
	}

	// Generate test code
	const int NUM_FUNCS = 200;
	const int CALLS_PER_FUNC = 5;
	std::string source = generateJS(NUM_FUNCS, CALLS_PER_FUNC);

	printf("=== Pipeline Benchmark ===\n");
	printf("source: %zu bytes (%.1f KB), %d functions, ~%d calls each\n",
	       source.size(), source.size() / 1024.0,
	       NUM_FUNCS, CALLS_PER_FUNC);
	printf("estimated total calls: ~%d\n",
	       NUM_FUNCS * CALLS_PER_FUNC);
	printf("\n");

	// Parse once, run both pipelines on the same tree
	TSParser *parser = ts_parser_new();
	ts_parser_set_language(parser, js_lang);
	TSTree *tree = ts_parser_parse_string(parser, nullptr,
					      source.c_str(),
					      source.size());
	if (!tree) {
		fprintf(stderr, "FAIL: parse failed\n");
		return 1;
	}
	ts_parser_delete(parser);

	// ── Old pipeline: Translator → TranslationUnit ──────────
	printf("─── OLD PIPELINE (Translator → TranslationUnit/Node*) ───\n");

	size_t rss_before_old = currentRSS();

	auto t0_old = Clock::now();
	auto translator = std::unique_ptr<ir::Translator>(
		ir::createTranslator("javascript"));
	ir::TranslationUnit *old_unit =
		translator->translate(tree, source.c_str(),
				      "/bench/test.js");
	double translate_old_ms = elapsed(t0_old);

	size_t rss_after_old = currentRSS();
	size_t old_node_count = old_unit->all_nodes.size();
	size_t old_mem_estimate = estimateNodeTreeMemory(old_unit);

	// Build graph from old pipeline
	graph::GraphBuilder old_builder(1);
	t0_old = Clock::now();
	auto old_sym = old_builder.buildSymbolGraph(old_unit);
	auto old_call = old_builder.buildCallGraph(old_unit);
	double old_graph_ms = elapsed(t0_old);

	printf("  translate:  %.2f ms\n", translate_old_ms);
	printf("  graph:      %.2f ms\n", old_graph_ms);
	printf("  total:      %.2f ms\n", translate_old_ms + old_graph_ms);
	printf("  nodes:      %zu IR nodes\n", old_node_count);
	printf("  graph:      %zu graph nodes, %zu edges\n",
	       old_sym.nodes.size(),
	       old_sym.edges.size() + old_call.edges.size());
	printf("  mem_est:    %.1f KB (Node* tree estimate)\n",
	       old_mem_estimate / 1024.0);
	printf("  RSS_delta:  %zu KB\n",
	       rss_after_old - rss_before_old);
	printf("\n");

	// Copy tree (tree-sitter trees can't be reused)
	TSTree *tree2 = ts_tree_copy(tree);

	// ── New pipeline: JsVisitor → SemanticUnit ─────────────
	printf("─── NEW PIPELINE (JsVisitor → SemanticUnit/flat records) ───\n");

	size_t rss_before_new = currentRSS();

	auto t0_new = Clock::now();
	ir::JsVisitor visitor;
	ir::SemanticUnit *new_unit = visitor.visit(
		tree2, source.c_str(), "/bench/test.js");
	double visit_new_ms = elapsed(t0_new);

	size_t rss_after_new = currentRSS();
	size_t new_record_count = new_unit->size();
	size_t new_mem_estimate = estimateSemanticUnitMemory(new_unit);

	// Build graph from new pipeline
	graph::GraphBuilder new_builder(1);
	t0_new = Clock::now();
	auto new_sym = new_builder.buildSymbolGraph(*new_unit);
	auto new_call = new_builder.buildCallGraph(*new_unit);
	double new_graph_ms = elapsed(t0_new);

	printf("  visit:      %.2f ms\n", visit_new_ms);
	printf("  graph:      %.2f ms\n", new_graph_ms);
	printf("  total:      %.2f ms\n", visit_new_ms + new_graph_ms);
	printf("  records:    %zu flat records\n", new_record_count);
	printf("  graph:      %zu graph nodes, %zu edges\n",
	       new_sym.nodes.size(),
	       new_sym.edges.size() + new_call.edges.size());
	printf("  mem_est:    %.1f KB (SemanticUnit estimate)\n",
	       new_mem_estimate / 1024.0);
	printf("  RSS_delta:  %zu KB\n",
	       rss_after_new - rss_before_new);
	printf("\n");

	// ── Comparison ─────────────────────────────────────────
	printf("─── COMPARISON ───\n");
	double old_total = translate_old_ms + old_graph_ms;
	double new_total = visit_new_ms + new_graph_ms;
	printf("  CPU speedup:     %.2fx\n", old_total / new_total);
	printf("  mem reduction:   %.1fx (estimate)\n",
	       (double)old_mem_estimate /
		       (new_mem_estimate > 0 ? new_mem_estimate : 1));
	printf("  record vs node:  %.1f bytes/node (old), "
	       "%.1f bytes/record (new)\n",
	       (double)old_mem_estimate / old_node_count,
	       (double)new_mem_estimate / new_record_count);
	printf("\n");

	// Cleanup
	ts_tree_delete(tree);
	ts_tree_delete(tree2);
	delete old_unit;
	delete new_unit;

	printf("=== Pipeline benchmark passed ===\n");
	return 0;
}

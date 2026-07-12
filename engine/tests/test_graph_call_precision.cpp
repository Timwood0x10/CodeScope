#include "../src/ir/ir.h"
#include "../src/ir/semantic_unit.h"
#include "../src/ir/semantic_emitter.h"
#include "../src/graph/graph_builder.h"
#include <cassert>
#include <cstdio>

// ── Tests for GraphBuilder layered callee resolution ─────────────────────
//
// These tests verify that buildCallGraph uses the layered matching strategy
// (ref_original_id -> qualified_name -> name+arity -> name) and avoids false
// edges between same-name functions in different scopes or with different
// arities.

// ── Test 1: qualified_name prevents false edges between same-name functions ─

static void test_qualified_name_prevents_false_edges()
{
	using namespace ir;
	using namespace graph;

	SemanticUnit unit;
	SemanticEmitter emitter(&unit);
	unit.setFilePath("/test/qualified.js");
	unit.setLanguage("javascript");

	// Two functions with the same bare name "process" but different
	// qualified_names — a call to ClassA::process must NOT also link to
	// ClassB::process.
	uint64_t fn_a = unit.addRecord(RecordKind::Function, "process",
				       "ClassA::process", 0, 0, { 1, 0, 3, 0 });
	uint64_t fn_b = unit.addRecord(RecordKind::Function, "process",
				       "ClassB::process", 0, 0, { 5, 0, 7, 0 });
	(void)fn_a;
	(void)fn_b;

	// main() calls ClassA::process specifically (qualified_name set on CallExpr)
	uint64_t main_fn = emitter.emitFunction("main", { 9, 0, 15, 0 });
	unit.addRecord(RecordKind::CallExpr, "process", "ClassA::process", 0,
		       main_fn, { 10, 4, 10, 20 });

	GraphBuilder builder(1);
	auto sym_g = builder.buildSymbolGraph(unit);
	auto call_g = builder.buildCallGraph(unit);

	// Find graph node IDs by qualified_name
	uint64_t main_gid = 0, a_gid = 0, b_gid = 0;
	for (auto &n : sym_g.nodes) {
		if (n.name == "main")
			main_gid = n.id;
		if (n.qualified_name == "ClassA::process")
			a_gid = n.id;
		if (n.qualified_name == "ClassB::process")
			b_gid = n.id;
	}
	assert(main_gid > 0);
	assert(a_gid > 0);
	assert(b_gid > 0);

	// Expect: edge main -> ClassA::process (YES)
	//         edge main -> ClassB::process (NO — qualified_name prevents it)
	bool edge_to_a = false, edge_to_b = false;
	for (auto &e : call_g.edges) {
		if (e.type == EdgeType::Calls && e.source_id == main_gid) {
			if (e.target_id == a_gid)
				edge_to_a = true;
			if (e.target_id == b_gid)
				edge_to_b = true;
		}
	}
	assert(edge_to_a);
	assert(!edge_to_b);

	printf("  \u2713 test_qualified_name_prevents_false_edges: %zu call edges\n",
	       call_g.edges.size());
}

// ── Test 2: arity prevents false edges between same-name overloads ───────

static void test_arity_prevents_false_edges()
{
	using namespace ir;
	using namespace graph;

	SemanticUnit unit;
	SemanticEmitter emitter(&unit);
	unit.setFilePath("/test/arity.js");
	unit.setLanguage("javascript");

	// Two overloads: init(int) with arity=1 and init(int,int) with arity=2
	uint64_t fn1 =
		emitter.emitFunction("init", { 1, 0, 3, 0 }, 0, 1); // arity=1
	uint64_t fn2 =
		emitter.emitFunction("init", { 5, 0, 7, 0 }, 0, 2); // arity=2

	// main() calls init(x) — arity=1
	uint64_t main_fn = emitter.emitFunction("main", { 9, 0, 15, 0 });
	emitter.emitReference("init", { 10, 4, 10, 15 }, main_fn, 1); // arity=1

	GraphBuilder builder(1);
	auto sym_g = builder.buildSymbolGraph(unit);
	auto call_g = builder.buildCallGraph(unit);

	// Distinguish the two init functions by ir_node_id (record id)
	uint64_t main_gid = 0, init1_gid = 0, init2_gid = 0;
	for (auto &n : sym_g.nodes) {
		if (n.name == "main")
			main_gid = n.id;
		if (n.name == "init" && n.ir_node_id == fn1)
			init1_gid = n.id;
		if (n.name == "init" && n.ir_node_id == fn2)
			init2_gid = n.id;
	}
	assert(main_gid > 0);
	assert(init1_gid > 0);
	assert(init2_gid > 0);

	// Expect: edge main -> init(arity=1) (YES — exact arity match)
	//         edge main -> init(arity=2) (NO — arity mismatch)
	bool edge_to_init1 = false, edge_to_init2 = false;
	for (auto &e : call_g.edges) {
		if (e.type == EdgeType::Calls && e.source_id == main_gid) {
			if (e.target_id == init1_gid)
				edge_to_init1 = true;
			if (e.target_id == init2_gid)
				edge_to_init2 = true;
		}
	}
	assert(edge_to_init1);
	assert(!edge_to_init2);

	printf("  \u2713 test_arity_prevents_false_edges: %zu call edges\n",
	       call_g.edges.size());
}

// ── Test 3: ref_original_id resolves to the exact callee ────────────────

static void test_ref_original_id_resolves_exactly()
{
	using namespace ir;
	using namespace graph;

	SemanticUnit unit;
	SemanticEmitter emitter(&unit);
	unit.setFilePath("/test/refid.js");
	unit.setLanguage("javascript");

	// Two same-name functions: handle (A) and handle (B)
	uint64_t fn_a = emitter.emitFunction("handle", { 1, 0, 3, 0 });
	uint64_t fn_b = emitter.emitFunction("handle", { 5, 0, 7, 0 });

	// main() calls handle() — bare name, but ref_original_id pinpoints fn_a
	uint64_t main_fn = emitter.emitFunction("main", { 9, 0, 15, 0 });
	uint64_t call_id =
		emitter.emitCall("handle", { 10, 4, 10, 15 }, main_fn);
	// Resolve the call precisely to fn_a via ref_original_id
	bool ok = unit.setCallReference(call_id, fn_a);
	assert(ok);

	GraphBuilder builder(1);
	auto sym_g = builder.buildSymbolGraph(unit);
	auto call_g = builder.buildCallGraph(unit);

	// Distinguish the two handle functions by ir_node_id
	uint64_t main_gid = 0, a_gid = 0, b_gid = 0;
	for (auto &n : sym_g.nodes) {
		if (n.name == "main")
			main_gid = n.id;
		if (n.name == "handle" && n.ir_node_id == fn_a)
			a_gid = n.id;
		if (n.name == "handle" && n.ir_node_id == fn_b)
			b_gid = n.id;
	}
	assert(main_gid > 0);
	assert(a_gid > 0);
	assert(b_gid > 0);

	// Expect: edge main -> fn_a (YES — exact ref_original_id match)
	//         edge main -> fn_b (NO — ref_original_id is authoritative)
	bool edge_to_a = false, edge_to_b = false;
	for (auto &e : call_g.edges) {
		if (e.type == EdgeType::Calls && e.source_id == main_gid) {
			if (e.target_id == a_gid)
				edge_to_a = true;
			if (e.target_id == b_gid)
				edge_to_b = true;
		}
	}
	assert(edge_to_a);
	assert(!edge_to_b);

	printf("  \u2713 test_ref_original_id_resolves_exactly: %zu call edges\n",
	       call_g.edges.size());
}

// ── Test 4: bare name fallback resolves when candidate is unique ────────

static void test_bare_name_fallback_unique_candidate()
{
	using namespace ir;
	using namespace graph;

	SemanticUnit unit;
	SemanticEmitter emitter(&unit);
	unit.setFilePath("/test/fallback.js");
	unit.setLanguage("javascript");

	// A single compute() function (unique name)
	uint64_t fn = emitter.emitFunction("compute", { 1, 0, 3, 0 });
	(void)fn;

	// main() calls compute() — bare name, no arity, no qualified_name
	uint64_t main_fn = emitter.emitFunction("main", { 5, 0, 9, 0 });
	emitter.emitCall("compute", { 6, 4, 6, 15 }, main_fn);

	GraphBuilder builder(1);
	auto sym_g = builder.buildSymbolGraph(unit);
	auto call_g = builder.buildCallGraph(unit);

	uint64_t main_gid = 0, compute_gid = 0;
	for (auto &n : sym_g.nodes) {
		if (n.name == "main")
			main_gid = n.id;
		if (n.name == "compute")
			compute_gid = n.id;
	}
	assert(main_gid > 0);
	assert(compute_gid > 0);

	// Expect: edge main -> compute (name-only fallback works for unique name)
	bool edge_found = false;
	for (auto &e : call_g.edges) {
		if (e.type == EdgeType::Calls && e.source_id == main_gid &&
		    e.target_id == compute_gid)
			edge_found = true;
	}
	assert(edge_found);

	printf("  \u2713 test_bare_name_fallback_unique_candidate: %zu call edges\n",
	       call_g.edges.size());
}

// ── Test 5: external cross-file index resolves calls to other units ─────

static void test_external_index_cross_file_call()
{
	using namespace ir;
	using namespace graph;

	// Unit A: main() calls "helper" — helper is NOT defined in unit A
	SemanticUnit unit_a;
	SemanticEmitter emitter_a(&unit_a);
	unit_a.setFilePath("/test/a.js");
	unit_a.setLanguage("javascript");
	uint64_t main_fn = emitter_a.emitFunction("main", { 1, 0, 5, 0 });
	emitter_a.emitCall("helper", { 2, 4, 2, 15 }, main_fn);
	(void)main_fn;

	GraphBuilder builder(1);
	auto sym_a = builder.buildSymbolGraph(unit_a);

	// Find main's graph node ID
	uint64_t main_gid = 0;
	for (auto &n : sym_a.nodes) {
		if (n.name == "main")
			main_gid = n.id;
	}
	assert(main_gid > 0);

	// Build an external CalleeIndex manually (simulating a cross-file index:
	// "helper" is defined in another file with a known graph node ID).
	const uint64_t external_helper_gid = main_gid + 100; // fake external ID
	CalleeIndex ext_index;
	ext_index.by_name.emplace("helper",
				  CalleeCandidate{ 0, external_helper_gid });

	auto call_g = builder.buildCallGraph(unit_a, ext_index);

	// Expect: edge main -> external_helper_gid (cross-file resolution)
	bool found = false;
	for (auto &e : call_g.edges) {
		if (e.type == EdgeType::Calls && e.source_id == main_gid &&
		    e.target_id == external_helper_gid)
			found = true;
	}
	assert(found);

	printf("  \u2713 test_external_index_cross_file_call: %zu call edges\n",
	       call_g.edges.size());
}

// ── Main ──────────────────────────────────────────────────────────────────

int main()
{
	printf("GraphBuilder call-edge precision tests:\n");

	test_qualified_name_prevents_false_edges();
	test_arity_prevents_false_edges();
	test_ref_original_id_resolves_exactly();
	test_bare_name_fallback_unique_candidate();
	test_external_index_cross_file_call();

	printf("\n=== GraphBuilder call-edge precision tests passed ===\n");
	return 0;
}

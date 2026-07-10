/**
 * Unit tests for SemanticUnit + SemanticEmitter.
 *
 * Tests cover:
 * - Record creation and ID assignment
 * - Parent-child relationships via parent_id
 * - Query methods (findByKind, findByName, getChildren)
 * - Emitter interface wrapping addRecord
 * - Memory / size expectations
 */

#include "../src/ir/semantic_unit.h"
#include "../src/ir/semantic_emitter.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg)                                                      \
	do {                                                                   \
		tests_run++;                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL [%d]: %s\n", tests_run, msg);    \
			exit(1);                                               \
		}                                                              \
		tests_passed++;                                                \
	} while (0)

#define CHECK_EQ(a, b, msg)                                                    \
	do {                                                                   \
		tests_run++;                                                   \
		if ((a) != (b)) {                                              \
			fprintf(stderr, "FAIL [%d]: %s — expected %llu, "      \
					"got %llu\n",                          \
				tests_run, msg,                                 \
				static_cast<unsigned long long>(b),            \
				static_cast<unsigned long long>(a));           \
			exit(1);                                               \
		}                                                              \
		tests_passed++;                                                \
	} while (0)

// ── Test: Empty SemanticUnit ──────────────────────────────────

static void test_empty_unit()
{
	ir::SemanticUnit unit;
	CHECK(unit.empty(), "new unit should be empty");
	CHECK_EQ(unit.size(), 0ULL, "empty unit size == 0");
	CHECK(unit.allRecords().empty(), "empty unit records vector");
}

// ── Test: Add records and verify IDs ──────────────────────────

static void test_add_records()
{
	ir::SemanticUnit unit;
	unit.setFilePath("/test/file.ts");
	unit.setLanguage("typescript");

	ir::SourceRange loc = { 0, 0, 5, 0 };
	uint64_t id1 = unit.addRecord(ir::RecordKind::Function,
				      "foo", 0, loc);
	uint64_t id2 = unit.addRecord(ir::RecordKind::Function,
				      "bar", 0, loc);
	uint64_t id3 = unit.addRecord(ir::RecordKind::CallExpr,
				      "baz", id1, loc);

	CHECK_EQ(unit.size(), 3ULL, "3 records added");
	CHECK_EQ(id1, 1ULL, "first record id == 1");
	CHECK_EQ(id2, 2ULL, "second record id == 2");
	CHECK_EQ(id3, 3ULL, "third record id == 3");

	// Verify parent_id
	const auto &rec1 = unit.getRecord(id1);
	const auto &rec3 = unit.getRecord(id3);
	CHECK_EQ(rec1.parent_id, 0ULL, "foo is top-level");
	CHECK_EQ(rec3.parent_id, id1, "baz has foo as parent");
	CHECK(rec1.file_path == "/test/file.ts",
	      "file path propagated");
	CHECK(rec1.language == "typescript",
	      "language propagated");
}

// ── Test: Query methods ───────────────────────────────────────

static void test_queries()
{
	ir::SemanticUnit unit;

	ir::SourceRange loc1 = { 0, 0, 2, 0 };
	ir::SourceRange loc2 = { 3, 0, 5, 0 };
	ir::SourceRange loc3 = { 6, 0, 6, 10 };

	uint64_t fn1 = unit.addRecord(ir::RecordKind::Function,
				      "compute", 0, loc1);
	uint64_t fn2 = unit.addRecord(ir::RecordKind::Function,
	         "add", 0, loc2);
	(void)fn2;
	uint64_t call = unit.addRecord(ir::RecordKind::CallExpr,
	          "add", fn1, loc3);
	(void)call;

	// findByName
	size_t idx = unit.findRecordByName("compute");
	CHECK(idx != SIZE_MAX, "find compute by name");
	CHECK_EQ(unit.allRecords()[idx].id, fn1, "compute ID matches");

	idx = unit.findRecordByName("nonexistent");
	CHECK_EQ(idx, SIZE_MAX, "nonexistent name returns SIZE_MAX");

	// findByKind
	auto fns = unit.findRecordsByKind(ir::RecordKind::Function);
	CHECK_EQ(fns.size(), 2ULL, "2 functions found");

	auto calls = unit.findRecordsByKind(ir::RecordKind::CallExpr);
	CHECK_EQ(calls.size(), 1ULL, "1 call found");

	// getChildren
	auto children = unit.getChildren(fn1);
	CHECK_EQ(children.size(), 1ULL, "compute has 1 child");
	CHECK_EQ(unit.allRecords()[children[0]].kind,
		 ir::RecordKind::CallExpr, "compute's child is a call");
}

// ── Test: Emitter interface ───────────────────────────────────

static void test_emitter()
{
	ir::SemanticUnit unit;
	ir::SemanticEmitter emitter(&unit);

	ir::SourceRange loc = { 0, 0, 10, 0 };

	uint64_t fn = emitter.emitFunction("main", loc);
	CHECK(fn > 0, "emitFunction returns non-zero ID");

	ir::SourceRange call_loc = { 5, 0, 5, 15 };
	uint64_t call = emitter.emitCall("console.log", call_loc, fn);
	CHECK(call > 0, "emitCall returns non-zero ID");

	ir::SourceRange var_loc = { 3, 0, 3, 10 };
	uint64_t var = emitter.emitVariable("x", var_loc, fn);
	CHECK(var > 0, "emitVariable returns non-zero ID");

	// Verify containment
	auto children = unit.getChildren(fn);
	CHECK_EQ(children.size(), 2ULL, "main has 2 children");
	CHECK_EQ(unit.getRecord(call).parent_id, fn,
		 "call parent is main");
	CHECK_EQ(unit.getRecord(var).parent_id, fn,
		 "variable parent is main");

	// Verify location preservation
	CHECK_EQ(unit.getRecord(call).loc.start_row, 5U,
		 "call start row preserved");
	CHECK_EQ(unit.getRecord(call).loc.end_col, 15U,
		 "call end col preserved");

	// Verify export/import emitters
	ir::SourceRange imp_loc = { 0, 0, 0, 20 };
	uint64_t imp = emitter.emitImport("fs", imp_loc);
	CHECK(imp > 0, "emitImport returns non-zero ID");

	ir::SourceRange exp_loc = { 0, 0, 0, 10 };
	uint64_t exp = emitter.emitExport("main", exp_loc);
	CHECK(exp > 0, "emitExport returns non-zero ID");
}

// ── Test: Memory / size sanity ────────────────────────────────

static void test_memory_size()
{
	ir::SemanticUnit unit;
	ir::SemanticEmitter emitter(&unit);

	ir::SourceRange loc = { 0, 0, 1, 0 };

	// Emit 1000 records
	for (int i = 0; i < 1000; i++) {
		emitter.emitFunction("f" + std::to_string(i), loc);
	}

	CHECK_EQ(unit.size(), 1000ULL, "1000 records emitted");

	// Verify memory is reasonable: each Record is ~72 bytes (with SSO),
	// plus vector overhead. 1000 records should be < 200 KB.
	// We can't easily check exact bytes from here, but the vector
	// capacity tells us about contiguous allocation.
	CHECK(unit.allRecords().capacity() >= 1000,
	      "capacity >= 1000 (contiguous allocation)");
}

// ── Main ──────────────────────────────────────────────────────

int main()
{
	test_empty_unit();
	test_add_records();
	test_queries();
	test_emitter();
	test_memory_size();

	printf("=== semantic_unit test passed (%d/%d)\n",
	       tests_passed, tests_run);
	return 0;
}

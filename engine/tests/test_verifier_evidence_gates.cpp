// test_verifier_evidence_gates.cpp
//
// Pins two evidence rules that were added to the verifiers.
//
// 1. Drift detectors must not draw a conclusion from an empty backend.
//    Before the gate, a project with a declared capability but nothing indexed
//    reported every capability as a severity-2 CapabilityDrift ("declared in
//    the README but not implemented"), because countImplementingEntities()
//    returns 0 when the entity table is empty. findOrphanModules() had the
//    same shape: its `NOT EXISTS (import …)` test is vacuously true when the
//    import table is empty, so every module with >=10 entities was reported as
//    an orphan. Both are hard conclusions drawn from missing evidence.
//
// 2. ContractVerifier(ThreadSafe) must
//    (a) not read a lock-shaped but unrelated name (Block / BlockStore /
//        Clock / Deadlock / Unlock) as synchronisation evidence — an
//        unanchored `%lock%` pattern made a project with a `Block` class
//        report as thread-safe — and
//    (b) answer Unknown rather than Contradicted when nothing matches, since a
//        name pattern that fails to match is not evidence that the code is
//        unsafe (Rust's std::sync, Java `synchronized`, an OS lock or a
//        message-passing design are all invisible to the pattern).
//
// The database state is built with explicit SQL rather than by indexing a
// fixture, so each case tests exactly what it claims to.

#include "../src/store/store.h"
#include "../src/verify/capability_drift.h"
#include "../src/verify/claim.h"
#include "../src/verify/contract_verifier.h"
#include "../src/verify/dead_code_inspector.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <sqlite3.h>
#include <string>
#include <unistd.h>
#include <vector>

using namespace store;

static const char *kDbPath = "/tmp/test_verifier_evidence_gates.db";

/// Run a statement, failing loudly: this test builds its own state, so an
/// error here means the fixture is wrong rather than the code under test.
static void execOrDie(sqlite3 *db, const std::string &sql)
{
	char *err = nullptr;
	if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) !=
	    SQLITE_OK) {
		fprintf(stderr, "FAIL: sqlite3_exec: %s | sql=%s\n",
			err ? err : "(no message)", sql.c_str());
		sqlite3_free(err);
		exit(1);
	}
}

static void insertEntity(sqlite3 *db, uint64_t pid, int64_t id, int kind,
			 const std::string &name, const std::string &file,
			 int start_row)
{
	execOrDie(db,
		  "INSERT INTO entity (id, project_id, kind, name, "
		  "qualified_name, file_path, language, start_row, start_col, "
		  "end_row, end_col) VALUES (" +
			  std::to_string(id) + "," + std::to_string(pid) + "," +
			  std::to_string(kind) + ",'" + name + "','','" + file +
			  "','go'," + std::to_string(start_row) + ",0," +
			  std::to_string(start_row + 1) + ",0)");
}

static void insertCallRelation(sqlite3 *db, uint64_t pid, int64_t src,
			       int64_t tgt)
{
	execOrDie(
		db,
		"INSERT INTO relation (project_id, source_id, target_id, type) "
		"VALUES (" +
			std::to_string(pid) + "," + std::to_string(src) + "," +
			std::to_string(tgt) + ",1)");
}

static void clearGraph(sqlite3 *db)
{
	execOrDie(db, "DELETE FROM entity");
	execOrDie(db, "DELETE FROM relation");
	execOrDie(db, "DELETE FROM import");
}

/// Minimal graph so that evidence_backend_ready() reports "ready": at least
/// one entity row and one relation row.
static void seedReadyBackend(sqlite3 *db, uint64_t pid)
{
	insertEntity(db, pid, 1, 0, "helper", "src/main.go", 1);
	insertCallRelation(db, pid, 1, 1);
}

/// Count the orphan-module findings ("DeadModule") in an inspect() bundle.
/// inspect() also runs the orphan-function / architecture-drift / component
/// checks, so the type has to be filtered rather than the total counted.
static size_t countDeadModules(const std::vector<verify::Finding> &all)
{
	size_t n = 0;
	for (const auto &f : all)
		if (f.type == "DeadModule")
			++n;
	return n;
}

/// Count the dead-function findings ("DeadFunction") in an inspect() bundle.
/// Mirrors countDeadModules: inspect() emits several finding types and only
/// this one is the orphan-function check's output.
static size_t countDeadFunctions(const std::vector<verify::Finding> &all)
{
	size_t n = 0;
	for (const auto &f : all)
		if (f.type == "DeadFunction")
			++n;
	return n;
}

static std::string verdictOf(const verify::EvidenceRecord &rec)
{
	switch (rec.verdict) {
	case verify::Verdict::Supported:
		return "Supported";
	case verify::Verdict::Contradicted:
		return "Contradicted";
	case verify::Verdict::Unknown:
		return "Unknown";
	}
	return "?";
}

int main()
{
	unlink(kDbPath);
	unlink((std::string(kDbPath) + "-wal").c_str());
	unlink((std::string(kDbPath) + "-shm").c_str());

	GraphStore store;
	if (!store.open(kDbPath)) {
		fprintf(stderr, "FAIL: cannot open store: %s\n",
			store.error().c_str());
		return 1;
	}
	const uint64_t pid = store.createProject("/tmp", "verifier-gates");
	assert(pid > 0);
	sqlite3 *db = store.handle();
	assert(db != nullptr);
	// ThreadSafe must be a *declared* contract, otherwise verify() returns
	// Unknown at the "no contract declared" step and every assertion below
	// would pass for the wrong reason.
	execOrDie(db,
		  "INSERT INTO contract (project_id, name, origin) VALUES (" +
			  std::to_string(pid) + ",'threadsafe','readme')");

	verify::ContractVerifier verifier(&store, pid);
	verify::Claim claim;
	claim.type = verify::ClaimType::ContractHolds;
	claim.subject = "ThreadSafe";
	claim.predicate = "implemented_by";
	claim.scope = "repository";
	claim.source_kind = "manual";

	// ── Case 1: a `Block` class is not synchronisation evidence ──
	// `%lock%` matched it before the patterns were anchored.
	clearGraph(db);
	insertEntity(db, pid, 1, 2, "Block", "src/block.go", 1);
	insertEntity(db, pid, 2, 0, "helper", "src/main.go", 2);
	insertCallRelation(db, pid, 2, 1);
	{
		const std::string v = verdictOf(verifier.verify(claim));
		printf("  [debug] Block-only          -> %s\n", v.c_str());
		assert(v == "Unknown" &&
		       "a class named Block must not be read as a lock");
	}

	// ── Case 2: real synchronisation evidence still supports it ──
	insertEntity(db, pid, 3, 5, "mutex", "src/main.go", 3);
	{
		const std::string v = verdictOf(verifier.verify(claim));
		printf("  [debug] with mutex          -> %s\n", v.c_str());
		assert(v == "Supported" &&
		       "an actual mutex entity must still support ThreadSafe");
	}

	// ── Case 3: no synchronisation name at all -> Unknown ──
	// The previous code returned Contradicted ("not thread safe") here.
	clearGraph(db);
	seedReadyBackend(db, pid);
	{
		const std::string v = verdictOf(verifier.verify(claim));
		printf("  [debug] no sync primitive   -> %s\n", v.c_str());
		assert(v == "Unknown" &&
		       "absence of a matching name is not evidence of being "
		       "unsafe; Contradicted would over-claim");
	}

	// ── Case 4: capability drift with an empty backend ──
	// A declared capability plus no indexed entity rows must yield no drift
	// conclusion (previously one severity-2 CapabilityDrift).
	clearGraph(db);
	execOrDie(
		db,
		"INSERT INTO capability (project_id, name, summary, source_kind) "
		"VALUES (" +
			std::to_string(pid) + ",'FancyFeature','','readme')");
	{
		const std::vector<verify::DriftItem> drifts =
			verify::detectCapabilityDrift(store, pid);
		printf("  [debug] capability drifts   -> %zu (empty backend)\n",
		       drifts.size());
		assert(drifts.empty() &&
		       "an empty backend must not produce capability drift");
	}

	// ── Case 5: orphan modules with entity rows but no imports ──
	// The `NOT EXISTS (import …)` test is vacuously true without imports, so
	// a scope with >=10 entities used to be reported as an orphan module.
	clearGraph(db);
	execOrDie(db, "INSERT INTO scope (project_id, parent_id, kind, name) "
		      "VALUES (" +
			      std::to_string(pid) + ",0,1,'src/')");
	for (int64_t i = 1; i <= 10; ++i)
		insertEntity(db, pid, i, 0, "f" + std::to_string(i),
			     "src/f" + std::to_string(i) + ".go", i);
	insertCallRelation(db, pid, 1, 2);
	{
		verify::DeadCodeInspector inspector(&store, pid);
		const size_t orphans = countDeadModules(inspector.inspect());
		printf("  [debug] orphan modules      -> %zu (no import rows)\n",
		       orphans);
		assert(orphans == 0 &&
		       "modules must not be called orphaned when the import "
		       "table is empty");
	}

	// ── Case 6: orphan detection still works with real import data ──
	// Guards against "fixed" by disabling the query: a module with entities
	// and no importing file is still reported once imports exist.
	// Must run BEFORE Case 5b: 5b's clearGraph drops the 10 entities this
	// case needs to clear the >=10-entity threshold.
	execOrDie(
		db,
		"INSERT INTO import (project_id, file_path, target_path, alias, "
		"source_scope_id) VALUES (" +
			std::to_string(pid) +
			",'other/consumer.go','x/y','',0)");
	{
		verify::DeadCodeInspector inspector(&store, pid);
		const size_t orphans = countDeadModules(inspector.inspect());
		printf("  [debug] orphan modules      -> %zu (imports present)\n",
		       orphans);
		assert(orphans > 0 &&
		       "with import rows present the orphan query must still "
		       "report src/");
	}

	// ── Case 5b: dead functions with an empty relation table ──
	// findOrphanFunctions's `NOT EXISTS (relation …)` test is vacuously
	// true for every non-public, non-entry-point function when relation
	// is empty, so a pre-index project would emit mass DeadFunction
	// findings at 0.90 confidence. The relation gate must suppress them.
	clearGraph(db);
	for (int64_t i = 1; i <= 5; ++i)
		insertEntity(db, pid, i, 0, "fn" + std::to_string(i),
			     "src/fn" + std::to_string(i) + ".go", i);
	{
		verify::DeadCodeInspector inspector(&store, pid);
		const auto all = inspector.inspect();
		const size_t dead = countDeadFunctions(all);
		printf("  [debug] dead functions      -> %zu (no relation rows)\n",
		       dead);
		assert(dead == 0 &&
		       "empty relation table must not produce DeadFunction "
		       "findings (vacuous NOT EXISTS)");
	}

	store.close();
	unlink(kDbPath);
	unlink((std::string(kDbPath) + "-wal").c_str());
	unlink((std::string(kDbPath) + "-shm").c_str());

	printf("\n=== test_verifier_evidence_gates PASSED ===\n");
	printf("Drift detectors gated; ThreadSafe no longer reads Block as a "
	       "lock and no longer contradicts on absence\n");
	return 0;
}

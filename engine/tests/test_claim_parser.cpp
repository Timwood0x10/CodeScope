// test_claim_parser: verify the ClaimParser extracts the expected claims
// from free-form text. Uses simple assert() + printf like the other tests
// in engine/tests.
#include "verify/claim_parser.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace verify;

// Helper: return true when `claims` contains a claim with the given type
// and subject (case-sensitive exact match on subject).
static bool hasClaim(const std::vector<Claim> &claims, ClaimType type,
		     const std::string &subject)
{
	for (const auto &c : claims) {
		if (c.type == type && c.subject == subject)
			return true;
	}
	return false;
}

// Helper: count claims of a given type.
static int countByType(const std::vector<Claim> &claims, ClaimType type)
{
	int n = 0;
	for (const auto &c : claims)
		if (c.type == type)
			++n;
	return n;
}

int main()
{
	ClaimParser parser;

	// ── Test 1: primary spec example ──────────────────────────────
	// "CodeScope supports incremental indexing and is thread-safe" must
	// produce a CapabilityExists claim with subject="IncrementalIndexing"
	// (PascalCase-normalized to match KnowledgeBuilder naming) AND a
	// ContractHolds claim with subject="ThreadSafe".
	{
		std::string text =
			"CodeScope supports incremental indexing and is "
			"thread-safe";
		auto claims = parser.parse(text, "readme", "README.md");

		bool has_cap = hasClaim(claims, ClaimType::CapabilityExists,
					"IncrementalIndexing");
		bool has_contract = hasClaim(claims, ClaimType::ContractHolds,
					     "ThreadSafe");

		assert(has_cap);
		assert(has_contract);
		printf("Test 1 (supports + thread-safe): PASS\n");
	}

	// ── Test 2: "implements" pattern ──────────────────────────────
	// Subject is PascalCase-normalized: "call graph analysis" → "CallGraphAnalysis"
	{
		std::string text = "The engine implements call graph analysis.";
		auto claims = parser.parse(text, "readme", "README.md");

		bool has_cap = hasClaim(claims, ClaimType::CapabilityExists,
					"CallGraphAnalysis");
		assert(has_cap);
		printf("Test 2 (implements): PASS\n");
	}

	// ── Test 3: all four contract keywords ───────────────────────
	{
		std::string text =
			"This library is memory-safe, zero-copy, and "
			"lock-free.";
		auto claims = parser.parse(text, "readme", "README.md");

		assert(hasClaim(claims, ClaimType::ContractHolds,
				"MemorySafe"));
		assert(hasClaim(claims, ClaimType::ContractHolds, "ZeroCopy"));
		assert(hasClaim(claims, ClaimType::ContractHolds, "LockFree"));
		printf("Test 3 (memory-safe/zero-copy/lock-free): PASS\n");
	}

	// ── Test 4: architecture arrow chain (whitelisted) ───────────
	{
		std::string text =
			"Layered design: Controller -> Service -> Repository";
		auto claims = parser.parse(text, "readme", "README.md");

		int arch_count =
			countByType(claims, ClaimType::ArchitectureFollows);
		assert(arch_count >= 1);

		// Verify the claim fields: subject=Controller,
		// predicate=flows_to, object=Service, scope=Repository.
		bool found = false;
		for (const auto &c : claims) {
			if (c.type == ClaimType::ArchitectureFollows &&
			    c.subject == "Controller" &&
			    c.predicate == "flows_to" &&
			    c.object == "Service" && c.scope == "Repository") {
				found = true;
				break;
			}
		}
		assert(found);
		printf("Test 4 (arrow chain): PASS\n");
	}

	// ── Test 5: arrow chain with non-layer tokens is ignored ─────
	{
		std::string text = "value -> next -> null";
		auto claims = parser.parse(text, "readme", "README.md");

		int arch_count =
			countByType(claims, ClaimType::ArchitectureFollows);
		assert(arch_count == 0);
		printf("Test 5 (non-layer arrow rejected): PASS\n");
	}

	// ── Test 6: empty text produces no claims ────────────────────
	{
		auto claims = parser.parse("", "readme", "README.md");
		assert(claims.empty());
		printf("Test 6 (empty text): PASS\n");
	}

	// ── Test 7: source_kind + source_ref stamped on every claim ──
	// Note: the subject must be at least 2 chars to match the regex
	// [A-Za-z][A-Za-z0-9_\- ]{1,50} in claim_parser.cpp.
	{
		std::string text = "supports XYZ";
		auto claims = parser.parse(text, "ai_summary", "sum-42");
		assert(!claims.empty());
		for (const auto &c : claims) {
			assert(c.source_kind == "ai_summary");
			assert(c.source_ref == "sum-42");
		}
		printf("Test 7 (source stamping): PASS\n");
	}

	printf("\n=== test_claim_parser PASSED ===\n");
	return 0;
}

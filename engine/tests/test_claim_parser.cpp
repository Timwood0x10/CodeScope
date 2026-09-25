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

	// ── Test 3b: negation must NOT become a positive claim ───────
	// Regression: "not thread-safe" / "isn't thread-safe" previously
	// produced ContractHolds(ThreadSafe) — polarity inversion that let
	// ContractVerifier answer Supported on the README's denial.
	// "cannot" is NOT a denial (it merely ends in the letters "not"):
	// the keyword claim must still be emitted, or the `\bnot` anchor
	// would over-suppress.
	{
		std::string neg1 = "This library is not thread-safe.";
		std::string neg2 = "The buffer isn't thread-safe under load.";
		std::string pos1 = "This design cannot thread-safe anything.";
		std::string pos2 = "This library is thread-safe.";

		auto c1 = parser.parse(neg1, "readme", "README.md");
		auto c2 = parser.parse(neg2, "readme", "README.md");
		auto c3 = parser.parse(pos1, "readme", "README.md");
		auto c4 = parser.parse(pos2, "readme", "README.md");

		assert(!hasClaim(c1, ClaimType::ContractHolds, "ThreadSafe"));
		assert(!hasClaim(c2, ClaimType::ContractHolds, "ThreadSafe"));
		assert(hasClaim(c3, ClaimType::ContractHolds, "ThreadSafe"));
		assert(hasClaim(c4, ClaimType::ContractHolds, "ThreadSafe"));
		printf("Test 3b (negation / cannot): PASS\n");
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

	// ── Test 8: "has <subject>" existence claim ──────────────────
	// "The project has a LeaderAgent class" must produce a
	// CapabilityExists claim whose subject is the identity token
	// "LeaderAgent" — the article and the trailing generic noun
	// ("class") are not part of the name.
	{
		std::string text = "The project has a LeaderAgent class.";
		auto claims = parser.parse(text, "ai_summary", "sum-8");
		assert(hasClaim(claims, ClaimType::CapabilityExists,
				"LeaderAgent"));
		assert(!hasClaim(claims, ClaimType::CapabilityExists,
				 "LeaderAgentClass"));
		printf("Test 8 (has + trailing noun strip): PASS\n");
	}

	// ── Test 9: "should" behavioral claim ────────────────────────
	// "c_print should handle null input" must produce a
	// FunctionImplements claim whose subject is the VERBATIM function
	// name (PascalCase-mangling would break the entity lookup) and
	// whose object is the behavior phrase.
	{
		std::string text = "c_print should handle null input.";
		auto claims = parser.parse(text, "ai_summary", "sum-9");
		assert(hasClaim(claims, ClaimType::FunctionImplements,
				"c_print"));
		assert(!hasClaim(claims, ClaimType::FunctionImplements,
				 "CPrint"));
		bool object_ok = false;
		for (const auto &c : claims) {
			if (c.type == ClaimType::FunctionImplements &&
			    c.subject == "c_print" &&
			    c.object.rfind("handle", 0) == 0)
				object_ok = true;
		}
		assert(object_ok);
		printf("Test 9 (should -> FunctionImplements): PASS\n");
	}

	// ── Test 10: negated has/should must NOT become positive ────
	// "has no X" / "doesn't have X" / "should never X" / "should not
	// X" are denials; emitting a positive claim would invert the
	// source polarity (the same failure mode Test 3b pins for the
	// keyword contracts).
	{
		std::string t1 = "The project has no LeaderAgent class.";
		std::string t2 = "The project doesn't have a LeaderAgent.";
		std::string t3 = "c_print should never dereference null.";
		std::string t4 = "c_print should not crash on empty input.";
		auto c1 = parser.parse(t1, "ai_summary", "sum-10a");
		auto c2 = parser.parse(t2, "ai_summary", "sum-10b");
		auto c3 = parser.parse(t3, "ai_summary", "sum-10c");
		auto c4 = parser.parse(t4, "ai_summary", "sum-10d");
		assert(!hasClaim(c1, ClaimType::CapabilityExists, "LeaderAgent"));
		assert(!hasClaim(c2, ClaimType::CapabilityExists, "LeaderAgent"));
		assert(!hasClaim(c3, ClaimType::FunctionImplements, "c_print"));
		assert(!hasClaim(c4, ClaimType::FunctionImplements, "c_print"));
		printf("Test 10 (negated has/should suppressed): PASS\n");
	}

	printf("\n=== test_claim_parser PASSED ===\n");
	return 0;
}

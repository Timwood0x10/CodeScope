#ifndef CODESCOPE_CLAIM_H
#define CODESCOPE_CLAIM_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace verify
{

// Claim type drives Verifier selection in the VerifierRegistry.
// Values are persisted in the `claim.claim_type` column as integers, so
// existing rows must keep their numeric mapping. Append new types only.
enum class ClaimType : uint8_t {
	CapabilityExists = 0, // "X is implemented"
	ContractHolds = 1, // "codebase is thread-safe"
	ArchitectureFollows = 2, // "Controller -> Service -> Repository"
	FunctionImplements = 3, // "foo() does X"
};

// Verdict is the outcome of evidence collection for a single Claim.
// Persisted in `evidence.verdict`. 0/1/2 mapping is stable.
enum class Verdict : uint8_t {
	Supported = 0,
	Contradicted = 1,
	Unknown = 2,
};

// Claim is the unified intermediate representation consumed by every Verifier.
// `subject`/`predicate`/`object` form a (subject, predicate, object) triple,
// e.g. ("IncrementalIndex", "implemented_by", "Runtime"). `scope` narrows
// the verification context ("repository" by default). `source_kind` records
// where the claim originated ("readme" / "ai_summary" / "pr" / "manual")
// and `source_ref` holds a file path or summary fragment for traceability.
struct Claim {
	ClaimType type = ClaimType::CapabilityExists;
	std::string subject;
	std::string predicate;
	std::string object;
	std::string scope = "repository";
	std::string source_kind;
	std::string source_ref;
};

// EvidenceRecord is the output of Verifier::verify(Claim). It is persisted
// into the `evidence` table, and each fact reference is written to
// `evidence_fact` for traceability back to graph_nodes/graph_edges rows.
//
// `facts` is a list of (fact_kind, fact_ref) pairs where:
//   fact_kind 0 = graph_node, 1 = graph_edge, 2 = document
//   fact_ref   graph_nodes.id / graph_edges.id / document rowid

// Fact kind constants shared across all verifiers.
inline constexpr int kFactKindNode = 0;
inline constexpr int kFactKindEdge = 1;
inline constexpr int kFactKindDocument = 2;

struct EvidenceRecord {
	int64_t claim_id = 0;
	Verdict verdict = Verdict::Unknown;
	double confidence = 0.0;
	std::string verifier_name;
	std::string detail;
	std::vector<std::pair<int, int64_t>> facts;
};

// Human-readable name for a ClaimType (e.g. "CapabilityExists").
const char *claimTypeName(ClaimType t);

// Human-readable name for a Verdict (e.g. "Supported").
const char *verdictName(Verdict v);

} // namespace verify

#endif // CODESCOPE_CLAIM_H

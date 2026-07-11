#include "architecture_verifier.h"
#include "claim.h"
#include "../store/store.h"

namespace verify
{

ArchitectureVerifier::ArchitectureVerifier(store::GraphStore *store,
					   uint64_t project_id)
	: store_(store)
	, project_id_(project_id)
{
}

bool ArchitectureVerifier::accepts(const Claim &claim) const
{
	return claim.type == ClaimType::ArchitectureFollows;
}

// TODO: Implement real architecture verification.
//
// Intended design (for a future iteration):
//
// 1. Parse the claim's layered flow. An ArchitectureFollows claim carries
//    the layers in its fields:
//      subject   = source layer (e.g. "Controller")
//      predicate = "flows_to"
//      object    = intermediate layer (e.g. "Service")
//      scope     = sink layer (e.g. "Repository")
//    So the claim "Controller -> Service -> Repository" is encoded as
//    subject="Controller", object="Service", scope="Repository".
//
// 2. Map each layer name to a set of entity name patterns. A layer is
//    identified by module path or naming convention:
//      Controller  -> entities whose name ends with "Controller" or whose
//                     module path contains /controllers/ or /api/
//      Service      -> entities whose name ends with "Service" or whose
//                     module path contains /services/
//      Repository   -> entities whose name ends with "Repository",
//                     "Repo", "Store", or "DAO", or whose module path
//                     contains /repository/ or /data/
//    A config-driven mapping (e.g. a JSON table) would be more flexible
//    than hard-coded patterns; for v0.3 hard-coded is acceptable.
//
// 3. For each (source_layer, sink_layer) pair in the claimed flow, verify
//    that no entity in sink_layer calls an entity in source_layer. The
//    call graph is read from the `relation` table (type=1 = CALLS). A
//    single SQL query per pair can detect violations:
//      SELECT COUNT(*) FROM relation r
//      JOIN entity src ON r.target_id = src.id
//      JOIN entity dst ON r.source_id = dst.id
//      WHERE r.project_id=? AND r.type=1
//        AND <src matches source_layer>
//        AND <dst matches sink_layer>
//    If COUNT > 0, the flow is violated (sink calls source).
//
// 4. Also verify that the flow is connected: at least one entity in
//    source_layer should call at least one entity in the next layer. If
//    there is no edge at all, the claim is vacuously true but useless —
//    return Unknown with a low confidence.
//
// 5. Verdict:
//      - No violations + connected flow  -> Supported, confidence 0.8
//      - Violations found                -> Contradicted, confidence 0.75,
//                                           facts = violating relation ids
//      - Layers not found in codebase    -> Unknown, confidence 0.4,
//                                           detail = layer not detected
//      - No edges between layers         -> Unknown, confidence 0.5
//
// 6. Persist fact references:
//      - fact_kind 1 (relation) for violating edges
//      - fact_kind 0 (entity) for layer-member entities
//
// Until the above is implemented, every ArchitectureFollows claim returns
// Verdict::Unknown so the registry can still dispatch it without crashing.
EvidenceRecord ArchitectureVerifier::verify(const Claim &claim)
{
	EvidenceRecord rec;
	rec.claim_id = 0;
	rec.verdict = Verdict::Unknown;
	rec.confidence = 0.0;
	rec.verifier_name = "ArchitectureVerifier";
	rec.detail = "ArchitectureVerifier not yet implemented; claim '";
	rec.detail += claim.subject;
	rec.detail += "' cannot be verified";
	return rec;
}

} // namespace verify

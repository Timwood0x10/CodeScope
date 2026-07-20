#ifndef CODESCOPE_MODEL_PROJECT_STATE_BUILDER_H
#define CODESCOPE_MODEL_PROJECT_STATE_BUILDER_H

#include <cstdint>
#include <string>

#include "../store/store.h"

namespace model
{

// ─── Project State Snapshot (Phase 4) ───────────────────────────────
//
// ProjectStateBuilder aggregates all per-project evidence + state rows
// into a single project_state row: an overall confidence score and a
// JSON snapshot describing what inspectors ran and what they found.
//
// The snapshot is consumed by the MCP layer to give a fast overview
// of project quality without re-running the full analysis pipeline.
//
// The builder borrows the GraphStore pointer from the caller; the
// caller must keep it alive for the lifetime of the builder. All
// sqlite3_stmt handles are managed by an RAII guard (StmtGuard) so
// they are finalized on every exit path.
//
// build() follows plan section 6.2:
//   1. Run evidence::EvidenceBuilder::buildAll to get all evidence.
//   2. Aggregate by category: count issues + collect titles.
//   3. Query semantic_fact for summary counts per category.
//   4. Query capability / architecture_state / workflow_state /
//      module_summary for state counts.
//   5. Compute overall confidence: start at 1.0, subtract penalty per
//      issue, clamp to [0.0, 1.0].
//   6. Build snapshot_json (see plan §6.2 for the schema).
//   7. UPSERT into project_state.

/// Confidence penalty constants. Each category subtracts a fixed
/// amount per issue from the overall score (clamped to [0,1]).
/// Values are conservative starting points; tune against real
/// projects without touching the build logic.
constexpr double kPenaltyPerSyncIssue = 0.05;
constexpr double kPenaltyPerMemoryIssue = 0.10;
constexpr double kPenaltyPerErrorIssue = 0.04;
constexpr double kPenaltyPerPatternIssue = 0.03;
constexpr double kPenaltyPerFfiIssue = 0.04;
constexpr double kPenaltyPerArchitectureViolation = 0.02;
constexpr double kPenaltyPerDeadEntity = 0.005;

/// Maximum number of detail strings to embed per category in the
/// snapshot JSON. Keeps the snapshot small even for projects with
/// hundreds of issues per category.
constexpr size_t kMaxDetailsPerCategory = 8;

/// ProjectStateBuilder produces and persists a project_state row.
class ProjectStateBuilder {
    public:
	/// Construct with the store handle used for both reads (evidence,
	/// capability, etc.) and writes (project_state UPSERT). The
	/// pointer is borrowed; the caller owns it and must keep it
	/// alive for the lifetime of the builder.
	explicit ProjectStateBuilder(store::GraphStore *store)
		: store_(store)
	{
	}

	/// Compute the project state snapshot and persist it to the
	/// project_state table (UPSERT on project_id). Returns true on
	/// success, false on any error (logged to stderr with the
	/// [module=project_state, method=build] trace chain).
	///
	/// Steps:
	///   1. Run evidence::EvidenceBuilder::buildAll.
	///   2. Aggregate evidence by category.
	///   3. Query state tables (capability / architecture_state /
	///      workflow_state / module_summary) for counts.
	///   4. Compute overall confidence in [0.0, 1.0].
	///   5. Build snapshot_json.
	///   6. UPSERT into project_state.
	///
	/// @param project_id  Project to analyze.
	/// @return true on success.
	bool build(uint64_t project_id);

	/// Read the persisted snapshot_json for a project. Returns an
	/// empty string if no project_state row exists, or on error.
	///
	/// @param project_id  Project to read.
	/// @return snapshot_json string, or empty string if not found.
	std::string getSnapshotJson(uint64_t project_id) const;

	/// Read the persisted confidence for a project. Returns 0.0 if
	/// no project_state row exists, or on error.
	///
	/// @param project_id  Project to read.
	/// @return confidence in [0.0, 1.0], or 0.0 if not found.
	double getConfidence(uint64_t project_id) const;

    private:
	store::GraphStore *store_;
};

} // namespace model

#endif // CODESCOPE_MODEL_PROJECT_STATE_BUILDER_H

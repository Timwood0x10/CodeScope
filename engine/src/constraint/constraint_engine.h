#ifndef CODESCOPE_CONSTRAINT_CONSTRAINT_ENGINE_H
#define CODESCOPE_CONSTRAINT_CONSTRAINT_ENGINE_H

#include <cstdint>
#include <string>
#include <vector>

namespace store
{
class GraphStore;
}

namespace constraint
{

/// One declared engineering constraint (development_plan §18.2.2-B).
///
/// A constraint is one rule from .codescope/profile.yaml: a dependency
/// direction restriction, a metric threshold, or a coupling limit.
/// The ConstraintEngine loads them, validates the YAML structure, and
/// persists them to constraint_decl / arch_layer_decl for the current
/// constraint_digest.
struct DeclaredConstraint {
	/// Unique id from the YAML (e.g. "no_storage_calls_api").
	std::string id;
	/// Constraint type: "dependency_direction", "size_check", etc.
	std::string type;
	/// Type-specific parameters serialized as JSON.
	std::string params_json;
	/// Severity 0-4 (development_plan §10.2).
	int severity = 2;
	/// Evidence level ceiling: L1, L2, or L3.
	std::string evidence_level = "L1";
	/// Whether the constraint is active.
	bool enabled = true;
};

/// One layer membership declaration.
struct DeclaredLayer {
	std::string layer_name;
	std::string path_pattern;
};

/// Result of loading and validating a constraint declaration file.
struct ConstraintLoadResult {
	/// SHA-256 digest of the normalized declaration content.
	std::string constraint_digest;
	/// Loaded constraints (empty if no file found).
	std::vector<DeclaredConstraint> constraints;
	/// Loaded layer declarations.
	std::vector<DeclaredLayer> layers;
	/// ISO timestamp of the load.
	std::string declared_at;
	/// Error message; empty on success.
	std::string error;
	/// True when no .codescope/profile.yaml was found (not an error).
	bool no_file = false;
};

/// Load and validate engineering constraints from .codescope/profile.yaml
/// (or .codescope/profile.json) in the project root.
///
/// The loader reads the file, parses its structure, validates:
/// - layer path patterns are project-relative and non-empty
/// - constraint ids are unique
/// - evidence_level is L1/L2/L3 (cannot upgrade L2 to L1)
/// - thresholds are positive numbers where applicable
/// - no duplicate layer names with conflicting paths
///
/// On success, the constraints and layers are persisted to
/// constraint_decl and arch_layer_decl tables keyed by
/// constraint_digest. On failure, the error is returned and no
/// rows are written.
///
/// @param store        The GraphStore to persist constraints into.
/// @param project_id   The project to load constraints for.
/// @param project_root The project root directory path.
/// @return Load result with constraints, layers, digest, or error.
ConstraintLoadResult loadConstraints(store::GraphStore &store,
				     uint64_t project_id,
				     const std::string &project_root);

/// Compute the SHA-256 digest of the normalized constraint content.
/// Used by loadConstraints and by callers that need to compare digests
/// without re-reading the file.
std::string computeConstraintDigest(
	const std::vector<DeclaredConstraint> &constraints,
	const std::vector<DeclaredLayer> &layers);

/// Look up the active constraint_digest for a project (the most recent
/// successfully loaded set). Returns empty string when no constraints
/// have been loaded.
std::string activeConstraintDigest(store::GraphStore &store,
				   uint64_t project_id);

/// Load declared layers for a project + digest from the database.
/// Used by the ArchitectureVerifier to check declared layer violations.
std::vector<DeclaredLayer> loadDeclaredLayers(store::GraphStore &store,
					      uint64_t project_id,
					      const std::string &digest);

/// Load declared constraints for a project + digest from the database.
std::vector<DeclaredConstraint>
loadDeclaredConstraints(store::GraphStore &store, uint64_t project_id,
			const std::string &digest);

} // namespace constraint

#endif // CODESCOPE_CONSTRAINT_CONSTRAINT_ENGINE_H

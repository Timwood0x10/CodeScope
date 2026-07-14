#ifndef CODESCOPE_MODEL_PLUGIN_H
#define CODESCOPE_MODEL_PLUGIN_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace model
{

/// Relation type value for call edges (mirrors graph::EdgeType::Calls = 1).
inline constexpr int kRelationTypeCall = 1;

/// Scope kind value for module-level scopes.
inline constexpr int kScopeKindModule = 1;

/// Document type value for README documents.
inline constexpr int kDocumentTypeReadme = 0;

/// Entity kind for function nodes (mirrors graph::NodeType::Function = 0).
inline constexpr int kEntityKindFunction = 0;

/// Entity kind for method nodes (mirrors graph::NodeType::Method = 1).
inline constexpr int kEntityKindMethod = 1;

/// Maximum callee steps traced per workflow (matches original LIMIT 20).
inline constexpr int kMaxCalleesPerWorkflow = 20;

/// Maximum TODO/FIXME entities scanned for contracts (matches original
/// LIMIT 20).
inline constexpr int kMaxTodoEntities = 20;

/// Maximum README documents fetched per project (matches original LIMIT 5).
inline constexpr int kMaxReadmeDocuments = 5;

/// Result of a model plugin's build step.
struct ModelResult {
	std::string plugin_name;
	int64_t items_created = 0;
	std::string error;
	bool ok() const
	{
		return error.empty();
	}
};

/// Read-only snapshot of an entity row, pre-fetched by ModelEngine so that
/// plugins can iterate in-memory instead of issuing per-plugin SQL scans.
struct EntityInfo {
	uint64_t id = 0;
	std::string name;
	std::string file_path;
	std::string qualified_name;
	std::string module_path;
	int kind = 0;
	uint64_t project_id = 0;
};

/// Read-only snapshot of a relation row.
struct RelationRow {
	uint64_t id = 0;
	uint64_t source_id = 0;
	uint64_t target_id = 0;
	int type = 0;
	uint64_t project_id = 0;
};

/// Read-only snapshot of a scope row (module-level scopes, kind == 1).
struct ScopeInfo {
	uint64_t id = 0;
	std::string name;
	int kind = 0;
	uint64_t project_id = 0;
};

/// Read-only snapshot of a README document row.
struct DocumentInfo {
	std::string file_path;
	std::string content;
};

/// Pre-fetched, read-only snapshot of the entity / relation / scope /
/// document tables for a single project. Populated once by
/// ModelEngine::runAll and shared across all plugins to avoid N sequential
/// SQL scans of the entity and relation tables.
struct ModelContext {
	/// All entities for the project, keyed by entity id.
	std::unordered_map<uint64_t, EntityInfo> entities_by_id;
	/// Entity ids in SELECT (id-ascending) order, for ordered iteration
	/// that matches SQLite's natural scan order.
	std::vector<uint64_t> entity_ids_ordered;
	/// Call edges (relation.type == kRelationTypeCall), in relation id
	/// order.
	std::vector<RelationRow> call_edges;
	/// All relations (every type), in relation id order.
	std::vector<RelationRow> all_relations;
	/// Module scopes (scope.kind == kScopeKindModule), in scope id order.
	std::vector<ScopeInfo> scope_modules;
	/// README documents (document.type == kDocumentTypeReadme, non-empty
	/// content), limited to kMaxReadmeDocuments rows.
	std::vector<DocumentInfo> documents;
};

/// ModelPlugin interface: each plugin reads from Facts + Resolution layers
/// and writes to the Model layer (workflow, capability, etc.).
///
/// Plugins are registered in ModelEngine and run after the Resolver Pipeline.
class ModelPlugin {
    public:
	virtual ~ModelPlugin() = default;

	/// Name of this plugin (e.g. "Workflow", "Capability").
	virtual const char *name() const = 0;

	/// Build models for the given project using the pre-fetched
	/// ModelContext snapshot. INSERTs still go through the store but
	/// reads are served from the in-memory context.
	/// @param project_id  The project to analyze.
	/// @param ctx         Pre-fetched entity/relation/scope/document
	///                    snapshot.
	/// @return ModelResult with items_created count or error message.
	virtual ModelResult build(uint64_t project_id,
				  const ModelContext &ctx) = 0;
};

} // namespace model

#endif // CODESCOPE_MODEL_PLUGIN_H

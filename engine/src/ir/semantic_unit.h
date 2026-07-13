#ifndef SEMANTIC_UNIT_H
#define SEMANTIC_UNIT_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ir
{

/**
 * Source location range within a file.
 * All fields are 0-based (matching tree-sitter TSPoint convention).
 */
struct SourceRange {
	uint32_t start_row = 0;
	uint32_t start_col = 0;
	uint32_t end_row = 0;
	uint32_t end_col = 0;

	bool isValid() const
	{
		return start_row <= end_row ||
		       (start_row == end_row && start_col <= end_col);
	}
};

/**
 * Kind of a semantic record.
 * Only semantically meaningful nodes get a record — structural wrappers
 * (blocks, parentheses, expression statements) are elided during visiting.
 *
 * Type-related kinds (TypeDecl, TypeRef, TypeAssign) are used by the
 * type registry system to track type definitions, references, and
 * variable-to-type assignments throughout the knowledge graph.
 */
enum class RecordKind : uint8_t {
	Function,
	Method,
	Class,
	Interface,
	Enum,
	TypeAlias,
	Variable,
	Field,
	Parameter,
	CallExpr,
	MemberExpr,
	Import,
	Export,
	Literal,
	Comment,
	TranslationUnit,
	/// Type definition: struct/class/enum/trait/interface declaration.
	/// The `name` field stores the type name (e.g. "User", "Vec<T>").
	/// The `type_name` field stores the qualified name if applicable.
	TypeDecl,
	/// Type reference: a usage of a type (e.g. parameter type, field type).
	/// The `name` field stores the variable/parameter name.
	/// The `type_name` field stores the referenced type name.
	TypeRef,
	/// Type assignment: a variable/parameter/field is assigned a type.
	/// The `name` field stores the variable name.
	/// The `type_name` field stores the assigned type name.
	TypeAssign,
	/// Route registration: HTTP route handler (e.g. r.GET("/api", handler)).
	/// The `name` field stores the HTTP method + path (e.g. "GET /api/users").
	/// The `qualified_name` field stores the handler function name.
	Route,
};

/**
 * A single semantic fact extracted from the AST.
 * No pointers to other records — hierarchy is expressed via parent_id.
 * No children vector — tree structure is reconstructed from parent_id links.
 *
 * The `type_name` field stores the type associated with this record:
 *   - For Variable/Field/Parameter: the declared type (e.g. "int", "User")
 *   - For TypeDecl: the qualified type name
 *   - For TypeRef: the referenced type name
 *   - For TypeAssign: the assigned type
 *   - For other kinds: empty string (unused)
 */
struct Record {
	uint64_t id = 0; // internal sequential ID
	uint64_t original_id = 0; // external/original ID (may differ from id)
	RecordKind kind;
	std::string name;
	std::string qualified_name;
	std::string type_name; // type associated with this record (see above)
	uint64_t parent_id = 0; // 0 = top-level (child of TranslationUnit)
	uint64_t ref_original_id =
		0; // for CallExpr: resolved callee's original_id (0 = unresolved cross-file)
	SourceRange loc;
	std::string file_path;
	std::string language;
	int arity = 0; // number of parameters (for call resolution)
	bool is_static = false; // static function/method (C++/Rust/Java)
};

/**
 * Lightweight container for semantic facts extracted from one source file.
 *
 * Design principles:
 * - Flat vector of records (no tree, no Node objects, no children pointers)
 * - Parent-child relationships via parent_id (0 = root)
 * - Contiguous memory, cache-friendly iteration
 * - No heap-allocated per-node metadata (no std::vector per record)
 *
 * Typical size: ~50-200 bytes per record, ~50 KB for a 1000-line file.
 */
class SemanticUnit {
    public:
	SemanticUnit() = default;

	/**
	 * Pre-allocate capacity for expected number of records.
	 * Reduces vector reallocation overhead during batch insertion.
	 * Call before addRecord() when the approximate count is known.
	 */
	void reserve(size_t count)
	{
		records_.reserve(count);
		id_to_index_.reserve(count);
	}

	// ── Record Management ─────────────────────────────────────

	/** Add a record and return its assigned ID. */
	uint64_t addRecord(RecordKind kind, const std::string &name,
			   uint64_t parent_id, SourceRange loc, int arity = 0,
			   bool is_static = false);

	/**
	 * Add a typed record (Variable/Field/Parameter/TypeRef/TypeAssign with type info).
	 * The `type_name` field stores the type associated with this record.
	 * Returns the assigned ID.
	 */
	uint64_t addTypedRecord(RecordKind kind, const std::string &name,
				const std::string &type_name,
				uint64_t parent_id, SourceRange loc);

	/**
	 * Add a record with explicit original_id and qualified_name (for DB rebuild).
	 * The original_id allows parent_id links to be preserved across rebuilds.
	 * Returns the internally-assigned ID (may differ from original_id).
	 */
	uint64_t addRecord(RecordKind kind, const std::string &name,
			   const std::string &qualified_name,
			   uint64_t original_id, uint64_t parent_id,
			   SourceRange loc);

	/** Accessors. */
	const Record &getRecord(uint64_t id) const;
	const std::vector<Record> &allRecords() const
	{
		return records_;
	}
	size_t size() const
	{
		return records_.size();
	}
	bool empty() const
	{
		return records_.empty();
	}

	// ── Metadata ───────────────────────────────────────────────

	const std::string &filePath() const
	{
		return file_path_;
	}
	void setFilePath(const std::string &path)
	{
		file_path_ = path;
	}

	const std::string &language() const
	{
		return language_;
	}
	void setLanguage(const std::string &lang)
	{
		language_ = lang;
	}

	// ── Query ──────────────────────────────────────────────────

	/** Find records by kind. Returns indices into records_. */
	std::vector<size_t> findRecordsByKind(RecordKind kind) const;

	/** Find a record by name (first match, or SIZE_MAX if not found). */
	size_t findRecordByName(const std::string &name) const;

	/** Get children of a record (records whose parent_id == id). */
	std::vector<size_t> getChildren(uint64_t parent_id) const;

	/**
	 * Set the resolved callee reference (ref_original_id) on a CallExpr record.
	 * Enables precise intra-file call resolution in GraphBuilder: when set,
	 * the builder looks up the callee by this ID instead of name matching.
	 * \param record_id        ID of the CallExpr record to update.
	 * \param ref_original_id  original_id of the resolved callee (0 = unresolved).
	 * \return true if the record was found and updated, false otherwise.
	 */
	bool setCallReference(uint64_t record_id, uint64_t ref_original_id);

    private:
	std::vector<Record> records_;
	std::unordered_map<uint64_t, size_t>
		id_to_index_; // record.id → index in records_
	std::string file_path_;
	std::string language_;
	uint64_t next_id_ = 1;
};

} // namespace ir

#endif // SEMANTIC_UNIT_H

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
	/// Interface implementation: struct/class implements an interface/trait.
	/// The `name` field stores the implementing type name.
	/// The `type_name` field stores the interface/trait name.
	InterfaceImpl,
};

/// Kind of a call expression — used to distinguish direct calls from
/// interface dispatches, constructor calls, and method calls.
/// Helps the resolver pipeline avoid false-positive cross-module edges.
enum class CallKind : uint8_t {
	Direct = 0, // bare function call: doThing()
	Method = 1, // method call on a concrete receiver: obj.Method()
	Interface = 2, // interface dispatch: iface.Method() (Go/Java/C#)
	Constructor = 3, // constructor/factory call: NewService()
	StaticMethod =
		4, // static method call: ClassName.staticMethod() (Java/C++)
	Virtual = 5, // virtual dispatch / trait method (C++/Rust)
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
	/// Resolution strategy for this call record.
	/// Empty string = not a call or not yet resolved.
	/// "p1_intra"  = resolved within same file (ref_original_id > 0)
	/// "p3_name"   = resolved by name match across files
	/// "external"  = resolved to a known builtin/third-party symbol
	/// "unresolved" = could not resolve to any known symbol
	std::string resolve_strategy;
	SourceRange loc;
	std::string file_path;
	std::string language;
	int arity = 0; // number of parameters (for call resolution)
	bool is_static = false; // static function/method (C++/Rust/Java)
	CallKind call_kind = CallKind::
		Direct; // for CallExpr: direct/interface/constructor/method
	/// Visibility level for this declaration (v0.2.2 role classifier signal).
	/// 0 = private (default), 1 = pub/public/export, 2 = protected (Java/C#).
	/// Populated by Visitors per language; flows Record → SemanticUnit →
	/// entity.visibility column via the staging pipeline. The role classifier
	/// in state_builder.cpp fuses pub_count (visibility=1) with call-graph
	/// counts — see docs/dev_plans/role_classifier_plan.md.
	int visibility = 0;

	// ── Call fact fields (Step 3, plan §3.1) ────────────────────────
	// Populated by per-language Visitors for CallExpr records so the
	// Resolver Pipeline can disambiguate method/static/constructor calls
	// using structured evidence instead of bare-name + directory heuristics.
	// All fields default to empty (unknown). Direct calls must NOT
	// fabricate a receiver — an empty receiver_text means "no receiver"
	// (a bare function call), which is itself a meaningful signal.

	/// Full qualified call target text as written in source, e.g. "b.Get",
	/// "fmt.Println", "Type::method", "self.handler". For a bare call
	/// `alpha()` this is empty (the bare name already lives in `name`).
	/// Keeping the full qualifier lets the Resolver distinguish
	/// `pkg.Func()` from `obj.Method()` and from `Func()`.
	std::string qualified_target;
	/// Receiver expression text as written, e.g. "b", "fmt", "obj", "self".
	/// Empty for bare/free function calls. For `Type::staticMethod()` the
	/// receiver is "Type". This is the syntactic receiver only — its
	/// inferred type lives in receiver_type.
	std::string receiver_text;
	/// Statically inferred type of the receiver, e.g. "Box" for `b.Get()`
	/// when `b` is declared `Box b`. Empty when the type cannot be
	/// determined from local context (dynamic receivers, unknown variable).
	/// The Resolver uses this as the PRIMARY evidence for method-target
	/// disambiguation (replacing the old directory heuristic).
	std::string receiver_type;
	/// Import alias used in the call, if any. For `pkg.Func()` where `pkg`
	/// is an imported alias, this stores "pkg" and the canonical import
	/// target is resolved via the import table. Empty for calls that do
	/// not go through an import alias.
	std::string import_alias;
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
			   bool is_static = false, int visibility = 0);

	/**
	 * Add a typed record (Variable/Field/Parameter/TypeRef/TypeAssign with type info).
	 * The `type_name` field stores the type associated with this record.
	 * Returns the assigned ID.
	 */
	uint64_t addTypedRecord(RecordKind kind, const std::string &name,
				const std::string &type_name,
				uint64_t parent_id, SourceRange loc,
				int visibility = 0);

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
	// Returns a pointer to the record with the given id, or nullptr if not
	// found (e.g. when records_ is empty or the id was never added).
	// Returning a pointer avoids the undefined behaviour of calling
	// records_.back() on an empty container and forces callers to handle
	// the "not found" case explicitly.
	const Record *getRecord(uint64_t id) const;
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

	/**
	  * Set the call_kind on a CallExpr record (0=direct, 1=method, 2=interface,
	  * 3=constructor, 4=static, 5=virtual). Allows the parser to classify
	  * calls before persistence, enabling the resolver pipeline to
	  * distinguish direct invocations from interface dispatches.
	  * \param record_id  ID of the CallExpr record to update.
	  * \param kind       CallKind value to assign.
	  * \return true if the record was found and updated.
	  */
	bool setCallKind(uint64_t record_id, int kind);

	/**
	  * Set the resolve_strategy on a CallExpr record.
	  * Strategy values:
	  *   "p1_intra"   — resolved within same file via ref_original_id
	  *   "p3_name"    — resolved by name match across files
	  *   "external"   — known builtin/third-party library symbol
	  *   "unresolved" — could not resolve to any known symbol
	  * \param record_id ID of the CallExpr record to update.
	  * \param strategy  Resolution strategy string.
	  * \return true if the record was found and updated.
	  */
	bool setCallStrategy(uint64_t record_id, const std::string &strategy);

	/**
	 * Set the structured call facts on a CallExpr record (Step 3, plan §3.1).
	 * Visitors call this after emitCall() to attach the receiver/qualified
	 * target/import alias evidence that the Resolver uses for exact-first
	 * method disambiguation. Empty strings mean "unknown"; callers must NOT
	 * fabricate a receiver for bare direct calls.
	 * \param record_id        ID of the CallExpr record to update.
	 * \param qualified_target Full qualified call text (e.g. "b.Get").
	 * \param receiver_text    Syntactic receiver expression (e.g. "b").
	 * \param receiver_type    Inferred receiver type (e.g. "Box"); empty if unknown.
	 * \param import_alias     Import alias used in the call, if any.
	 * \return true if the record was found and updated.
	 */
	bool setCallFacts(uint64_t record_id,
			  const std::string &qualified_target,
			  const std::string &receiver_text,
			  const std::string &receiver_type,
			  const std::string &import_alias);

	/**
	 * Set the qualified_name on any record (Step 4/5, plan §4B/§4C).
	 * Visitors call this after emitFunction/emitMethod when the function
	 * is declared inside a class, so the qualified_name includes the
	 * class prefix (e.g. "Timeline.render", "Point::helper"). The
	 * Resolver's factorReceiverTypeMatch uses this to match a call's
	 * receiver_type against the candidate's declaring class.
	 * \param record_id      ID of the record to update.
	 * \param qualified_name The qualified name (e.g. "Box::draw").
	 * \return true if the record was found and updated.
	 */
	bool setQualifiedName(uint64_t record_id,
			      const std::string &qualified_name);

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

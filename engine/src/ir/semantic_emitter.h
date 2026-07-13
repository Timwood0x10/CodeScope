#ifndef SEMANTIC_EMITTER_H
#define SEMANTIC_EMITTER_H

#include <cstdint>
#include <string>

#include "semantic_unit.h"

namespace ir
{

/**
 * Emitter interface: bridges AST Visitor → SemanticUnit.
 *
 * The Visitor walks a tree-sitter CST and calls emit*() methods for each
 * semantically meaningful node. The Emitter appends flat records to a
 * SemanticUnit — no tree, no Node objects, no children vectors.
 *
 * Structural wrappers (blocks, parentheses, expression statements) are
 * handled by the Visitor recursing without calling emit.
 */
class SemanticEmitter {
    public:
	explicit SemanticEmitter(SemanticUnit *unit);

	// ── Declaration Emitters ─────────────────────────────────

	uint64_t emitFunction(const std::string &name, SourceRange loc,
			      uint64_t parent_id = 0, int arity = 0,
			      bool is_static = false);
	uint64_t emitMethod(const std::string &name, SourceRange loc,
			    uint64_t parent_id = 0, int arity = 0,
			    bool is_static = false);
	uint64_t emitClass(const std::string &name, SourceRange loc,
			   uint64_t parent_id = 0);
	uint64_t emitInterface(const std::string &name, SourceRange loc,
			       uint64_t parent_id = 0);
	uint64_t emitEnum(const std::string &name, SourceRange loc,
			  uint64_t parent_id = 0);
	uint64_t emitTypeAlias(const std::string &name, SourceRange loc,
			       uint64_t parent_id = 0);
	uint64_t emitVariable(const std::string &name, SourceRange loc,
			      uint64_t parent_id = 0);

	// ── Expression Emitters ──────────────────────────────────

	uint64_t emitCall(const std::string &callee_name, SourceRange loc,
			  uint64_t parent_id = 0);
	uint64_t emitMemberAccess(const std::string &name, SourceRange loc,
				  uint64_t parent_id = 0);

	// ── Module Emitters ──────────────────────────────────────

	uint64_t emitImport(const std::string &module_name, SourceRange loc,
			    uint64_t parent_id = 0);
	uint64_t emitExport(const std::string &name, SourceRange loc,
			    uint64_t parent_id = 0);

	// ── Scope Emitters ─────────────────────────────────────────

	/// Emit a scope entry/exit. kind: 0=enter, 1=exit.
	/// scope_kind: 0=Global, 1=Module, 2=Function, 3=Block, 4=Trait, 5=Impl
	void emitScope(int kind, int scope_kind, const std::string &name,
		       SourceRange loc);

	// ── Reference Emitter ──────────────────────────────────────

	/// Emit a call reference (no resolution).
	uint64_t emitReference(const std::string &callee_name, SourceRange loc,
			       uint64_t parent_id = 0, int arity = 0);

	// ── Route Emitter ───────────────────────────────────────────

	/// Emit an HTTP route registration (e.g. "GET /api/users").
	/// \param route_label  HTTP method + path, e.g. "GET /api/users"
	/// \param handler_name Handler function name, e.g. "listUsers"
	/// \param loc          Source location
	/// \param parent_id    Parent record ID
	uint64_t emitRoute(const std::string &route_label,
			   const std::string &handler_name,
			   SourceRange loc, uint64_t parent_id = 0);

	// ── Type Emitters ───────────────────────────────────────────

	/// Emit a type reference: variable/param/field has a type.
	/// Creates a TypeRef record with variable_name and type_name.
	uint64_t emitTypeRef(const std::string &variable_name,
			     const std::string &type_name, SourceRange loc,
			     uint64_t parent_id = 0);

	/// Emit a type declaration: struct/enum/trait/interface definition.
	/// Creates a TypeDecl record with the type name and location.
	uint64_t emitTypeDecl(const std::string &name, SourceRange loc,
			      uint64_t parent_id = 0);

	// ── Literal / Comment Emitters ─────────────────────────

	uint64_t emitLiteral(const std::string &value, SourceRange loc,
			     uint64_t parent_id = 0);
	uint64_t emitComment(const std::string &text, SourceRange loc,
			     uint64_t parent_id = 0);

	// ── Accessors ────────────────────────────────────────────

	SemanticUnit *unit() const
	{
		return unit_;
	}

    private:
	SemanticUnit *unit_;
};

} // namespace ir

#endif // SEMANTIC_EMITTER_H

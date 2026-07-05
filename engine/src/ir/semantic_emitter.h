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
			      uint64_t parent_id = 0);
	uint64_t emitMethod(const std::string &name, SourceRange loc,
			    uint64_t parent_id = 0);
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

#include "semantic_emitter.h"

namespace ir
{

SemanticEmitter::SemanticEmitter(SemanticUnit *unit)
	: unit_(unit)
{
}

// ── Declaration Emitters ──────────────────────────────────────

uint64_t SemanticEmitter::emitFunction(const std::string &name, SourceRange loc,
				       uint64_t parent_id)
{
	return unit_->addRecord(RecordKind::Function, name, parent_id, loc);
}

uint64_t SemanticEmitter::emitMethod(const std::string &name, SourceRange loc,
				     uint64_t parent_id)
{
	return unit_->addRecord(RecordKind::Method, name, parent_id, loc);
}

uint64_t SemanticEmitter::emitClass(const std::string &name, SourceRange loc,
				    uint64_t parent_id)
{
	return unit_->addRecord(RecordKind::Class, name, parent_id, loc);
}

uint64_t SemanticEmitter::emitInterface(const std::string &name,
					SourceRange loc, uint64_t parent_id)
{
	return unit_->addRecord(RecordKind::Interface, name, parent_id, loc);
}

uint64_t SemanticEmitter::emitEnum(const std::string &name, SourceRange loc,
				   uint64_t parent_id)
{
	return unit_->addRecord(RecordKind::Enum, name, parent_id, loc);
}

uint64_t SemanticEmitter::emitTypeAlias(const std::string &name,
					SourceRange loc, uint64_t parent_id)
{
	return unit_->addRecord(RecordKind::TypeAlias, name, parent_id, loc);
}

uint64_t SemanticEmitter::emitVariable(const std::string &name, SourceRange loc,
				       uint64_t parent_id)
{
	return unit_->addRecord(RecordKind::Variable, name, parent_id, loc);
}

// ── Expression Emitters ───────────────────────────────────────

uint64_t SemanticEmitter::emitCall(const std::string &callee_name,
				   SourceRange loc, uint64_t parent_id)
{
	return unit_->addRecord(RecordKind::CallExpr, callee_name, parent_id,
				loc);
}

uint64_t SemanticEmitter::emitMemberAccess(const std::string &name,
					   SourceRange loc, uint64_t parent_id)
{
	return unit_->addRecord(RecordKind::MemberExpr, name, parent_id, loc);
}

// ── Module Emitters ───────────────────────────────────────────

uint64_t SemanticEmitter::emitImport(const std::string &module_name,
				     SourceRange loc, uint64_t parent_id)
{
	return unit_->addRecord(RecordKind::Import, module_name, parent_id,
				loc);
}

uint64_t SemanticEmitter::emitExport(const std::string &name, SourceRange loc,
				     uint64_t parent_id)
{
	return unit_->addRecord(RecordKind::Export, name, parent_id, loc);
}

// ── Literal / Comment Emitters ───────────────────────────────

uint64_t SemanticEmitter::emitLiteral(const std::string &value, SourceRange loc,
				      uint64_t parent_id)
{
	return unit_->addRecord(RecordKind::Literal, value, parent_id, loc);
}

uint64_t SemanticEmitter::emitComment(const std::string &text, SourceRange loc,
				      uint64_t parent_id)
{
	return unit_->addRecord(RecordKind::Comment, text, parent_id, loc);
}

} // namespace ir

#include "semantic_emitter.h"

namespace ir
{

SemanticEmitter::SemanticEmitter(SemanticUnit *unit)
	: unit_(unit)
{
}

// ── Declaration Emitters ──────────────────────────────────────

uint64_t SemanticEmitter::emitFunction(const std::string &name, SourceRange loc,
				       uint64_t parent_id, int arity,
				       bool is_static)
{
	return unit_->addRecord(RecordKind::Function, name, parent_id, loc,
				arity, is_static);
}

uint64_t SemanticEmitter::emitMethod(const std::string &name, SourceRange loc,
				     uint64_t parent_id, int arity,
				     bool is_static)
{
	return unit_->addRecord(RecordKind::Method, name, parent_id, loc, arity,
				is_static);
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
				   SourceRange loc, uint64_t parent_id,
				   int arity, bool is_static, int call_kind)
{
	uint64_t id = unit_->addRecord(RecordKind::CallExpr, callee_name,
				       parent_id, loc, arity, is_static);
	if (call_kind != 0)
		unit_->setCallKind(id, call_kind);
	return id;
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

// ── Scope Emitters ───────────────────────────────────────────

void SemanticEmitter::emitScope(int kind, int scope_kind,
				const std::string &name, SourceRange loc)
{
	// Scope entries/exits are stored as records with special kinds.
	// kind=0 means scope enter, kind=1 means scope exit.
	// The scope_kind is encoded in the arity field.
	// scope_kind: 0=Global, 1=Module, 2=Function, 3=Block, 4=Trait, 5=Impl
	RecordKind rk = (kind == 0) ? RecordKind::TranslationUnit :
				      RecordKind::Comment;
	unit_->addRecord(rk, name, 0, loc, scope_kind, false);
}

// ── Reference Emitter ────────────────────────────────────────

uint64_t SemanticEmitter::emitReference(const std::string &callee_name,
					SourceRange loc, uint64_t parent_id,
					int arity)
{
	return unit_->addRecord(RecordKind::CallExpr, callee_name, parent_id,
				loc, arity, false);
}

// ── Type Emitters ─────────────────────────────────────────────

uint64_t SemanticEmitter::emitTypeRef(const std::string &variable_name,
				      const std::string &type_name,
				      SourceRange loc, uint64_t parent_id)
{
	return unit_->addTypedRecord(RecordKind::TypeRef, variable_name,
				     type_name, parent_id, loc);
}

uint64_t SemanticEmitter::emitTypeDecl(const std::string &name, SourceRange loc,
				       uint64_t parent_id)
{
	return unit_->addTypedRecord(RecordKind::TypeDecl, name, "", parent_id,
				     loc);
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

// ── Route Emitter ─────────────────────────────────────────────

uint64_t SemanticEmitter::emitRoute(const std::string &route_label,
				    const std::string &handler_name,
				    SourceRange loc, uint64_t parent_id)
{
	return unit_->addRecord(RecordKind::Route, route_label, handler_name, 0,
				parent_id, loc);
}

} // namespace ir

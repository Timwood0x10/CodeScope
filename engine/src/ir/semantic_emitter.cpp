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
				       bool is_static, int visibility)
{
	return unit_->addRecord(RecordKind::Function, name, parent_id, loc,
				arity, is_static, visibility);
}

uint64_t SemanticEmitter::emitMethod(const std::string &name, SourceRange loc,
				     uint64_t parent_id, int arity,
				     bool is_static, int visibility)
{
	return unit_->addRecord(RecordKind::Method, name, parent_id, loc, arity,
				is_static, visibility);
}

uint64_t SemanticEmitter::emitClass(const std::string &name, SourceRange loc,
				    uint64_t parent_id, int visibility)
{
	return unit_->addRecord(RecordKind::Class, name, parent_id, loc, 0,
				false, visibility);
}

uint64_t SemanticEmitter::emitInterface(const std::string &name,
					SourceRange loc, uint64_t parent_id,
					int visibility)
{
	return unit_->addRecord(RecordKind::Interface, name, parent_id, loc, 0,
				false, visibility);
}

uint64_t SemanticEmitter::emitEnum(const std::string &name, SourceRange loc,
				   uint64_t parent_id, int visibility)
{
	return unit_->addRecord(RecordKind::Enum, name, parent_id, loc, 0,
				false, visibility);
}

uint64_t SemanticEmitter::emitTypeAlias(const std::string &name,
					SourceRange loc, uint64_t parent_id,
					int visibility)
{
	return unit_->addRecord(RecordKind::TypeAlias, name, parent_id, loc, 0,
				false, visibility);
}

uint64_t SemanticEmitter::emitVariable(const std::string &name, SourceRange loc,
				       uint64_t parent_id, int visibility)
{
	return unit_->addRecord(RecordKind::Variable, name, parent_id, loc, 0,
				false, visibility);
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

uint64_t SemanticEmitter::emitImportBinding(const std::string &binding,
					    const std::string &module_spec,
					    SourceRange loc, uint64_t parent_id)
{
	// A distinct kind from emitImport, deliberately: `Import` records the whole
	// statement and is what the `import` table and the visitor tests count, so
	// reusing it inflated those counts (the JS/TS visitor tests failed exactly
	// that way, 1 import → 2). The binding sits in `name` and the specifier in
	// `type_name`, which is what lets the Resolver map a bare call name back to
	// its module.
	return unit_->addTypedRecord(RecordKind::ImportBinding, binding,
				     module_spec, parent_id, loc);
}

uint64_t SemanticEmitter::emitExport(const std::string &name, SourceRange loc,
				     uint64_t parent_id)
{
	return unit_->addRecord(RecordKind::Export, name, parent_id, loc);
}

// ── Reference Emitter ────────────────────────────────────────

uint64_t SemanticEmitter::emitReference(const std::string &callee_name,
					SourceRange loc, uint64_t parent_id,
					int arity)
{
	return unit_->addRecord(RecordKind::CallExpr, callee_name, parent_id,
				loc, arity, false);
}

bool SemanticEmitter::setCallFacts(uint64_t call_record_id,
				   const std::string &qualified_target,
				   const std::string &receiver_text,
				   const std::string &receiver_type,
				   const std::string &import_alias)
{
	return unit_->setCallFacts(call_record_id, qualified_target,
				   receiver_text, receiver_type, import_alias);
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

// ── Interface Impl Emitter ───────────────────────────────────

uint64_t SemanticEmitter::emitInterfaceImpl(const std::string &impl_type,
					    const std::string &iface_name,
					    SourceRange loc, uint64_t parent_id)
{
	return unit_->addTypedRecord(RecordKind::InterfaceImpl, impl_type,
				     iface_name, parent_id, loc);
}

} // namespace ir

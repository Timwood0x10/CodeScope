#include "ts_visitor.h"

#include <cstring>
#include <tree_sitter/api.h>

namespace ir
{

TsVisitor::TsVisitor()
{
}

SemanticUnit *TsVisitor::visit(TSTree *tree, const char *source,
			       const char *file_path)
{
	// Delegate to base visit(), then fix language
	SemanticUnit *unit = JsVisitor::visit(tree, source, file_path);
	if (unit) {
		unit->setLanguage("typescript");
	}
	return unit;
}

void TsVisitor::visitNode(TSNode node, uint64_t parent_id)
{
	const char *type = ts_node_type(node);

	// ── TypeScript-specific handlers ───────────────────────────
	if (strcmp(type, "interface_declaration") == 0)
		return visitInterfaceDecl(node, parent_id);
	if (strcmp(type, "type_alias_declaration") == 0)
		return visitTypeAliasDecl(node, parent_id);
	if (strcmp(type, "enum_declaration") == 0)
		return visitEnumDecl(node, parent_id);

	// ── Fall back to JavaScript handling for all shared types ────
	JsVisitor::visitNode(node, parent_id);
}

// ── Class Declaration (TS override: check type_identifier) ────

void TsVisitor::visitClassDecl(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;

	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (strcmp(ts_node_type(child), "identifier") == 0 ||
		    strcmp(ts_node_type(child), "type_identifier") == 0) {
			name = nodeText(child);
			break;
		}
	}

	uint64_t cls_id = emitter_->emitClass(name, loc, parent_id);
	defineSymbol(name, cls_id);

	pushScope();
	visitChildren(node, cls_id);
	popScope();
}

// ── Interface Declaration ────────────────────────────────────

void TsVisitor::visitInterfaceDecl(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;

	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (strcmp(ts_node_type(child), "identifier") == 0 ||
		    strcmp(ts_node_type(child), "type_identifier") == 0) {
			name = nodeText(child);
			break;
		}
	}

	uint64_t iface_id = emitter_->emitInterface(name, loc, parent_id);
	defineSymbol(name, iface_id);

	// Recurse into body members (property_signatures, method_signatures)
	// These are pass-through at the child level — just recurse
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		const char *t = ts_node_type(child);
		if (strcmp(t, "identifier") == 0 ||
		    strcmp(t, "type_identifier") == 0)
			continue;
		visitChildren(child, iface_id);
	}
}

// ── Type Alias Declaration ──────────────────────────────────

void TsVisitor::visitTypeAliasDecl(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;

	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (strcmp(ts_node_type(child), "identifier") == 0 ||
		    strcmp(ts_node_type(child), "type_identifier") == 0) {
			name = nodeText(child);
			break;
		}
	}

	uint64_t alias_id = emitter_->emitTypeAlias(name, loc, parent_id);
	defineSymbol(name, alias_id);

	// Recurse into type value (type_annotation, union_type, etc.)
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		if (strcmp(ts_node_type(child), "identifier") == 0)
			continue;
		visitChildren(child, alias_id);
	}
}

// ── Enum Declaration ─────────────────────────────────────────

void TsVisitor::visitEnumDecl(TSNode node, uint64_t parent_id)
{
	SourceRange loc = location(node);
	std::string name;

	uint32_t count = ts_node_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (strcmp(ts_node_type(child), "identifier") == 0 ||
		    strcmp(ts_node_type(child), "type_identifier") == 0) {
			name = nodeText(child);
			break;
		}
	}

	uint64_t enum_id = emitter_->emitEnum(name, loc, parent_id);
	defineSymbol(name, enum_id);

	// Recurse into enum body (enum_assignments, enum_members)
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_child(node, i);
		if (!ts_node_is_named(child))
			continue;
		if (strcmp(ts_node_type(child), "identifier") == 0)
			continue;
		visitChildren(child, enum_id);
	}
}

} // namespace ir

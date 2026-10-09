#ifndef CPP_VISITOR_H
#define CPP_VISITOR_H
#include "c_visitor.h"
namespace ir
{
class CppVisitor : public CVisitor {
    public:
	CppVisitor();
	SemanticUnit *visit(TSTree *tree, const char *source,
			    const char *file_path) override;

    protected:
	void visitNode(TSNode node, uint64_t parent_id) override;

    private:
	void handleClassSpec(TSNode node, uint64_t parent_id);
	void handleNamespace(TSNode node, uint64_t parent_id);
	void handleTemplate(TSNode node, uint64_t parent_id);
	/// Emit a method DECLARATION/definition-less member from a class-body
	/// `field_declaration` (`void foo();`, `bool operator==(...) const;`)
	/// or from the `declaration` a destructor/constructor declaration
	/// (`~Point();`, `Point();`) parses as. A member that declares no
	/// function (a plain data member such as `int x;`) is only recursed
	/// into, as before.
	void handleMemberFunctionDecl(TSNode node, uint64_t parent_id);

	/// Walk one class body (`field_declaration_list`).
	///
	/// Exists so a bare `declaration` node is only treated as a member
	/// declaration when it really is a DIRECT member of the class: a local
	/// function declaration inside a method body is a declaration too, and
	/// dispatching on the enclosing class name alone turned it into a
	/// phantom `Class::local` entity.
	///
	/// \param list       The class body node.
	/// \param parent_id  Record id of the class.
	void visitClassBody(TSNode list, uint64_t parent_id);

	/// True when `node` declares a function: a direct named child is a
	/// `function_declarator` (most forms) or a `parameter_list` (the
	/// special member forms). False for a plain data member.
	/// \param node  A `field_declaration` or `declaration` node.
	bool declaresFunction(TSNode node);
};
} // namespace ir
#endif

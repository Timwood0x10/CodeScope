#include "ir_complexity.h"

namespace ir
{

ComplexityResult ComplexityAnalyzer::analyze(Node *node)
{
	result_ = ComplexityResult{};
	depth_ = 0;
	max_depth_ = 0;

	if (!node)
		return result_;

	visitNode(node);

	result_.nesting_depth = max_depth_;
	// Cyclomatic = 1 + decision_points
	result_.cyclomatic = 1 + result_.decision_points;
	return result_;
}

void ComplexityAnalyzer::visitNode(Node *node)
{
	if (!node)
		return;

	bool is_decision = false;

	switch (node->kind) {
	// ── Decision points (each adds +1 to cyclomatic) ───────
	case NodeKind::IfStmt:
	case NodeKind::ForStmt:
	case NodeKind::WhileStmt:
	case NodeKind::DoWhileStmt:
	case NodeKind::CatchStmt:
	case NodeKind::TernaryExpr:
		result_.decision_points++;
		is_decision = true;
		break;

	case NodeKind::SwitchStmt:
		// switch itself is +1; each case is also +1
		result_.decision_points++;
		is_decision = true;
		break;

	case NodeKind::CaseStmt:
		result_.decision_points++;
		// cases don't increase nesting depth (they're at the same level as the
		// switch body)
		break;

	case NodeKind::BinaryExpr: {
		// Logical operators && and || add decision points
		// We can't check the operator here without the source text,
		// but we check the number of children — if it has 2+ children
		// and is inside a condition context, it's a logical op
		// Conservative: just count binary exprs in non-expression contexts
		// For v1 we rely on IfStmt/WhileStmt etc. which cover most cases
		break;
	}

	default:
		break;
	}

	// ── Nesting tracking for cognitive complexity ──────────────
	if (is_decision) {
		depth_++;
		if (depth_ > max_depth_)
			max_depth_ = depth_;
	}

	// Recurse into children
	for (auto *child : node->children) {
		visitNode(child);
	}

	if (is_decision) {
		depth_--;
	}
}

} // namespace ir

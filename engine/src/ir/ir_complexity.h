#ifndef IR_COMPLEXITY_H
#define IR_COMPLEXITY_H

#include <cstdint>
#include "ir.h"

namespace ir {

// ─── Complexity Result ─────────────────────────────────────────

struct ComplexityResult {
    uint64_t cyclomatic = 0;      // McCabe cyclomatic complexity
    uint64_t cognitive = 0;       // Cognitive complexity (v2)
    uint64_t nesting_depth = 0;   // Maximum nesting depth
    uint64_t decision_points = 0; // Raw count of decision nodes
};

// ─── Complexity Analyzer ───────────────────────────────────────
//
// Computes cyclomatic complexity for a given IR node (typically a
// FunctionDecl or MethodDecl). Walks the subtree and counts:
//
//   Cyclomatic = 1 + sum(decision_points)
//
// Decision points: if, for, while, do-while, switch-case, catch,
// ternary, logical AND/OR (&&, ||) in binary expressions.

class ComplexityAnalyzer {
  public:
    ComplexityResult analyze(Node *node);

  private:
    ComplexityResult result_;
    uint64_t depth_ = 0; // current nesting depth for cognitive complexity
    uint64_t max_depth_ = 0;

    void visitNode(Node *node);
};

} // namespace ir

#endif // IR_COMPLEXITY_H

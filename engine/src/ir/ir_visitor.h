#ifndef IR_VISITOR_H
#define IR_VISITOR_H

#include "ir.h"

namespace ir {

// Generic depth-first visitor over the IR tree.
// Override methods you care about; the default is to recurse into children.

class Visitor {
public:
  virtual ~Visitor() = default;

  // Called before visiting children. Return false to skip subtree.
  virtual bool visitEnter(Node * /*node*/) { return true; }

  // Called after visiting children.
  virtual void visitLeave(Node * /*node*/) {}

  // Full traversal of a TranslationUnit.
  void traverse(TranslationUnit *unit);
  void traverse(Node *node);

private:
  void traverseNode(Node *node);
};

} // namespace ir

#endif // IR_VISITOR_H

# CodeScope Architecture (7): Language Translators — 10 Languages Unified into One IR

> tree-sitter gives us 10 different AST structures. We need to turn them into one.

## Series Index

| # | Title | One-liner |
|:--:|------|-----------|
| 6 | [MCP Protocol Layer](codescope-architecture-06-mcp-layer.md) | Design of 35+ tools |
| **7** | **Language Translators** (this article) | 10 languages unified into one IR |
| 8 | [SQLite Storage Layer](codescope-architecture-08-storage-layer.md) | WAL, FTS5, vec0, incremental indexing |
| 9 | [Adaptive Queries](codescope-architecture-09-adaptive-queries.md) | When data isn't ready |
| 10 | [Performance Truth](codescope-architecture-10-performance-truth.md) | Measured data across project scales |

## I. Why Unified IR Matters

Each language has its own AST structure:
- Go's AST has `FuncDecl`, `TypeSpec`, `GenDecl`
- Rust's AST has `ItemFn`, `ItemStruct`, `ItemImpl`
- Python's AST has `FunctionDef`, `ClassDef`, `AsyncFunctionDef`

Without a unified IR, cross-language analysis is impossible. With a unified IR, code understanding becomes language-agnostic.

## II. IR Design

### Node Types

```
NodeType enum:
  TranslationUnit  // root node for a file
  Function         // function/method definition
  Struct           // struct/class definition
  Interface        // interface/trait/protocol
  Enum             // enum definition
  Variable         // variable/constant
  Field            // struct field
  Parameter        // function parameter
  Call             // function call expression
  TypeRef          // type reference
  Import           // import/include/use statement
  ...
```

### Node Structure

```cpp
struct SemanticNode {
    uint64_t id;
    NodeKind kind;         // the type enum above
    std::string name;      // symbol name
    std::string signature; // full signature
    std::string doc_comment; // documentation
    uint32_t line_start, line_end;
    uint32_t col_start, col_end;
    std::vector<uint64_t> children; // child nodes
    std::vector<SemanticEdge> edges; // semantic relationships
};
```

## III. Translator Implementation

Each language has a dedicated translator class:

```
Translator base class
├── GoTranslator
├── RustTranslator
├── PythonTranslator
├── CTranslator
├── CppTranslator
├── JavaTranslator
├── JavaScriptTranslator
├── TypeScriptTranslator
├── SwiftTranslator
└── TsxTranslator
```

Each translator implements a `translate(tree, source, file_path)` method that walks the tree-sitter CST and produces `SemanticNode` trees.

### Example: Go Function Translation

```cpp
SemanticNode* GoTranslator::translateFunction(TSNode node) {
    auto* fn = new SemanticNode();
    fn->kind = NodeKind::Function;
    fn->name = nodeText(node, "name");
    fn->signature = extractSignature(node);
    fn->doc_comment = extractComment(node);
    // ... recurse into body
    return fn;
}
```

## IV. Cross-Language Mapping

| Language | Function | Struct | Interface | Enum | Import |
|----------|:--------:|:------:|:---------:|:----:|:------:|
| Go | `func` | `struct` | `interface` | iota | `import` |
| Rust | `fn` | `struct` | `trait` | `enum` | `use` |
| Python | `def` | `class` | `ABC` | `Enum` | `import` |
| C | func decl | `struct` | — | `enum` | `#include` |
| C++ | func/method | `class/struct` | `virtual` | `enum` | `#include` |
| Java | method | `class` | `interface` | `enum` | `import` |
| JS/TS | `function` | `class` | `interface` | — | `import/require` |

## V. Trade-offs

### Visitor Pattern vs. Translator Pattern

The original design used a visitor pattern (walk the tree, emit nodes). The new design uses a translator pattern (walk the tree, return IR nodes). The translator pattern gives each language more control over node structure.

### Depth vs. Breadth

Deep nesting (e.g., Go's anonymous functions inside struct methods) complicates the IR. CodeScope flattens deeply nested structures where possible, at the cost of losing some nesting context.

### Macro Expansion

C/C++ macros are not expanded. This means some symbols defined in macros are invisible to the graph. This is a known limitation.

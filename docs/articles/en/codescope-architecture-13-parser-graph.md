# CodeScope Architecture (13): Parser & GraphBuilder — Parsing to Graph Construction

> tree-sitter was one of the best technology choices in this project. Not because it's fast — because it **never errors**. No matter how malformed the source code, tree-sitter always produces an AST. This is critical when indexing millions of lines: one file parse failure should not block the entire index.

---

## Parser: tree-sitter Wrapper

Located at `engine/src/parser/parser.cpp`, a C++ wrapper around the tree-sitter C API.

### Flow

1. Select language parser by file extension
2. Call tree-sitter to generate a CST
3. Pass CST to the appropriate IR Translator
4. Handle parse errors (primarily memory limits)

```cpp
TranslationUnit *parseFile(const char *path, const char *source,
                           size_t source_len, const char *language) {
    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, getLanguage(language));
    TSTree *tree = ts_parser_parse_string(parser, nullptr, source, source_len);
    Translator *t = createTranslator(language);
    return t->translate(tree, source, path);
}
```

### Why tree-sitter?

| Aspect | tree-sitter | Traditional LALR/LR |
|--------|-------------|-------------------|
| Error tolerance | ✅ Always produces AST (with error nodes) | ❌ Fails on syntax errors |
| Incremental parsing | ✅ Supported | ❌ Full re-parse required |
| Multi-language | ✅ 100+ languages built-in | ❌ Separate toolchain per language |

The downside: tree-sitter's CST includes **all syntactic details** — parentheses, semicolons, keywords. The IR Translator's job is to extract semantic structure and discard syntactic noise.

---

## Grammar Management

```cpp
// engine/src/codescope_grammars.h
// Manages 8 language tree-sitter grammars (.so files)
```

Grammars are auto-downloaded and compiled via CMake FetchContent at build time. Each language maps to a `.so` file loaded at runtime.

| Language | Grammar Source | AST Node Types |
|----------|---------------|:--------------:|
| C | tree-sitter-c | ~50 |
| C++ | tree-sitter-cpp | ~100 |
| Go | tree-sitter-go | ~60 |
| Python | tree-sitter-python | ~40 |
| Rust | tree-sitter-rust | ~80 |
| JavaScript | tree-sitter-javascript | ~70 |
| TypeScript | tree-sitter-typescript | ~80 |
| Java | tree-sitter-java | ~70 |

---

## GraphBuilder: IR → Graph

`GraphBuilder` at `engine/src/graph/graph_builder.h` bridges IR translation and the Resolver.

### Two APIs

```
Old API: TranslationUnit + Node* tree
  └── Used for: C, C++, Go, Python, Rust, Java
  └── Visitor pattern recursively walks IR tree
       Each NodeKind → GraphNode
       Each Relation → GraphEdge

New API: SemanticUnit + flat records
  └── Used for: JavaScript, TypeScript
  └── Accepts flat records directly (no tree traversal)
       Because JS/TS visitor output is already flat
```

### Graph Types

```cpp
enum class NodeType : uint8_t {
    Function, Method, Class, Struct, Interface,
    Variable, Macro, Module, File
};

enum class EdgeType : uint8_t {
    References, Calls, Defines, Contains, Imports, Inherits
};
```

### Parent Chain Cache

Key optimization: caches parent node IDs to avoid repeated SQLite lookups during deep AST traversal. Reduces database queries by **O(depth)** for deeply nested expressions.

### Write Flow

```
GraphBuilder::build(translation_unit)
    │
    ├── Walk IR node tree
    ├── Walk Relation list
    └── Batch write to SQLite
        ├── graph_nodes (individual insert)
        └── graph_edges (batch insert)
```

---

## Performance

| Phase | Small project (200 files) | Linux kernel (64K files) |
|-------|:------------------------:|:-----------------------:|
| tree-sitter parse | ~seconds | ~2 min |
| IR translation | ~ms | ~30s |
| GraphBuilder write | ~ms | ~20s |
| ResolverPipeline | ~ms | ~1 min (bottleneck) |

The **ResolverPipeline is the only super-linear phase** — each reference must search all entities for candidates. All other phases are O(N).

---

## Series Navigation

| # | Article | Topic |
|---|---------|-------|
| (1) | Intro | 56KB vs 629 bytes, what problem CodeScope solves |
| (2) | Progressive Readiness | ms-level project understanding |
| (3) | Worker Isolation | Indexing won't crash MCP Server |
| (4) | Zero-Redundancy Responses | Lean responses, on-demand return |
| (5) | C++ Engine Pipeline | Source code to multi-dimensional code graph |
| (6) | MCP Protocol Layer | Tool design philosophy |
| (7) | Language Translators | 10 languages → unified IR |
| (8) | Storage Layer | SQLite WAL + FTS5 + vec0 |
| (9) | Adaptive Queries | Fallback mechanism & readiness detection |
| (10) | Performance Truth | 200 to 60,000 files measured |
| (11) | Verification Layer | Making AI Accountable |
| (12) | Model Engine | From Facts to Understanding |
| **(13)** | **Parser & GraphBuilder** | **Parsing to Graph Construction ← this article** |

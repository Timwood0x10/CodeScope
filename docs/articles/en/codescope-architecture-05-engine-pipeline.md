# CodeScope Architecture (5): C++ Engine Pipeline — From Source Code to Multi-Dimensional Code Graph

> Once I was debugging a cross-file call chain and found a missing edge in the call graph. A function was clearly being called, but it wasn't in the graph. After an afternoon of debugging, I found the reason: **the call happened inside a macro expansion, and our visitor doesn't expand macros.** This wasn't a bug — it was a trade-off. But it made me realize: from source code to code graph, every step involves trade-offs.

## Three Questions

Any code understanding tool's core task is to turn "text" into "structure." This can be broken into three questions:

1. **How to quickly get code structure without parsing AST?** (key to ms-level result return)
2. **How to unify 10 languages' ASTs into one intermediate representation?** (foundation of cross-language analysis)
3. **How to build a queryable multi-dimensional code graph from unified IR?** (final deliverable)

CodeScope's C++ engine is designed around these three questions.

## Overall Pipeline

```
┌─────────────────────────────────────────────────────────────────┐
│                     C++ Engine Pipeline                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Phase A (Fast Scan)        Phase B/C (Full Index)              │
│  ┌───────────────┐         ┌─────────────────────────┐          │
│  │   Line-by-line │         │  File Read + Language   │          │
│  │   Regex Scan   │         │  Detection              │          │
│  │   (no AST!)    │         │         │               │          │
│  └───────┬───────┘         │         ▼               │          │
│          │                 │  ┌──────────────┐        │          │
│          ▼                 │  │ tree-sitter   │        │          │
│  ┌───────────────┐         │  │ Parse         │        │          │
│  │   Modules     │         │  └──────┬───────┘        │          │
│  │   Symbols     │         │         ▼               │          │
│  │   EntryPoints │         │  ┌──────────────┐        │          │
│  │   Stubs       │         │  │ IR Translator │        │          │
│  └───────────────┘         │  │ (CST → IR)   │        │          │
│          │                 │  └──────┬───────┘        │          │
│          │                 │         ▼               │          │
│          ▼                 │  ┌──────────────┐        │          │
│   ┌────────────┐          │  │ Linker Passes │        │          │
│   │  JSON      │          │  │ ┌────────────┐│        │          │
│   │  Response  │          │  │ │ BuildSymbol││        │          │
│   └────────────┘          │  │ │ IndexPass  ││        │          │
│          │                 │  │ ├────────────┤│        │          │
│          │                 │  │ │ ResolveCall││        │          │
│          │                 │  │ │ Pass       ││        │          │
│          │                 │  │ ├────────────┤│        │          │
│          │                 │  │ │ EmitGraph  ││        │          │
│          │                 │  │ │ Pass       ││        │          │
│          │                 │  │ └────────────┘│        │          │
│          │                 │  └──────┬───────┘        │          │
│          │                 │         ▼               │          │
│          │                 │  ┌──────────────┐        │          │
│          │                 │  │ Complexity   │        │          │
│          │                 │  │ Analyzer     │        │          │
│          │                 │  └──────┬───────┘        │          │
│          │                 │         ▼               │          │
│          │                 │  ┌──────────────┐        │          │
│          │                 │  │ SQLite       │        │          │
│          │                 │  │ Persistence  │        │          │
│          │                 │  └──────────────┘        │          │
│          │                 └─────────────────────────┘          │
└─────────────────────────────────────────────────────────────────┘
```

---

## Phase A: Understanding Code Without Building an AST

Phase A's goal is to **let AI know what's in the project at millisecond level.** Full AST parsing is impossible — tree-sitter parsing 24K lines takes seconds, and IR translation and graph building take even longer.

CodeScope's Phase A uses a different approach: **line-by-line regex scanning.**

### Filter Policy

Phase A starts by reading the project's `.gitignore`, `.hgignore`, and common ignore patterns. This filters out non-source files early.

- Ignored patterns: `node_modules/`, `.git/`, `build/`, `dist/`, `*.pyc`, etc.
- Accepted extensions: `.c`, `.h`, `.cpp`, `.go`, `.rs`, `.py`, `.java`, `.ts`, `.js`, `.swift`, etc.
- Max file size: 1MB by default (configurable)

### Regex Scanner

The scanner matches language-specific patterns line by line:

```
Go:     func | type | struct | interface | const | var
Rust:   fn | struct | enum | trait | impl | mod | pub
Python: def | class | import | from
C/C++:  int|void|char|struct|class|#define|#include
Java:   class | interface | enum | public | private
```

### Output

Phase A outputs structured data written to SQLite `semantic_records` table, including:
- Symbol name
- Kind (function/struct/class/interface/enum/const/var)
- File path and line number
- Visibility (public/private/internal)
- Language

---

## Phase B/C: Full Index Pipeline

### File Read + Language Detection

Reading a file involves:

1. `open()` the file, `fstat()` to get size
2. `read()` entire file content
3. Detect language by file extension
4. Pass to tree-sitter for parsing

### tree-sitter Parse

Each language has a dedicated tree-sitter parser. Parsing produces a Concrete Syntax Tree (CST).

### IR Translator

The CST is language-specific. The IR translator converts it into a language-neutral Intermediate Representation.

### Linker Passes

Three sequential passes over the IR:

1. **BuildSymbolIndexPass**: Indexes all symbols for cross-file resolution
2. **ResolveCallPass**: Resolves function calls to their definitions
3. **EmitGraphPass**: Emits graph nodes and edges to SQLite

### Complexity Analyzer

Computes cyclomatic and cognitive complexity for each function.

### SQLite Persistence

Writes the final graph to SQLite tables.

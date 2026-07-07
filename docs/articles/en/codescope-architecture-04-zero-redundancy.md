# CodeScope Architecture (4): Zero-Redundancy Responses — Minimal Responses, On-Demand Return

> The first time I used codebase-memory-mcp, a `search_graph` response returned 56KB. I asked myself: does AI really need all this?

## Series Index

| # | Title | One-liner |
|:--:|------|-----------|
| 1 | [Introduction](codescope-architecture-01-intro.md) | Why rewrite a code understanding tool |
| 2 | [Progressive Readiness](codescope-architecture-02-progressive-readiness.md) | Millisecond-level code understanding |
| 3 | [Worker Isolation](codescope-architecture-03-worker-isolation.md) | Why indexing won't crash your MCP Server |
| **4** | **Zero-Redundancy Responses** (this article) | Minimal responses, on-demand return |
| 5 | C++ Engine Pipeline | From source code to multi-dimensional code graph |

## I. See the Data First

Same machine, same project (GoAgent, ~24K lines of Go), same question. Two tools, different design philosophies, different response formats:

### CBM v0.8.1 — search_graph Response (56,183 bytes)

```json
[
  {
    "id": "2851",
    "name": "ChaosExecutor",
    "kind": "STRUCT",
    "file": "internal/ares_quant/.../chaos.go",
    "line": 72,
    "column": 1,
    "fingerprint": "abcdef1234567890",
    "ast_profile": {
      "node_count": 427,
      "depth": 12,
      "token_count": 3842,
      "body_tokens": 2891,
      "doc_tokens": 0
    },
    "route": "/api/v1/...",
    "dataType": "code_symbol",
    "body": "type ChaosExecutor struct { ... }",
    "doc": "",
    "children": ["...", "..."],
    "parent_id": "2845",
    "siblings": ["...", "..."],
    "metadata": { ... }
  }
]
```

56KB, 64 results. CBM returns complete information in one shot — fields like fingerprint and ast_profile are valuable for dedup and performance analysis, but in a "quick answer a simple question" scenario, they may not be needed.

### CodeScope — find_symbol Response (629 bytes)

```json
{
  "results": [
    {
      "id": 2851,
      "kind": "struct",
      "name": "ChaosExecutor",
      "signature": "type ChaosExecutor struct {",
      "visibility": "default",
      "language": "go",
      "file_path": "~/go/src/goagent/.../chaos.go",
      "line": 72,
      "column": 1
    }
  ]
}
```

629 bytes, 2 results (exact match). Every field is one AI needs.

| Dimension | CBM | CodeScope |
|-----------|:---:|:---------:|
| Response size | **56,183 bytes** | **629 bytes** |
| Equivalent tokens (ASCII×0.3) | ~16,855 | ~189 |
| Results | 64 | 2 |
| Useful field ratio | ~30% (depends on scenario) | ~100% |
| Unnecessary fields | 7+ | 0 |

Both designs are valid. CBM's response is field-rich, suitable for one-shot deep analysis. CodeScope's is minimal, suitable for token-sensitive and fast interaction scenarios.

## II. Design Philosophy: Every Field Must Answer "Why"

CodeScope's tool response field design follows a simple rule:

> **Every field must answer "what can AI do with this field."**

### Field Existence Analysis

| Field | Exists? | Why Exists / Why Not |
|-------|:-------:|---------------------|
| `id` | ✅ | AI needs id for follow-up queries (e.g., `get_complexity(id)`) |
| `kind` | ✅ | AI needs to know if this is struct / func / var / const |
| `name` | ✅ | Symbol name, basic identifier |
| `signature` | ✅ | AI doesn't need full body, but needs function signature (param types, return types) |
| `visibility` | ✅ | AI needs to know if externally callable |
| `language` | ✅ | AI needs to know which language this symbol belongs to |
| `file_path` | ✅ | AI needs file location for citation |
| `line` | ✅ | AI needs line number for context reference |
| `column` | ✅ | Precise position |
| `fingerprint` | ❌ | **AI doesn't need this.** It's for dedup algorithms, not AI |
| `ast_profile` | ❌ | **AI doesn't need this.** Node count, depth, token count — AI can compute these itself |
| `body_tokens` | ❌ | **AI doesn't need this.** Full body is for human reading; AI needs the signature |
| `doc_tokens` | ❌ | **AI doesn't need this.** FTS search is sufficient |
| `route` | ❌ | **AI doesn't need this.** A REST API artifact |

## III. The Balance: How Much Is Enough?

### 3.1 The Risk of Over-Minimalism

Minimal responses have trade-offs:

**Low error tolerance.** If CodeScope's `find_symbol` returns wrong results, AI has no extra context to detect the error. CBM's 56KB contains redundancy that can help with cross-validation.

**AI uncertainty.** Some AI agents repeatedly call the same tool to confirm — "are you sure this struct has no other fields?" — because the response lacks `children`, and AI doesn't know if it's "no children" or "children not returned."

### 3.2 Token Savings Are Real

| Scenario | CBM | CodeScope | Savings |
|----------|:---:|:---------:|:-------:|
| Find symbol | 19,632 tokens | 552 tokens | **97.2%** |
| Check project overview | Full index 12 min | ms-level + 629 bytes | Time and token savings |
| Check call relationships | 56KB + extra source reading | Call graph direct return | No source reading needed |
| Index DB size | 64 MB | 270 KB | **99.6%** |

### 3.3 Visual Comparison

```
CBM approach:
  ┌────────────────────────────────────────────────┐
  │  56KB JSON (70% noise)  │  Actually analyzing   │
  │  ██████████████████████████████░░░░░░░░░░░░░░░  │
  └────────────────────────────────────────────────┘
    Tokens: ~19,632              Result: answered

CodeScope approach:
  ┌────────────────────────────────────────────────┐
  │  629B JSON (100% useful)  │  Actually analyzing │
  │  ████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  │
  └────────────────────────────────────────────────┘
    Tokens: ~552                Result: answered
```

AI's limited context window means every token matters. But there's no free lunch.

**Minimalism's cost: low error tolerance.** If CodeScope returns wrong results, AI has no buffer to detect the error. CBM's redundancy can help with cross-validation.

**Overly minimal responses may confuse AI.** Some agents repeatedly call to confirm — "are you sure?" — because they can't distinguish "no data" from "data not returned."

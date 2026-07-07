# CodeScope Architecture (2): Progressive Readiness — Millisecond-Level Code Understanding

> The first time I used CBM to index the Linux Kernel, I waited 12 minutes.
> Later I realized this isn't a technical problem — it's a design philosophy problem. CBM's model requires full indexing before querying. CodeScope's model is "get you usable first, then deepen gradually."

## Series Index

| # | Title | One-liner |
|:--:|------|-----------|
| 1 | [Introduction](codescope-architecture-01-intro.md) | Why rewrite a code understanding tool |
| **2** | **Progressive Readiness** (this article) | Millisecond-level code understanding |
| 3 | Worker Isolation | Why indexing won't slow down MCP Server |
| 4 | Zero-Redundancy Responses | Minimal responses, on-demand |
| 5 | C++ Engine Pipeline | From source code to multi-dimensional code graph |

## I. The Problem: 1-12 Minutes of Waiting

MCP code understanding tools share a common problem: **full indexing required before querying.**

You have to wait for the tool to scan all files, parse all ASTs, and build the complete call graph before asking anything. This takes from 1 minute (small project) to 12 minutes (Linux Kernel).

The issues with this wait:

1. **AI doesn't know if it's worth it.** It just wants to check the project's module structure, but has to wait for full indexing.
2. **Most queries don't need full data.** "What entry points does this project have?" — does this need full AST parsing? No. File names and symbol names are enough.
3. **Waste.** You waited 12 minutes for indexing, and AI's first query is "what language is this project written in."

### First-Principles Question

> In a code understanding tool, what's the minimum data needed to answer the top 80% of AI's most common questions?

I analyzed the typical query distribution of AI agents in code understanding scenarios:

| Query Type | Frequency | Minimum Data Needed |
|-----------|:---------:|-------------------|
| Find symbol definition | ~40% | Symbol name + file + line number |
| View project structure/modules | ~20% | Directory structure + file names |
| Find entry points | ~10% | Symbol name + kind (main/init) |
| Query call relationships | ~15% | Graph edges |
| Search code (keywords) | ~10% | FTS index |
| Full analysis | ~5% | Complete graph + complexity |

### Design Implications

- AI can start querying symbols after **milliseconds**
- Level 2 auto-triggers in background, no manual action needed
- Query engine auto-detects readiness, falls back when data isn't ready

---

## II. Five Levels of Readiness

The progressive readiness model has five levels, each adding one capability:

| Level | What's Ready | How | Time (60K files) |
|:-----:|-------------|:---:|:----------------:|
| 1 | File scan + filter | filter_policy reading `.gitignore` | ~5ms |
| 2 | Symbol scan | RapidJSON regex scan | **~ms** |
| 3 | Module tree | Directory hierarchy | **~ms** |
| 4 | Call graph | tree-sitter parsing + IR | Background 1-5 min |
| 5 | FTS + Vector | Full-text search + semantics | Background 5-15 min |

---

## III. Phase A: Millisecond-Level Scan of 60,000 Files

Phase A is the first layer of progressive readiness, and CodeScope's most "counter-intuitive" design.

### 3.1 What Makes It Fast

### 3.2 Filter Policy

### 3.3 Regex Scanner

| Project | Files | Phase A Time |
|---------|:-----:|:------------:|
| ARES Agent | 95 | **<100ms** |
| GoAgent | 1,167 | **<500ms** |
| memscope-rs | 238 | **<200ms** |

### 3.4 What Phase A Can Answer

- Symbol name search (find_symbol)
- Module tree (get_module_tree)
- Entry points (get_entry_points)
- Project overview

### 3.5 What Phase A Cannot Answer

- Call relationships (no call graph built yet)
- Full-text search (no FTS index)
- Call chain tracing
- Community detection

---

## IV. Phase B: Async Enhancement

### 4.1 tree-sitter Full Parse

### 4.2 IR Translation

### 4.3 Call Graph Construction

---

## V. Phase C: On-Demand Full Index

---

## VI. Measured Data: GoAgent Project

Rustc project benchmark:

| Phase | Time | Cumulative |
|:-----:|:----:|:----------:|
| Phase A scan | **<500ms** | <500ms |
| Phase B parse | **~2s** | ~2.5s |
| Phase C graph | **~26s** | ~29.5s |
| FTS post-process | **~2s** | ~31.5s |

Comparing with CBM on the same project:

| Tool | First Query Time | Full Completion |
|------|:---------------:|:---------------:|
| CBM | After full index | **3.94s** |
| **CodeScope** | **<500ms** | **31.5s** |

Of course, CodeScope's full indexing takes longer because it builds more nodes and edges (263,614 nodes vs 24,658 nodes, **10.7x**). But AI doesn't need to wait — it starts working from <500ms.

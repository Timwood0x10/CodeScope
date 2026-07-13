# CodeScope Architecture (12): Model Engine & State Builder — From Facts to Understanding

> After Phase B completes, the database has entity, relation, and graph_edges — raw facts. But facts aren't understanding. You need to compose them into higher-level models: what capabilities does this module have? What's its workflow? Is the architecture layering correct? That's the Model Engine's job.

---

## Overview

Model Engine runs after the Resolver Pipeline, building high-level models from facts and resolution results.

```
Resolver Pipeline ──→ entity + relation + call_edges
                            │
                            ▼
                    ModelEngine::runAll()
                            │
                    ┌───────┼───────┐
                    ▼       ▼       ▼
              Workflow   Capability  Architecture
              Plugin     Plugin      Plugin
```

---

## ModelEngine: Plugin Manager

```cpp
// engine/src/model/engine.h
class ModelEngine {
public:
    void addPlugin(std::unique_ptr<ModelPlugin> plugin);
    int64_t runAll(uint64_t project_id);
    ModelResult run(const std::string &name, uint64_t project_id);
};
```

Each plugin implements the `ModelPlugin` interface and produces output into its own SQLite table.

---

## Built-in Plugins

| Plugin | Output Table | Description |
|--------|-------------|-------------|
| Workflow | workflow | Cross-module execution paths |
| Capability | capability | Module capability declarations & verification |
| Architecture | architecture | Layer classification & inter-layer dependencies |
| Contract | contract | Interface implementation relationships |

---

## StateBuilder: Project State Recovery

```cpp
// engine/src/model/state_builder.h
// Reads from built models and assembles AI-readable module
// summaries and project overviews.
```

Used by `project_overview` and `explain_module` tools.

---

## Relationship with Verification Layer

| | Model Engine | Verification Layer |
|---|---|---|
| Input | entity + relation + graph_edges | entity + relation + README |
| Output | Model tables (workflow/capability/architecture) | Findings (evidence chain) |
| Timing | During index (Phase B) | On-demand query |
| Used by | `project_overview`, `explain_module` | `verify_claim`, `detect_drift` |

Model Engine **builds models**; Verification Layer **uses models to check claims**.

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
| **(12)** | **Model Engine** | **From Facts to Understanding ← this article** |

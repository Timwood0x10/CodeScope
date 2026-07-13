# CodeScope E2E Benchmark Report

**Project**: goagent (Go, 1,134 files)  
**Date**: 2026-07-13  
**Machine**: Apple M3 Max, 64GB RAM, macOS 15.0  
**Engine**: 14 workers × 8MB stack, FilterPolicy Normal

---

## Pipeline Overview

```
1,134 Go files
    │
    ▼
┌──────────────────────────────────────────────────────────┐
│ Phase 1: Index                                           │
│  2,739ms parse → 0ms sqlite → 23,624ms buildGraph       │
│  17,127 nodes, 3,341 edges, 2,949 references             │
│  call_kind: 2,402/2,949 (81%) classified                 │
└──────────────────────────────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────────────────────────────┐
│ Phase 2: Knowledge Graph                                 │
│  159 modules, 176 module_edge rows                       │
│  Architecture: 1,151 items                               │
│  3 workflows, 7 capabilities, 4 contracts                │
└──────────────────────────────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────────────────────────────┐
│ Phase 3: Resolver                                        │
│  552 / 2,949 references resolved (18.7%)                 │
│  2,397 unresolved (mostly fmt.Errorf etc. stdlib calls)  │
└──────────────────────────────────────────────────────────┘
```

## Feature Verification

| Feature | Status | Data |
|---|---|---|
| Type Registry | ✅ | 2,164 type defs + 2,850 refs |
| Route Detection | ✅ | 46 HTTP routes |
| Call Classification | ✅ | 6 languages, 81% classified |
| Interface Method Capture | ✅ | 663 methods with correct parent_id |
| Module Role Labels | ✅ | 159 modules, classified by role |
| Architecture Communities | ✅ | 14 communities, 176 module edges |
| Multi-factor Resolver | ✅ | 8 factors, call_kind weighted |

## Resource Usage

| Metric | Value |
|---|---|
| Total time | ~26s (1,134 files) |
| Parse phase | 2,739ms |
| buildGraph phase | 23,624ms |
| Peak memory | ~150MB |
| Database size | ~176MB |

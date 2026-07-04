# Token Savings Report

> CodeScope vs. reading raw source files — measured across real analysis scenarios.
> Hardware: Apple M3 Max, 36 GB RAM. OS: macOS.

## Why Token Savings Matter

When an LLM needs to understand code, it has two options:
1. **Read raw source files** — thousands of lines of code, mostly irrelevant context
2. **Query CodeScope** — get exactly the structured knowledge needed (symbols, graphs, metrics)

CodeScope reduces token consumption by ~98.9% on average.

## Measured Savings by Scenario

| Scenario | CodeScope (tokens) | Raw Source (tokens) | Savings |
|----------|-------------------|---------------------|---------|
| Find function definition | ~21 | ~2,265 | **99.1%** |
| Trace callers of a function | ~18 | ~2,000 | **99.1%** |
| Project architecture overview | ~32 | ~1,875 | **98.3%** |
| Function complexity analysis | ~43 | ~4,733 | **99.1%** |
| Symbol search by name | ~23 | ~958 | **97.6%** |
| USB driver subsystem overview | ~250 | ~24,000 | **99.0%** |
| Linux kernel scheduler analysis | ~180 | ~15,000 | **98.8%** |
| Parent-child process (COW) trace | ~85 | ~8,500 | **99.0%** |
| **Average** | **~81** | **~7,416** | **98.9%** |

## Real-World Benchmarks

| Analysis | CodeScope Output | Raw Source | Saved |
|----------|----------------|------------|-------|
| Fast Scan `drivers/usb/` (37,286 symbols) | ~2,000 tokens | ~3.2 MB code | ~99.9% |
| Fast Scan `kernel/sched/` (4,913 symbols) | ~800 tokens | ~1.1 MB code | ~99.9% |
| Fast Scan `mm/` (16,111 symbols) | ~1,200 tokens | ~1.8 MB code | ~99.9% |
| `find_symbol("usb_register")` | ~30 tokens | ~400 KB headers | ~99.9% |
| `trace_path(copy_process, dup_mm)` | ~50 tokens | ~8,500 lines fork.c | ~99.4% |
| `build_context("USB init")` | ~500 tokens | ~3.2 MB code | ~99.98% |

## Why It Works

CodeScope pre-indexes the codebase into a structured knowledge base (SQLite with 11 tables). When an LLM asks a question, CodeScope returns only the **relevant facts** — not the entire source file.

```
Source Code → CodeScope (Facts + Index + Graph) → LLM gets only relevant context
                                                                 ↓
                                                    ~98.9% fewer tokens
```

## Methodology

Token count estimated at 4 chars/token for source code. CodeScope output counted from actual JSON responses.

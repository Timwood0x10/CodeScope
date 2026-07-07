# CodeScope Architecture (10): Performance Truth — Measured from 95 to 89,465 Files

> Before I ran proper benchmarks, I told people "our engine is fast."
>
> That was vague confidence — demo projects returned instantly, but I hadn't tested real large projects. Until one day I ran the Linux kernel full index, stared at the terminal for 15 seconds, and thought it had deadlocked. It was just the indexing thread doing a WAL checkpoint. After that, I made a decision: all performance claims must come from at least three projects of different sizes, and must be timed phase-by-phase.

## Series Index

| # | Title | One-liner |
|:--:|------|-----------|
| **10** | **Performance Truth** (this article) | Measured data across project scales |

## Three Questions

1. **From small to large projects, how does indexing time scale? Linear, super-linear, or explosive after a threshold?**
2. **Where's the bottleneck? CPU (parsing), IO (SQLite writes), or something else?**
3. **When the project grows from 100 to 100,000 files, can CodeScope's architecture hold up? Where does it break first?**

## Full Picture: Measured Data from 7 Projects

| Project | Language | Files | Parse Time | Index Time (Total) | Graph Nodes | Node Density |
|---------|----------|:-----:|:----------:|:-----------------:|:-----------:|:------------:|
| ARES Agent | Go | 95 | 9ms | **0.3s** | 24,924 | 262/file |
| CodeScope self | C++/Rust | 174 | — | **1s** | — | — |
| memscope-rs | Rust | 238 | 31ms | **2s** | 123,270 | 518/file |
| CPython | Python | 1,022 | 0.5s | **6s** | 446,618 | 437/file |
| Bun | Zig/C++/JS | 9,641 | 1.0s | **61s** | 2,951,664 | 306/file |
| rustc | Rust | 36,807 | 3.2s | **104s** | 4,130,017 | 112/file |
| JDK | Java/C++ | 19,821 | 2.4s | **211s** | 8,047,762 | 405/file |

From 95 to 36,807 files (388x), indexing time goes from 0.3s to 104s (347x). Roughly linear, with two anomalies.

### Anomaly: JDK vs rustc

JDK has 19,821 files (54% of rustc), but takes 211s (203% of rustc). Why?

The answer is **node density**. JDK produces 405 graph nodes per file, 3.6x rustc's 112 nodes/file. Total nodes 8.05M is 1.95x rustc's 4.13M. JDK is slower not because of file count, but because **Java code has inherently higher symbol density than Rust.**

This finding is important: **CodeScope's indexing time is O(symbols), not O(files).** File count is a proxy metric; the real driver is semantic unit count.

### Scaling Pattern

- **Small (<1K files)**: ~0.3-6s, near-instant
- **Medium (1K-10K files)**: 6-61s, ~1 minute
- **Large (10K-40K files)**: 61-211s, 2-3.5 minutes
- **Extra-large (>60K files)**: Linux kernel full indexing stability and performance still under optimization.

> Note: codebase-memory-mcp performs excellently on Linux kernel full indexing, completing ~65K files in minutes. CodeScope's support for very large projects is still being improved and cannot yet reliably index Linux-scale codebases. This data will be updated after optimization.

---

## Bottleneck Analysis

Breaking indexing into four phases:

```
Parse → SQLite insert → buildGraph → FTS build
```

Phase time distribution (averaged across projects):

```
Parse:    2.6%     ← tree-sitter parsing, CPU-bound
SQLite:  37.4%     ← semantic record writes, IO-bound
buildGraph: 33.2%  ← graph building, SQL+JOIN-bound
FTS:     18.9%     ← full-text index build
Other:    7.9%     ← Vector, complexity, etc.
```

The key finding: **CodeScope is SQLite-bound, not CPU-bound.** Parsing (tree-sitter) takes less than 3% of time. Over 75% of time is spent on SQLite writes and graph building.

### Phase Breakdown by Project

| Project | Parse | SQLite | buildGraph | FTS |
|---------|:----:|:------:|:---------:|:---:|
| ARES (95 files) | 3.0% | 32.0% | 27.0% | 10.0% |
| Bun (9,641 files) | 1.7% | 43.6% | 34.4% | 19.6% |
| rustc (36,807 files) | 3.0% | 37.4% | 29.5% | 22.1% |
| JDK (19,821 files) | 1.2% | 41.7% | 38.1% | 19.1% |

Parse ratio is always low (1-3%). FTS grows with project size (10% → 22%). SQLite is always the largest component.

---

## Single-File Performance

Benchmark: parsing `kernel/latencytop.c` (7,763 bytes):

| Metric | Value |
|--------|:-----:|
| engine_init | 14.6 ms (75% SQLite schema, 15% grammar load) |
| Single file index | **4.9 ms** |
| Throughput | 1,533 KB/s |
| 9 queries total | **0.17 ms** |

---

## Phase A: Fastest Mode

| Project | Files | Phase A Time |
|---------|:-----:|:------------:|
| ARES Agent | 95 | **9ms** |
| GoAgent | 1,167 | **<500ms** |
| memscope-rs | 238 | **<200ms** |

---

## Token Efficiency

Comparing `codescope_trace` (find callers and callees):

**CBM approach**: Bring full call chain source into context (~19,632 tokens), let LLM analyze.
**CodeScope approach**: Return structured caller/callee list (~552 tokens).

```
CBM:      19,632 tokens → LLM analyzes and responds
CodeScope:  552 tokens  → directly usable structured data

Savings: 97.2%
```

### E2E Pipeline Token Comparison

| Step | CBM | CodeScope | Savings |
|------|:---:|:---------:|:-------:|
| Find symbol `Symbol` | 828 tokens | 138 tokens | 83% |
| Get detailed definition | 5,012 tokens | 17 tokens | 99.7% |
| Resolve type reference | 598 tokens | 18 tokens | 97% |
| Find callers | 9,342 tokens | 207 tokens | 97.8% |
| Trace call chain | 3,852 tokens | 172 tokens | 95.5% |
| **Total** | **19,632 tokens** | **552 tokens** | **97.2%** |

---

## Scalability Limits

### Verified Scale

| Size | Project | Index Time |
|:----:|---------|:----------:|
| 100 files | ARES Agent | 0.3s |
| 1,000 files | CPython | 6s |
| 10,000 files | Bun | 61s |
| 20,000 files | JDK | 211s |
| 35,000 files | rustc | 104s |
| >60K files | Linux kernel | ⏳ Under optimization |

### Expected Bottlenecks

1. **SQLite write contention** (first bottleneck): WAL mode allows concurrent reads but writes are serialized. Multiple workers would need serialized writes — currently implemented as single-worker index_project.

2. **buildGraph ROW_NUMBER() OVER()**: This window function is sub-millisecond at million-node scale, but becomes a bottleneck at tens of millions (JDK's r2n phase ~14.5s). Expected to need sharding or incremental buildGraph at hundred-million scale.

3. **Brute-force vector search**: `searchSemantic()` does full table scan on `node_vectors`. Currently microsecond-level at <100K nodes, second-level at million nodes. vec0 extension solves this but requires user installation.

4. **FTS5 index rebuild**: `buildFTSFromGraph()` uses batch INSERT-SELECT, performs well at tens of millions of nodes (rustc 23s). Full rebuild is the slowest phase (JDK 38.3s).

### Practical Limits

Based on measured data:

- **Files: <50,000 files**. Beyond this, buildGraph + FTS combined time exceeds 5 minutes.
- **Nodes: <10M nodes**. Beyond this, SQLite WAL checkpoint time becomes noticeable.
- **DB size: <5 GB**. Beyond this, mmap I/O page fault rate increases.

---

## Honest Reflection

**Benchmark data is more honest than I initially assumed.**

Several things I believed before benchmarking turned out to be wrong:

**"Parsing is the bottleneck"** — Wrong. Tree parsing takes 1-3%. The bottleneck is SQLite.

**"File count determines speed"** — Wrong. Node density is the dominant factor. JDK's 19K files is 2x slower than rustc's 36K files.

**"Querying symbols directly is always fast"** — Mostly true, but Phase A's regex scan took 1.8s on the fs/ directory. Regex scanning is slower than expected on files with heavy macro expansion.

**"New architecture is always faster than old"** — True, but not by the expected margin. The new architecture was proven better not because it's faster, but because it produces higher quality output.

**WAL checkpoint is a hidden pitfall.** When the WAL file grows to hundreds of MB, checkpoint can block writes for ~0.5-2s. This is imperceptible in normal use but shows up in benchmarks.

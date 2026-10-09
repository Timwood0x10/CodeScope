# CodeScope Performance Benchmarks

## Directory Structure

```
benchmarks/
├── README.md             # This file
├── run_benchmark.sh      # Performance benchmark execution script
├── graph_baseline.sh     # Graph-SHAPE regression (see below)
├── baselines/            # Baseline results for each project (JSON)
│   ├── rustc.json        #   performance: index time, latency, memory
│   ├── jdk.json
│   ├── bun.json
│   ├── cpython.json
│   ├── memscope.json
│   ├── ares.json
│   └── graph_*.json      #   graph shape: edges + resolution distribution
└── results/              # Latest benchmark results
    └── latest.json       # Current latest result (symlink)
```

## Measurement Metrics

| Metric | Description | Importance |
|------|------|:------:|
| `index_time` | Total index time | Core |
| `index_speed` | Files per second | Core |
| `query_latency.p50` | Median latency per query | Core |
| `query_latency.p99` | P99 latency per query | Core |
| `memory.rss_peak` | Peak RSS memory | Important |
| `memory.rss_post_gc` | RSS after GC cleanup | Important |
| `node_count` | Total node count | Reference |
| `edge_count` | Total edge count | Reference |

## Running Benchmarks

```bash
# Run benchmarks for all baseline projects
./run_benchmark.sh --all

# Run for a specific project
./run_benchmark.sh --project /path/to/repo --name my_project

# Compare results
./run_benchmark.sh --compare
```

## Baselines

Current baselines are in the `baselines/` directory. `run_benchmark.sh --all` will write results to `results/` named by date.
`--compare` mode outputs a table comparing latest results with baselines.

## Regression Detection

`--check` mode detects:
- Index time regression >20%
- Query latency regression >30%
- Memory growth >15%

Exceeding thresholds will return a non-zero exit code with a regression report.

## Graph-Shape Regression

The benchmarks above measure SPEED. `graph_baseline.sh` measures the RESULT:
how many call edges the resolver produces on a fixed tree, and how they are
distributed across resolution kinds, call-site strategies and deciding factors.
A visitor or resolver-factor change can add, drop or re-point edges on real
projects while `make test-engine` stays green, because the unit tests pin
hand-built fixtures.

```bash
make bench-graph                              # compare against baselines/my checkouts
make bench-graph-update                       # rewrite them (intended changes only)
./benchmarks/graph_baseline.sh --only self    # one project
CODESCOPE_BENCH_PROJECTS=/path/to/checkouts make bench-graph
```

The comparison is exact — no tolerance window — because the shape is
reproducible: two consecutive `index-parallel` runs of the same binary over the
same tree produce identical edge sets (0 added, 0 removed). A baseline that
drifts after a code change is the signal; a baseline that drifts between two
runs of one binary would be a determinism bug in the indexer.

Current baselines: `self` (C++), `c-redis` (C), `go-tinygo` (Go),
`java-spring-petclinic` (Java), `rust-pyo3` (Rust) and
`ts-codebase-memory-mcp` (TypeScript, C).

`self` is the one baseline that tracks the WORKING TREE rather than an external
checkout: it indexes this repository, so adding or removing any indexed source
file under `engine/src` changes its shape by construction and the run reports a
diff (`files`, `entities`, `call_edges` all move) until `--update` rewrites the
file. A diff on `self` alone therefore means "this tree changed", not
"resolution changed" — the other five index fixed checkouts under
`$CODESCOPE_BENCH_PROJECTS` and only move when the resolver moves, which makes
them the ones to read when deciding whether a code change altered resolution.

Java was missing until the discovery defect behind it was fixed:
`discover::discover_modules` asks the engine (`engine_path_is_skipped`) which
directories hold source, and the policy behind that query was built with no
language context, so the test/docs/samples skip names took their any-depth
branch. spring-petclinic keeps its entire source tree under
`src/main/java/org/springframework/samples/petclinic/`, so the scheduler found
**zero** modules, printed `note: "no source modules found"` and indexed nothing
— 0 files, 0 nodes — while `codescope discover <same path>` counted all 49
files. The Java relaxation (top-only, depth ≤ 3) existed, but only the indexer's
own walk knew to set it. Both entry points now share one
`applyProjectLanguageContext()`; petclinic indexes 30 files / 137 nodes / 33
edges with `src/test` still excluded, and the baseline above is its graph.
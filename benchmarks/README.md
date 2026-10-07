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

Current baselines: `self` (C++), `c-redis` (C), `go-tinygo` (Go), `rust-pyo3`
(Rust), `ts-codebase-memory-mcp` (TypeScript, C). Java is absent for a reason
worth knowing: `index-parallel` discovers modules through
`discover::discover_modules`, whose engine-supplied skip rules drop the
directory names `sample`, `samples`, `example`, `examples`, `test`, `tests` and
`docs` at depth 3 and deeper. spring-petclinic keeps its entire source tree
under `src/main/java/org/springframework/samples/petclinic/`, so the scheduler
finds **zero** modules, reports `note: "no source modules found"` and indexes
nothing — while `codescope discover <same path>` counts all 49 files. Reproduce
with any tree shaped `src/main/java/<anyname>/X.java`; `org` is counted, the
seven names above are not.
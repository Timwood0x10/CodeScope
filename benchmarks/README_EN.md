# CodeScope Performance Benchmarks

## Directory Structure

```
benchmarks/
├── README.md         # This file
├── run_benchmark.sh  # Benchmark execution script
├── baselines/        # Baseline results for each project (JSON)
│   ├── rustc.json
│   ├── jdk.json
│   ├── bun.json
│   ├── cpython.json
│   ├── memscope.json
│   └── ares.json
└── results/          # Latest benchmark results
    └── latest.json   # Current latest result (symlink)
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
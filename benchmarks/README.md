# CodeScope 性能基准

## 目录结构

```
benchmarks/
├── README.md         # 本文件
├── run_benchmark.sh  # 基准测试运行脚本
├── baselines/        # 各项目的基准基线（JSON）
│   ├── rustc.json
│   ├── jdk.json
│   ├── bun.json
│   ├── cpython.json
│   ├── memscope.json
│   └── ares.json
└── results/          # 最新运行结果
    └── latest.json   # 当前最新结果（symlink）
```

## 测量指标

| 指标 | 说明 | 重要性 |
|------|------|:------:|
| `index_time` | 完整索引耗时 | 核心 |
| `index_speed` | 文件/秒 | 核心 |
| `query_latency.p50` | 各查询中位数延迟 | 核心 |
| `query_latency.p99` | 各查询 P99 延迟 | 核心 |
| `memory.rss_peak` | 峰值 RSS 内存 | 重要 |
| `memory.rss_post_gc` | 索引后 GC 归还的 RSS | 重要 |
| `node_count` | 总节点数 | 参考 |
| `edge_count` | 总边数 | 参考 |

## 运行基准测试

```bash
# 对所有基线项目运行基准测试
./run_benchmark.sh --all

# 对特定项目运行
./run_benchmark.sh --project /path/to/repo --name my_project

# 比较结果
./run_benchmark.sh --compare
```

## 基线

当前基线在 `baselines/` 目录下。`run_benchmark.sh --all` 会将结果写入 `results/` 并按日期命名。
`--compare` 模式输出表格对比最新结果与基线。

## 回归检测

`--check` 模式会检测：
- 索引时间退化 >20%
- 查询延迟退化 >30%
- 内存增长 >15%

超出阈值会返回非零退出码和退化报告。

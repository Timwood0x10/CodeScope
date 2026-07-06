# CodeScope 下一阶段开发计划

**目标**: 在已经超过 `codebase-memory-mcp 0.8.1` 的基础上，继续扩大性能和能力优势。  
**日期**: 2026-07-06  
**核心结论**: 不再追 parser。当前可靠 benchmark 显示 CodeScope 是 SQL-bound，下一阶段主线是数据库 bulk-load、buildGraph、FTS/vector 分层、模式裁剪和防回退体系。

---

## 1. 当前事实基线

### 1.1 GoAgent 对标

| Metric | CodeScope | codebase-memory-mcp 0.8.1 |
|---|---:|---:|
| Index time | **2.89s** | 3.94s |
| CPU time | **4.22s** | 7.49s |
| Nodes | **261,743** | 24,658 |
| Edges | **244,078** | 124,882 |

结论：CodeScope 已经快 25%，同时节点数约 10x，边数约 2x。

### 1.2 统一计时后的瓶颈

GoAgent，1157 Go files：

| Phase | Time | % |
|---|---:|---:|
| Parse | 103ms | 3.0% |
| SQLite | 1,223ms | 35.2% |
| buildGraph | 1,322ms | 38.1% |
| FTS/vectors | 787ms | 22.7% |
| Overhead | 38ms | 1.1% |
| Total | 3,473ms | 100% |

Linux kernel `kernel/` 子目录，541 C files，501K lines：

| Phase | Time | % |
|---|---:|---:|
| Parse | 232ms | 2.5% |
| SQLite | 2,791ms | 30.5% |
| buildGraph | 3,414ms | 37.3% |
| FTS/vectors | 2,680ms | 29.3% |

结论：所有可靠样本都是 SQL-bound。旧版 “parse 92%” 是 cumulative wall time 计时 bug。

---

## 2. 方向调整

### 2.1 不再作为主线的方向

- Parser 优化。
- tree-sitter allocator/slab 优先优化。
- Arena/string interning 作为 P0/P1 主线。
- C 全量重写。
- 直接写 SQLite B-tree 页面。

这些不是永远不做，而是当前收益/风险比不高。

### 2.2 新主线

1. Benchmark JSON/report + regression gates。
2. Discovery filter policy + FAST mode。
3. buildGraph 分段计时 + SQL plan audit。
4. FTS/vector 从核心索引路径拆出。
5. SQLite bulk-load 后建索引实验。
6. Resolve cache trio。
7. Index supervisor 子进程隔离。
8. Incremental indexing + shared graph artifact。

---

## 3. P0: Benchmark 防回退体系

目标：以后每一刀都有 before/after，不再靠 stdout 表格和主观判断。

### Tasks

- [ ] `test_bench_project` 增加 `--json <path>` 或 `CODESCOPE_BENCH_JSON`。
- [ ] 固定 JSON schema：schema_version、git_rev、machine、project、config、files、lines、nodes、edges、phase_ms、query_ms、RSS、CPU。
- [ ] 增加 `--repeat N`，输出 min/median/p95/max。
- [ ] 增加 `--compare baseline.json current.json`。
- [ ] 修复 source file count / total lines 统计，按 `lang_filter` 统计，不再硬编码 TS/JS。
- [ ] 新增 `benchmarks/baselines/`。
- [ ] 新增 `benchmarks/results/`。
- [ ] 新增 `make bench-check`：跑小项目 + GoAgent 快速检查。
- [ ] 新增 `make bench-full`：跑 engine + GoAgent + kernel 子目录。

### Gates

- Index time 回退 > 8%：失败。
- RSS 回退 > 10%：失败。
- query latency 回退 > 15%：失败。
- nodes/edges 变化 > 5%：标记 review。

### Acceptance

- [ ] 一条命令生成机器可读 benchmark report。
- [ ] 一条命令对比 baseline/current。
- [ ] 性能回退能自动发现。

---

## 4. P1: Discovery Filter Policy + FAST Mode

目标：提前过滤，减少无意义输入；同时为 TTFA 建立 FAST 路线。

### Tasks

- [ ] 新建集中式 `FilterPolicy`。
- [ ] 统一 skip dirs、skip suffixes、skip filenames、language detection。
- [ ] 支持 `.codescopeignore`。
- [ ] 区分 Normal/FAST filter policy。
- [ ] FAST mode 额外跳过 docs、examples、testdata、generated、scripts、tools、migrations、third_party、vendor、assets、media。
- [ ] discovery 输出 stats：seen files、skipped dirs、skipped files、candidate files、indexed files。
- [ ] discovery 阶段记录 file size。
- [ ] parse jobs 按 file size 降序调度。

### Acceptance

- [ ] GoAgent/engine/kernel 都能解释过滤结果。
- [ ] FAST mode TTFA 明显低于 Normal。
- [ ] size-desc 调度不改变节点/边输出。

---

## 5. P1: buildGraph SQL Plan

目标：先看清楚 buildGraph 内部哪段 SQL 慢，再针对性打掉。

### Tasks

- [ ] buildGraph 分段计时：file_list、delete_old_graph、changed_files temp table、_r2n、insert_nodes、insert_contains_edges、insert_call_edges。
- [ ] 增加 `CODESCOPE_SQL_EXPLAIN=1`，输出 `EXPLAIN QUERY PLAN`。
- [ ] 记录每段 SQL 的 rows affected / elapsed ms。
- [ ] 审计 `_r2n` 的 `ROW_NUMBER() OVER` 排序成本。
- [ ] 验证 containment edge join 是否命中 `(file_path, original_id)` 索引。
- [ ] call edge 构建增加 candidate filter：callee kind 只允许 function/method/class constructor 等可调用类型。
- [ ] 区分 local/external/name-only，减少无效全局 name join。

### Acceptance

- [ ] GoAgent buildGraph 从约 1.3s 降到 <900ms。
- [ ] kernel 子目录 buildGraph 从约 3.4s 降到 <2.4s。
- [ ] CALLS 边数量变化可解释，准确性不下降。

---

## 6. P1: FTS / Vector 分层构建

目标：FTS/vector 不阻塞首答。Normal 先可查，Deep 再增强。

### Tasks

- [ ] 拆分 FTS 和 vector 计时，不再混合为 `time_ftsvector_ms`。
- [ ] FTS 只索引高价值节点：function、method、class、interface、module、file。
- [ ] vector 默认只对可排名节点生成，跳过低价值短 name 或重复 name。
- [ ] Normal index 默认只建 graph + FTS。
- [ ] Deep/background 再建 vector、semantic/similarity edges。
- [ ] DB 记录 readiness：fast_ready、normal_ready、deep_ready、fts_ready、vector_ready。
- [ ] API 响应带 result_mode / readiness，避免用户误解答案完整性。

### Acceptance

- [ ] GoAgent FTS/vector 阻塞时间从约 787ms 降到 <450ms，或默认 Normal 可跳过 vector。
- [ ] kernel 子目录 FTS/vector 从约 2.68s 降到 <1.6s。
- [ ] searchCode 保持约 0.1ms 级别。

---

## 7. P1: SQLite Bulk Load Strategy

目标：用数据库 bulk-loader 思路替代“插入时持续维护所有索引”。

### Tasks

- [ ] schema 拆分为 base tables 和 heavy indexes。
- [ ] bulk load 期间只保留必要约束。
- [ ] 写完 semantic_records/graph_nodes/graph_edges 后再创建或重建重型索引。
- [ ] 对比两种策略：插入时维护索引 vs 写完建索引。
- [ ] 记录 DB size、WAL size、index build time、query latency。
- [ ] 明确 benchmark SQLite profile 与 production SQLite profile 的差异。
- [ ] 明确 `synchronous=OFF` 风险边界，生产默认考虑 NORMAL。

### Acceptance

- [ ] GoAgent SQLite write 从约 1.2s 降到 <850ms。
- [ ] kernel 子目录 SQLite write 从约 2.8s 降到 <2.0s。
- [ ] DB 不出现一致性或崩溃恢复问题。

---

## 8. P2: Resolve Cache Trio

目标：抄 CBM 的 resolve 缓存思想，提高 call graph 构建质量和速度。

### Tasks

- [ ] `reach_cache`：per-file import reachability cache。
- [ ] `import_map_cache`：import prefix -> module/symbol scope。
- [ ] `resolve_cache`：同一 caller file 内相同 callee name 只走一次完整策略链。
- [ ] resolve benchmark 输出 cache hit/miss。
- [ ] 先做 Go/Python，再扩展 TS/JS/C/C++。
- [ ] call edge 添加 resolution_kind：local、imported、external、name_only、unresolved。

### Acceptance

- [ ] 大项目 call resolve 不再做重复全局查找。
- [ ] cache hit rate 可观测。
- [ ] CALLS 边准确率提升，数量变化可解释。

---

## 9. P2: Index Supervisor

目标：索引内存和常驻 MCP server 隔离。索引结束后 RSS 100% 归还 OS。

### Tasks

- [ ] 新增 `codescope-index-worker` 或 engine worker entrypoint。
- [ ] MCP server 调起 worker 子进程执行索引。
- [ ] worker 输出 progress JSON 和最终 benchmark JSON。
- [ ] worker 只通过 DB artifact 和 progress pipe 与 server 通信。
- [ ] worker 退出后索引期 RSS 归还 OS。
- [ ] 支持 cancel job。
- [ ] 支持 queued/running/ready/failed 状态。
- [ ] 失败时保留错误日志和 partial DB 策略。

### Acceptance

- [ ] 常驻 MCP server RSS 不随大项目索引持续上涨。
- [ ] kernel 级项目索引结束后 RSS 回落到 server 基线。
- [ ] 用户可以取消索引任务。

---

## 10. P2: Incremental Indexing + Shared Artifact

目标：首次索引拼速度，后续索引拼增量；团队共享避免重复全量构建。

### Incremental Tasks

- [ ] 记录 file mtime_ns、size、content_hash。
- [ ] 下次索引先 stat，未变文件不读、不 parse。
- [ ] 变更文件删除旧 semantic_records/graph_nodes/graph_edges/FTS/vector。
- [ ] 只重建受影响文件和必要 call edges。
- [ ] 输出 incremental stats：changed、unchanged、deleted、added。

### Artifact Tasks

- [ ] 导出 `.codescope/graph.db.zst`。
- [ ] 使用 `VACUUM INTO` 生成紧凑 DB。
- [ ] zstd 压缩。
- [ ] 支持 import artifact 后再跑 incremental。
- [ ] 记录 artifact schema version 和 source git rev。

### Acceptance

- [ ] 改 1 个文件后重新索引 <500ms。
- [ ] 新 clone 可从 artifact 启动，无需全量索引。
- [ ] artifact schema 不兼容时能安全回退到 full index。

---

## 11. P3: Memory Work

目标：只有当 RSS 成为第一瓶颈时才投入。当前先不做 parser allocator 主线。

### Tasks

- [ ] 先通过 supervisor 隔离常驻 RSS。
- [ ] 再做 per-worker scratch arena。
- [ ] visitor 临时 vector/unordered_map reuse。
- [ ] memory budget：超过预算暂停 reader/parser，等待 writer/buildGraph 消化。
- [ ] 只有 allocator profile 证明需要时，再评估 mimalloc/slab。

### Acceptance

- [ ] GoAgent RSS 保持约 350MB 或更低。
- [ ] kernel 子目录 RSS 从约 805MB 继续下降，或通过 supervisor 完全隔离。
- [ ] server 常驻 RSS 稳定。

---

## 12. 保留但降级的小优化

这些可以做，但不作为主线。

- [ ] 移除无必要的 `collect_lock`。如果 `local_idx` 唯一，直接写 `semantic_units[local_idx]`。
- [ ] 线程栈从 256MB 调到 8MB 或 16MB，减少虚拟内存压力。
- [ ] worker-local graph buffer 原型，验证 lock contention 和 RSS。
- [ ] parser pool / parser recycle 策略，只在 profiler 证明有收益时做。

---

## 13. 明确不做

- [ ] 不直接写 SQLite B-tree 页面。
- [ ] 不做 C 全量重写。
- [ ] 不把 arena/slab/mimalloc 放到当前 P0/P1。
- [ ] 不继续围绕 parser 做主线优化。

---

## 14. 推荐执行顺序

1. Benchmark JSON/report + regression gates。
2. FilterPolicy + `.codescopeignore` + discovery stats。
3. Size-desc scheduling。
4. buildGraph split timing + `EXPLAIN QUERY PLAN`。
5. call edge candidate filter。
6. FTS/vector 分层构建。
7. SQLite bulk-load 后建索引实验。
8. Resolve cache trio。
9. FAST / Normal / Deep readiness model。
10. Index supervisor。
11. Incremental indexing + shared artifact。
12. Memory budget + scratch arena。

---

## 15. Success Targets

| Target | Goal |
|---|---:|
| GoAgent Normal | 保持 <3s，nodes/edges 不下降 |
| GoAgent FAST | <1s TTFA |
| Kernel 子目录 Normal | SQL 总时间下降 25% |
| Kernel 子目录 FAST | <4s TTFA |
| caller/callee latency | <1ms |
| server 常驻 RSS | 索引后回到基线 |
| benchmark | 每次优化都有 before/after JSON |


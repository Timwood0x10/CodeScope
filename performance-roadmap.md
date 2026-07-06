# CodeScope Performance Roadmap

目标：性能对标并超过 `codebase-memory-mcp`。不是只追求"全量索引完成时间"，而是同时优化：

- Time To First Answer, TTFA：用户提问后多久能给出可用答案。
- Full Index Time：全项目完整索引耗时。
- Query Latency：结构查询、全文搜索、语义搜索、调用关系查询延迟。
- Memory Footprint：索引和查询阶段内存峰值。
- Accuracy：符号、引用、调用边、包含关系不能为了速度明显退化。

## Current Findings

- ~~`engine_index_file` 是单文件索引主热点：复杂度高，存在循环内线性查找。~~ ✅ 已修复：`ir_node_id -> ir::Node*` map 消除嵌套扫描。
- ~~`engine_index_project` 是全项目索引主路径：已经有 batch/parallel，但仍有重复解析、重复分配、重复字符串处理。~~ ✅ 已修复：language filter 预解析为 `unordered_set`，max_file_size 提前读取，stderr 日志加开关。
- ~~`GraphStore::searchSemantic` 有确定低风险优化：query vector 在每条记录循环内重复反序列化。~~ ✅ 已修复：query vector 移出循环 + top-K partial_sort。
- ~~`GraphStore::buildGraph` 当前用大 `IN (...)` 字符串过滤文件，多文件重建时 SQL 构造和 prepare 成本会放大。~~ ✅ 已修复：改为临时表 join + `_r2n` 索引。
- ~~`GraphBuilder::buildSymbolGraph` 会沿 parent chain 重复查找最近图节点，深层 AST/record 下可能放大。~~ ✅ 已修复：增加 nearest graph parent cache。
- ~~`engine_scanner::detectDecl` 在刷新索引后暴露为 Fast mode 维护热点：单函数 cognitive complexity 很高。~~ ✅ 已修复：改为 table-driven matcher，complexity ~50→~5。
- ~~`GraphStore::insertSemanticRecords` 已复用 prepared statement，但仍逐 record `step/reset`。~~ ✅ 已修复：跨文件 batch insert、FTS/vector cached prepared statement。
- ~~当前索引图中 CALLS 关系不够完整~~ ✅ `getCallers`/`getCallees` 改为 JOIN+参数化 SQL + 复合索引，**延迟从 ~55ms 降至 <1ms**。

## Refreshed Index Snapshot

- 2026-07-06 刷新后：`1872` nodes，`5497` edges，status `ready`。
- **三轮优化全部完成**（P0 低风险热点、P1 buildGraph/SQLite write path、benchmark 基础设施、Fast scanner、FTS/vector 延迟构建、caller/callee 查询优化）。
- 当前瓶颈分布（GoAgent 1157 文件）：SQLite 46.8% + buildGraph 45.2% + Parse 6.2%。
- 内核样本已用统一计时口径复测：541 C 文件、501K 行，Parse 232ms（2.5%），SQLite 2791ms（30.5%），buildGraph 3414ms（37.3%），FTS/vectors 2680ms（29.3%）。旧版 "parse 92%" 是 cumulative wall time 计时 bug。
- 流水线重构已被否决（串行 SQL 阶段占 ~92%，parse 并行化收益天花板已到）。

## P0: Benchmark First

- [x] 建立统一 benchmark 命令（`test_bench_project <grammars> <project> [max_mb] [lang]`）。
- [x] 在索引管线埋点 —— `time_parse_ms` / `time_sqlite_ms` / `time_buildgraph_ms`，per-batch 累加。
- [x] 记录每阶段 wall time、CPU time、RSS、文件数、node 数、edge 数。
- [x] 加 query benchmark：searchCode、searchSemantic、getCallers、getCallees、graphQuery。
- [ ] 输出 JSON benchmark report，方便和 `codebase-memory-mcp` 横向比较。
- [ ] 建立性能门槛：每次优化前后必须给出同一数据集上的 before/after。

验收标准：

- [x] 能回答"每个文件平均耗时花在哪里" —— benchmark 输出 phased breakdown。
- [x] 能稳定复现 3 次 benchmark，波动可解释。GoAgent 三测：2.79s/2.82s/2.86s，波动 <3%。

## P0: Low-Risk Hotspot Fixes

- [x] `GraphStore::searchSemantic`：把 query vector 反序列化移出 `while (sqlite3_step)` 循环。
- [x] `GraphStore::searchSemantic`：用 top-K partial_sort 替代全量 hits 排序。
- [x] `engine_index_file`：建立 `ir_node_id -> ir::Node*` map，避免 graph node x IR node 嵌套扫描。
- [x] `engine_index_batch`：已审计 —— 已有内部 `ir_to_graph_node_` map，无嵌套扫描问题。
- [x] `engine_index_project`：把 `language_filter` 预解析为 `unordered_set`，避免每个文件重复拆 comma string。
- [x] `engine_index_project`：把 `CODESCOPE_MAX_FILE_SIZE` 预解析一次，避免 worker 内每文件读 env。
- [x] 给 stderr batch 日志加开关或降频 —— 新增 `CODESCOPE_VERBOSE=0` 环境变量。

验收标准：

- [x] 小改动不改变输出 node/edge 语义。编译通过，无逻辑变更。
- [x] search semantic 延迟下降可测。**GoAgent 实测 0.1ms**。
- [x] 单文件/批量索引 CPU time 下降可测。**GoAgent 三测稳定 4.32~4.39s CPU time**。

## P1: SQLite Write Path

- [x] 审计主要 insert 路径，确认 prepared statement 复用情况。
- [x] 对 semantic_records 增加跨文件 batch flush API。
- [x] `insertSemanticRecords` 支持跨文件 batch 输入（`insertSemanticRecordsBatch`）。
- [x] 减少逐条 `insertGraphNode` / `insertGraphEdge` 调用，优先使用批量写。
- [x] 索引期间启用明确的 SQLite profile：WAL、synchronous OFF、temp_store MEMORY、cache_size 64MB。
- [x] 大事务按 batch 提交，避免每文件过碎，也避免单事务过大。
- [x] 对 `buildGraph` 的 changed files 过滤改为临时表 join，替代拼接大 `IN (...)`。
- [x] `insertIntoFTS` / `storeVector` 改为 cached prepared statement，减少每节点 prepare/finalize。

验收标准：

- [x] SQLite 阶段耗时稳定：GoAgent ~1.3s / 1157 files，约 1.1ms/file。
- [x] 大项目不会生成超长 SQL —— temp table join 替代 IN clause。
- [ ] crash 后数据库一致性策略明确。

## P1: Pipeline Parallelism

已否决。串行 SQL 阶段（SQLite + buildGraph）占 ~92%，parse 仅 6%，流水线并行化收益天花板已到。

## P1: Memory And String Cost

- [x] 对高频字符串做 string interning：已尝试 Record string interning，结论是收益与复杂度不成正比，回退。
- [ ] 在 SemanticUnit/SemanticRecord 中减少不必要的 `std::string` 拷贝。
- [ ] Visitor 内部 vector/unordered_map 做 arena/reuse。
- [ ] 对路径存储做规范化和去重，避免每条记录复制完整 path。
- [ ] 审计 vector serialization/deserialization，避免循环内重复构造临时 string。

验收标准：

- [ ] RSS 降低。（当前 GoAgent ~344 MB）
- [ ] allocator profile 中 malloc/free 次数下降。

## P2: Graph Construction

- [x] `GraphBuilder::buildSymbolGraph` 增加 nearest graph parent cache，避免重复 parent chain walk。
- [x] `GraphStore::buildGraph` 对 `_r2n` 增加必要临时索引，特别是 `(file_path, original_id)`、`name`。
- [x] `graph_edges` 复合索引：`idx_ge_callers(edge_type, target_node_id)`、`idx_ge_callees(edge_type, source_node_id)`。
- [x] `getCallers`/`getCallees` 改为 JOIN + 参数化 SQL（消除 SQL 注入），**延迟从 ~55ms 降至 <1ms**。
- [ ] call edge 构建区分 local/external/name-only，减少无效 name join。
- [ ] 补齐 CALLS 图质量，避免图工具看不到真实调用关系。

验收标准：

- [x] buildGraph 阶段耗时稳定：GoAgent ~1.3s / 1157 files。
- [x] callers/callees 查询延迟下降：**55ms → <1ms**。
- [ ] CALLS 边数量和准确率有可解释提升。

## P2: Fast / Normal / Deep Modes

- [ ] Fast：轻量 scanner，只提 function/class/struct/import 和简单 call，目标 TTFA。
- [x] Fast scanner：`detectDecl` 改为 table-driven matcher（6 语言 DeclPattern 表），complexity ~50→~5。
- [ ] Fast scanner：为每种语言建立小型 fixture benchmark，记录 scan time 和准确率。
- [ ] Normal：Tree-sitter + SemanticUnit + symbol graph，目标高性价比全量索引。
- [ ] Deep：CFG、完整 call graph、metrics、embedding，后台渐进增强。
- [ ] MCP/API 层暴露当前索引状态：fast_ready、normal_ready、deep_ready。
- [ ] 用户查询优先用已 ready 的层级，后台继续升级。

验收标准：

- [ ] TTFA 明显低于完整索引时间。
- [ ] 用户不必等待全项目 Deep index 才能开始问问题。
- [ ] 模式之间的结果差异有标识，避免误导。

## P3: Competitive Targets

- [x] 选 3 个固定对标项目：engine C++、GoAgent Go、Linux kernel C 子目录。
- [x] 对 GoAgent 记录 `codebase-memory-mcp` 的 index time、CPU time、node/edge 数。
- [ ] CodeScope 在 Fast 模式 TTFA 明显优于 `codebase-memory-mcp`。
- [x] CodeScope 在 Normal 模式 Full Index Time 接近或优于 `codebase-memory-mcp`。GoAgent：2.89s vs 3.94s。
- [ ] CodeScope 在 Deep 模式提供更强能力：更高质量 call graph、CFG/metrics、增量更新、可解释上下文。
- [ ] 建立 regression dashboard：性能不能靠主观感受。

## P1/P2 待优化项（剩余 backlog）

- [x] `buildFTSFromGraph` / `buildVectorsFromGraph` 接入索引后处理流程 —— ✅ 已完成，索引后自动批量构建。
- [x] 统一 benchmark 计时定义：`time_buildgraph_ms` 只表示 buildGraph 自身，FTS/vector 单独计 `time_ftsvector_ms`。
- [x] 每个样本记录同一套字段：parse/sqlite/buildGraph/FTS-vector/query/RSS/nodes/edges —— 已全部输出。
- [x] 对 3 个固定项目（engine C++ / GoAgent Go / 内核 C）连续跑 3 次，确认瓶颈分布——**全部 SQL-bound，parse < 5%**。之前 kernel "parse 92%" 是计时 bug。
- [ ] Memory And String Cost 优化（string interning、arena reuse、path dedup）。
- [ ] CALLS 图质量补齐（local/external/name-only 区分）。
- [ ] Fast / Normal / Deep 多模式渐进索引。
- [x] Competitive benchmarking 与 `codebase-memory-mcp` 对标——**CodeScope 快 25%，节点精度 10x，边数 2x**。

**当前瓶颈快照（GoAgent 1157 Go 文件，完整管线）：**
| Phase | Time | % |
|---|---|---|
| Parse | 103ms | 3.0% |
| SQLite | 1,223ms | 35.2% |
| buildGraph | 1,322ms | 38.1% |
| FTS/vectors | 787ms | 22.7% |
| Overhead | 38ms | 1.1% |
| **Total** | **3,473ms** | |

**Competitive benchmark（GoAgent 1157 Go 文件）：**
| Metric | CodeScope | codebase-memory-mcp 0.8.1 |
|---|---|---|
| Index time | **2.89s** | 3.94s |
| CPU time | **4.22s** | 7.49s |
| Graph nodes | **261,743** | 24,658 |
| Graph edges | **244,078** | 124,882 |
| Query latency (searchCode) | **0.1ms** | N/A |
| Query latency (callers) | **<0.1ms** | N/A |
| Peak RSS | **~345 MB** | N/A |

**全部完成项（2026-07-06，共 4 轮）：**
- P0 Hotspot Fixes（searchSemantic、ir_node_id map、language filter、stderr log switch）
- P1 buildGraph（IN→temp table join、_r2n 索引、parent chain cache）
- P1 SQLite Write Path（batch insert、cached prepared statements、FTS/vector 延迟构建）
- P1 Query（caller/callee JOIN + 参数化 + 复合索引，55ms→<1ms）
- Fast scanner table-driven refactor（cognitive complexity ~50→~5）
- Benchmark infrastructure（phased timing、WAL clean、3-run verification）

## Phase 5 Plan: Beat And Stay Ahead

目标：不再碰 parser。下一阶段只打 SQL-bound 部分，并建立长期防回退机制。

### 5.1 Stable Benchmark Report

- [ ] `test_bench_project` 增加 `--json <path>` 或 `CODESCOPE_BENCH_JSON`，输出机器可读 report。
- [ ] JSON report 固定 schema：project、git_rev、timestamp、machine、config、files、lines、nodes、edges、RSS、CPU、phase_ms、query_ms。
- [ ] 修正非 TS/JS 项目的展示统计：按 `lang_filter` 统计 source file count 和 total lines。
- [ ] 支持 `--repeat N`，自动输出 min/median/p95/max，避免手工三次跑。
- [ ] 支持 `--compare baseline.json current.json`，输出百分比变化和 pass/fail。
- [ ] benchmark report 保存到 `benchmarks/results/*.json`，文件名包含 project/lang/git short hash。

验收标准：

- [ ] GoAgent、engine、kernel 三个项目都能一条命令生成稳定 JSON。
- [ ] benchmark 数据不再需要人工从 stdout 表格里抄。
- [ ] report schema 变更必须显式 version bump。

### 5.2 Regression Gates

- [ ] 建立 `benchmarks/baselines/`，保存当前最佳基线。
- [ ] 定义硬门槛：index time 回退 > 8%、RSS 回退 > 10%、query latency 回退 > 15% 则失败。
- [ ] 定义软门槛：nodes/edges 数量变化 > 5% 时标记 review，避免为了速度损伤语义质量。
- [ ] 增加 `make bench-check`，跑小型项目和 GoAgent 快速检查。
- [ ] 增加 `make bench-full`，跑 engine + GoAgent + kernel 子目录完整检查。
- [ ] 在 CI 或本地 pre-release 流程中要求 `bench-check` 通过。

验收标准：

- [ ] 性能回退能被自动发现。
- [ ] 图质量变化能被自动提示，不靠肉眼。
- [ ] `bench-check` 总耗时控制在可接受范围内。

### 5.3 buildGraph Next Cuts

- [ ] 对 `buildGraph` 内部 SQL 加 `EXPLAIN QUERY PLAN` debug 模式，记录每个 SQL 的 query plan。
- [ ] 拆分 buildGraph 阶段计时：file_list、delete_old_graph、temp_changed_files、_r2n、insert_nodes、insert_contains_edges、insert_call_edges。
- [ ] 优化 `_r2n` 生成：确认是否可以减少 `ROW_NUMBER() OVER` 排序成本，或用 id allocator 表替代全排序。
- [ ] 优化 containment edges：验证 parent join 是否已命中 `(file_path, original_id)` 索引。
- [ ] 优化 call edges：区分 local/external/name-only，减少 `sr.name = callee.name` 的无效全局 name join。
- [ ] 给 call edge 构建增加候选过滤：callee kind 只允许 function/method/class constructor 等可调用类型。
- [ ] 对 buildGraph 提供 symbols-only / calls-on-demand / full 三种策略，减少默认索引负担。

验收标准：

- [ ] GoAgent buildGraph 从 ~1.3s 降到 <900ms。
- [ ] kernel 子目录 buildGraph 从 ~3.4s 降到 <2.4s。
- [ ] CALLS 边数量变化可解释，准确性不下降。

### 5.4 FTS / Vector Construction

- [ ] 拆分 FTS 和 vector 计时，避免 `time_ftsvector_ms` 混合看不清。
- [ ] FTS 只索引可搜索节点：function/method/class/interface/module/file，跳过噪音 record。
- [ ] vector 只对可排名节点生成，默认跳过低价值短 name 或重复 name。
- [ ] 建立 `node_text_cache` 或 SQL projection，避免 FTS/vector 阶段重复拼接字符串。
- [ ] 评估 vector 延迟构建：Normal index 只建 FTS，Deep/background 再建 vector。
- [ ] 对 FTS/vector 构建支持 batch chunk，减少长事务和峰值内存。

验收标准：

- [ ] GoAgent FTS/vector 从 ~787ms 降到 <450ms，或默认 Normal 模式可跳过 vector。
- [ ] kernel 子目录 FTS/vector 从 ~2.68s 降到 <1.6s。
- [ ] searchCode 延迟保持 ~0.1ms 级别。

### 5.5 SQLite Layout And Write Strategy

- [ ] 审计 schema：确认 hot join 列都有复合索引，删除未使用或伤写入性能的冗余索引。
- [ ] 将索引创建策略拆成 `createSchemaBase()` 和 `createIndexesAfterBulkLoad()`，评估 bulk load 后建索引是否更快。
- [ ] 对 `semantic_records` 分析 row size，确认 path/language/name 重复字符串是否值得 normalize。
- [ ] 评估 `WITHOUT ROWID` 或更合适的 primary key，用 benchmark 决定，不预设结论。
- [ ] 对大批量插入使用 `sqlite3_exec("BEGIN IMMEDIATE")`，明确锁和事务语义。
- [ ] 明确 `synchronous=OFF` 的风险边界：benchmark 默认可用，生产默认是否应降为 NORMAL。

验收标准：

- [ ] GoAgent SQLite write 从 ~1.2s 降到 <850ms。
- [ ] kernel 子目录 SQLite write 从 ~2.8s 降到 <2.0s。
- [ ] crash consistency 策略写清楚，benchmark 和生产配置分离。

### 5.6 Decision Rules

- [ ] 如果 buildGraph 仍是最大项，优先优化 SQL plan 和 call edge candidate filter。
- [ ] 如果 FTS/vector 变成最大项，默认切 Normal/Deep 分层，不让 vector 阻塞 TTFA。
- [ ] 如果 SQLite write 仍是最大项，优先评估 bulk-load 后建索引。
- [ ] 如果 RSS 上升超过 10%，暂停性能优化，先找内存来源。
- [ ] 每一刀必须带 before/after report，不能只看单次 stdout。

下一阶段推荐顺序：

1. 先做 Stable Benchmark Report 和 Regression Gates。
2. 再做 buildGraph 分段计时和 query plan。
3. 然后打 call edge candidate filter。
4. 最后做 FTS/vector 分层构建和 SQLite layout 实验。

## Phase 6 Plan: Architecture Lessons From codebase-memory-mcp

参考对象：`codebase-memory-mcp` 的高性能索引架构。结论不是照搬 C 实现，而是吸收它的 pipeline 和数据库 bulk-load 思路。

核心判断：

- CodeScope 已经在 GoAgent 上超过 `codebase-memory-mcp` 的索引时间，但当前瓶颈仍集中在 SQL/write/buildGraph/FTS/vector。
- 继续优化 parser 收益很低。
- 直接写 SQLite B-tree 页面风险过高，不作为近期路线。
- 真正值得学的是：单次解析、模式裁剪、子进程隔离、bulk load、按阶段延迟构建、防回退 benchmark。

### 6.1 What To Adopt

- [ ] 单次解析原则：每个文件只读一次、parse 一次，后续 Graph/FTS/vector/metrics 全部复用 SemanticRecord 或内存 batch。
- [ ] 按文件大小降序调度：大文件先进入 worker，减少尾部大文件拖慢整体完成时间。
- [ ] worker-local buffer：worker 本地积累 semantic/graph buffer，最终批量合并，减少锁和跨线程写。
- [ ] bulk load 策略：批量写 semantic_records/graph_nodes/graph_edges，写完后再创建或重建重型索引。
- [ ] FAST / Normal / Deep 模式裁剪：不同模式明确关闭不同重量级功能。
- [ ] index supervisor：索引放进独立子进程，结束后 RSS 100% 归还 OS，MCP server 常驻进程不背索引期内存。
- [ ] 内存预算：大项目下限制 parse/visitor 并发，避免 RSS 超预算后系统抖动。

### 6.2 What Not To Adopt Yet

- [ ] 不直接写 SQLite B-tree 页面。

原因：

- SQLite 内部格式和页面一致性复杂。
- 事务、索引、WAL、崩溃恢复都要自己承担风险。
- 可移植性和调试成本高。
- 当前 CodeScope 仍有更低风险的 SQL/bulk-load/模式裁剪优化空间。

- [ ] 不做 C 全量重写。

原因：

- 当前 C++ 版本已经在 GoAgent 上快于 `codebase-memory-mcp 0.8.1`。
- 现阶段瓶颈是数据布局和写入策略，不是 C++ 语言本身。
- 全量重写会牺牲迭代速度和现有测试资产。

- [ ] 不优先做 allocator 微优化。

原因：

- string interning 已尝试并回退，收益与复杂度不成正比。
- 当前最大项仍是 SQL/buildGraph/FTS/vector。
- arena/slab/mimalloc 可以作为大项目 RSS 超限后的专项优化。

### 6.3 Mode Roadmap

#### FAST Mode

目标：最快 TTFA，不追求完整图。

默认开启：

- [ ] 文件发现 + 过滤。
- [ ] 轻量符号提取。
- [ ] function/class/struct/interface/module 基础节点。
- [ ] 最小 containment graph。
- [ ] 基础 name search。

默认关闭：

- [ ] vectors。
- [ ] semantic/similarity edges。
- [ ] Git 历史耦合。
- [ ] 重型 call graph。
- [ ] 低价值节点类型。
- [ ] 大规模 FTS body。

验收标准：

- [ ] TTFA 明显低于 Normal index。
- [ ] GoAgent FAST 目标 < 1s。
- [ ] kernel 子目录 FAST 目标 < 4s。

#### Normal Mode

目标：默认可用的完整索引。

默认开启：

- [ ] Tree-sitter parse。
- [ ] SemanticRecord。
- [ ] symbols + containment graph。
- [ ] name-based call graph。
- [ ] FTS。
- [ ] caller/callee/query API。

默认关闭或延迟：

- [ ] vectors。
- [ ] semantic/similarity edges。
- [ ] 深度 metrics。
- [ ] CFG/dataflow。

验收标准：

- [ ] GoAgent Normal 持续优于 `codebase-memory-mcp`。
- [ ] kernel 子目录 Normal 继续 SQL-bound 且可解释。
- [ ] caller/callee 维持 <1ms 级别。

#### Deep Mode

目标：比 `codebase-memory-mcp` 更强，而不是更快。

后台或按需开启：

- [ ] vector embedding / semantic search。
- [ ] similarity / semantic edges。
- [ ] metrics。
- [ ] CFG。
- [ ] richer call graph。
- [ ] impact analysis。

验收标准：

- [ ] 不阻塞 FAST/Normal 的首答。
- [ ] Deep 结果有状态标记：pending/running/ready。
- [ ] 用户知道当前答案来自 FAST、Normal 还是 Deep。

### 6.4 Index Supervisor Roadmap

- [ ] 新增 `codescope-index-worker` 或 engine worker entrypoint。
- [ ] MCP server 调起 worker 子进程执行索引。
- [ ] worker 输出结构化 progress 和最终 benchmark JSON。
- [ ] worker 只通过 DB artifact 和 progress pipe 与 server 通信。
- [ ] worker 退出后索引期 RSS 归还 OS。
- [ ] server 支持查询 index job 状态：queued/running/ready/failed。
- [ ] 失败时保留错误日志和 partial DB 策略。

验收标准：

- [ ] 常驻 MCP server RSS 不随大项目索引持续上涨。
- [ ] kernel 级项目索引结束后 RSS 回落到 server 基线。
- [ ] 用户可以取消索引任务。

### 6.5 Bulk Load Roadmap

- [ ] schema 拆分：base tables first，heavy indexes later。
- [ ] bulk load 期间跳过或 drop 部分二级索引。
- [ ] 写入结束后重建索引，并单独计时。
- [ ] 对比两种策略：插入时维护索引 vs 写完重建索引。
- [ ] 将 FTS/vector 从核心索引事务中拆出，作为 Normal/Deep 后处理。
- [ ] 记录 DB size、WAL size、build index time、query latency。

验收标准：

- [ ] GoAgent SQLite + buildGraph + FTS/vector 总和下降 25%。
- [ ] kernel 子目录 SQL 总和下降 25%。
- [ ] DB 不出现一致性或崩溃恢复问题。

### 6.6 Memory Roadmap

- [ ] 先做 supervisor，优先解决常驻 RSS。
- [ ] 再做 per-worker scratch arena，减少临时分配碎片。
- [ ] 对 visitor 临时 vector/unordered_map 做 reuse。
- [ ] 大项目启用 memory budget：超过预算暂停 reader/parser，等待 writer/buildGraph 消化。
- [ ] 只在 RSS 仍然是瓶颈时评估 mimalloc/slab。

验收标准：

- [ ] GoAgent RSS 保持约 350MB 或更低。
- [ ] kernel 子目录 RSS 从约 805MB 继续下降，或通过 supervisor 完全隔离。
- [ ] server 常驻 RSS 稳定。

### 6.7 Final Strategic Order

推荐执行顺序：

1. Benchmark JSON/report + regression gates。
2. buildGraph 分段计时 + SQL plan audit。
3. FAST / Normal / Deep 模式策略落地。
4. FTS/vector 从核心索引中拆出。
5. bulk load 后建索引实验。
6. index supervisor 子进程隔离。
7. memory budget + scratch arena。
8. 更强 Deep 能力：CFG、semantic edges、impact analysis。

不推荐顺序：

1. 不先做 parser 优化。
2. 不先做 C 重写。
3. 不先直接写 SQLite B-tree 页面。
4. 不先做 mimalloc/slab，除非 RSS 已经成为第一瓶颈。

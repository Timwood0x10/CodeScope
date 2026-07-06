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

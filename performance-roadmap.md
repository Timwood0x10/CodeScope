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
- `GraphStore::buildGraph` 当前用大 `IN (...)` 字符串过滤文件，多文件重建时 SQL 构造和 prepare 成本会放大。
- `GraphBuilder::buildSymbolGraph` 会沿 parent chain 重复查找最近图节点，深层 AST/record 下可能放大。
- `engine_scanner::detectDecl` 在刷新索引后暴露为 Fast mode 维护热点：单函数 cognitive complexity 很高，需要表驱动或按语言拆分。
- `GraphStore::insertSemanticRecords` 已复用 prepared statement，但仍逐 record `step/reset`，是 SQLite write path 优化的直接入口。
- 当前索引图中 CALLS 关系不够完整，调用扇入/扇出不能作为唯一性能判断依据。

## Refreshed Index Snapshot

- 2026-07-06 刷新后：`1872` nodes，`5497` edges，status `ready`。
- Edge counts: `CALLS=1004`，`DEFINES=1694`，`USAGE=883`。
- P0 Low-Risk Hotspot Fixes 已合入（2026-07-06）。下一步：修复分阶段计时精度，运行 benchmark 记录 baseline。

## P0: Benchmark First

- [ ] 建立统一 benchmark 命令，固定输入项目、语言过滤、文件大小上限、数据库路径。
- [x] 在索引管线埋点 —— 初步埋点已存在（time_parse_ms / time_sqlite_ms / time_buildgraph_ms），需修复精度。
- [ ] 记录每阶段 wall time、CPU time、RSS、文件数、record 数、node 数、edge 数。
- [ ] 加 query benchmark：search_graph 等价查询、trace callers/callees、FTS search、semantic search、graph query。
- [ ] 输出 JSON benchmark report，方便和 `codebase-memory-mcp` 横向比较。
- [ ] 建立性能门槛：每次优化前后必须给出同一数据集上的 before/after。

验收标准：

- [ ] 能回答"每个文件平均耗时花在哪里"，不靠猜。
- [ ] 能稳定复现 3 次 benchmark，波动可解释。

## P0: Low-Risk Hotspot Fixes

- [x] `GraphStore::searchSemantic`：把 query vector 反序列化移出 `while (sqlite3_step)` 循环。
- [x] `GraphStore::searchSemantic`：用 top-K partial_sort 替代全量 hits 排序。
- [x] `engine_index_file`：建立 `ir_node_id -> ir::Node*` map，避免 graph node x IR node 嵌套扫描。
- [x] `engine_index_batch`：已审计 —— batch 路径经 linker -> GraphBuilder，已有内部 `ir_to_graph_node_` map，无嵌套扫描问题，无需改动。
- [x] `engine_index_project`：把 `language_filter` 预解析为 `unordered_set`，避免每个文件重复拆 comma string。
- [x] `engine_index_project`：把 `CODESCOPE_MAX_FILE_SIZE` 预解析一次，避免 worker 内每文件读 env。
- [x] 给 stderr batch 日志加开关或降频 —— 新增 `CODESCOPE_VERBOSE=0` 环境变量。

验收标准：

- [x] 小改动不改变输出 node/edge 语义。编译通过，无逻辑变更。
- [ ] search semantic 延迟下降可测。（等待 benchmark baseline）
- [ ] 单文件/批量索引 CPU time 下降可测。（等待 benchmark baseline）

## P1: SQLite Write Path

- [ ] 审计所有 insert 路径，确认是否复用 prepared statement。
- [ ] 对 semantic_records、graph_nodes、graph_edges、FTS、vectors 增加批量 flush API。
- [ ] `insertSemanticRecords` 支持跨文件 batch 输入，减少每文件 prepare/finalize 和逐 record 调用开销。
- [ ] 减少逐条 `insertGraphNode` / `insertGraphEdge` 调用，优先使用批量写。
- [ ] 索引期间启用明确的 SQLite profile：WAL、synchronous NORMAL/OFF、temp_store MEMORY、合理 cache_size。
- [ ] 大事务按 batch 提交，避免每文件过碎，也避免单事务过大。
- [ ] 对 `buildGraph` 的 changed files 过滤改为临时表 join，替代拼接大 `IN (...)`。

验收标准：

- [ ] SQLite 阶段耗时显著下降。
- [ ] 大项目不会生成超长 SQL。
- [ ] crash 后数据库一致性策略明确。

## P1: Pipeline Parallelism

- [ ] 将全项目索引拆成 Reader -> Parser -> Visitor/Emitter -> Writer 四阶段流水线。
- [ ] Reader 负责文件发现、大小过滤、语言识别、读取/mmap。
- [ ] Parser worker 复用 tree-sitter parser，不按文件反复创建。
- [ ] Visitor/Emitter worker 复用 visitor arena，减少 malloc/free。
- [ ] Writer 单独串行写 SQLite，持续消费 batch，避免 parser 等数据库。
- [ ] 增加 backpressure，防止队列过大导致内存峰值失控。

验收标准：

- [ ] CPU 利用率提升，SQLite 写入和 parse/visit 有重叠。
- [ ] 峰值 RSS 可控。
- [ ] 同等准确性下 Full Index Time 下降。

## P1: Memory And String Cost

- [ ] 对高频字符串做 string interning：kind、language、file path prefix、symbol names。
- [ ] 在 SemanticUnit/SemanticRecord 中减少不必要的 `std::string` 拷贝。
- [ ] Visitor 内部 vector/unordered_map 做 arena/reuse。
- [ ] 对路径存储做规范化和去重，避免每条记录复制完整 path。
- [ ] 审计 vector serialization/deserialization，避免循环内重复构造临时 string。

验收标准：

- [ ] RSS 降低。
- [ ] allocator profile 中 malloc/free 次数下降。

## P2: Graph Construction

- [ ] `GraphBuilder::buildSymbolGraph` 增加 nearest graph parent cache，避免重复 parent chain walk。
- [ ] `GraphStore::buildGraph` 对 `_r2n` 增加必要临时索引，特别是 `(file_path, original_id)`、`name`。
- [ ] call edge 构建区分 local/external/name-only，减少无效 name join。
- [ ] 对 on-demand call graph 查询做专门索引和 benchmark。
- [ ] 补齐 CALLS 图质量，避免图工具看不到真实调用关系。

验收标准：

- [ ] buildGraph 阶段耗时下降。
- [ ] callers/callees 查询延迟下降。
- [ ] CALLS 边数量和准确率有可解释提升。

## P2: Fast / Normal / Deep Modes

- [ ] Fast：轻量 scanner，只提 function/class/struct/import 和简单 call，目标 TTFA。
- [ ] Fast scanner：把 `detectDecl` 按语言拆分，或改为 table-driven matcher，降低复杂度和误判风险。
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

- [ ] 选 3 个固定对标项目：小型、中型、大型。
- [ ] 对每个项目记录 `codebase-memory-mcp` 的 index time、query latency、node/edge 数、RSS。
- [ ] CodeScope 在 Fast 模式 TTFA 明显优于 `codebase-memory-mcp`。
- [ ] CodeScope 在 Normal 模式 Full Index Time 接近或优于 `codebase-memory-mcp`。
- [ ] CodeScope 在 Deep 模式提供更强能力：更高质量 call graph、CFG/metrics、增量更新、可解释上下文。
- [ ] 建立 regression dashboard：性能不能靠主观感受。

## Immediate Next Tasks

- [x] 先修 `searchSemantic` query vector 重复反序列化。
- [x] 给 `engine_index_file` 加 `ir_node_id` map（batch 路径已审计无需改动）。
- [x] 给 `engine_index_project` 增加 language filter 和 max file size 预解析。
- [x] 修复分阶段计时精度（per-batch 累加替代 cumulative wall time 减法）。
- [x] 升级 `test_bench_project`：解析 JSON 分阶段耗时 + query benchmark，固定清除 WAL/SHM 文件。
- [x] 跑 benchmark 记录 baseline。
- [x] 确认瓶颈分布：**SQLite 47.7% > buildGraph 42.8% > Parse 7.4%**。
- [x] 结论：不进入流水线重构。串行 SQL 阶段（buildGraph + SQLite = 90.5%）是绝对瓶颈，并行化 Parse 收益已到天花板。
- [x] 优化 `buildGraph`：`IN (...)` 改为临时表 join + `_r2n` 索引 + 修复 `_r2n` 缺失 name 列 bug。
- [x] 优化 SQLite Write Path：WAL mode、synchronous OFF、temp_store MEMORY、cache_size 64MB——已在 `open()` 中配置。
- [x] `insertSemanticRecords` 支持跨文件 batch 输入——新增 `insertSemanticRecordsBatch()` 方法，prepare 一次插入所有文件记录。
- [x] 给 `semantic_records` 增加缺失索引：`(project_id, file_path)`、`(file_path, original_id)`、`(project_id, kind)`。
- [x] 再次 benchmark 验证：**SQLite 时间稳定在 ~154ms**（受限于实际 I/O 写入，batch insert 对 prepare/finalize 的节省在此数据集不显著）。

**最新 Benchmark (72 C++ 文件，clean DB)：**
- **SQLite: 154ms (47.7%)** ← INSERT I/O 瓶颈
- **buildGraph: 138ms (42.8%)** ← SQL JOIN 瓶颈
- **Parse: 24ms (7.4%)** ← 已很高效
- **Total: 322ms**

**下一步方向（P1/P2 混合）：**
- [ ] 审计 `insertGraphNode` / `insertGraphEdge` 调用路径——old pipeline 中 `EmitGraphPass` 单条插入，改用 batch insert API。
- [ ] `GraphBuilder::buildSymbolGraph` 增加 nearest graph parent cache（当前每 record 遍历 parent chain 查找最近图节点）。
- [ ] 对 `node_vectors` / `code_fts` insert 路径做 same-statement reuse 审计。
- [ ] 建立大项目（1000+ 文件）benchmark，暴露 IN-clause 退化。
- [ ] 跑 Rust server 测试，验证 MCP query 延迟。

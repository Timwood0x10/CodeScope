# CodeScope 架构拆解（八）：存储层——SQLite WAL + FTS5 + vec0

> 我知道这听起来有点反直觉：一个代码分析引擎，为什么用 SQLite 做存储？
>
> 我试过其他方案。Neo4j 的调用图查询很优雅，但为了一个 CLI 工具让用户装 JDK + Neo4j 就太离谱了。RocksDB 的写入性能出色，但想做个 `LIKE '%query%'` 就得自己搭布隆过滤器。至于内存里裸放 `HashMap<String, Vec<Node>>`——查询确实快，300ms 搞定调用链。但进程一挂，全部白干，增量索引更是不用想。
>
> SQLite 的好处是：它不需要部署。不需要单独的进程，不需要配置，不需要用户安装任何东西。代码里 `sqlite3_open()` 一行就搞定了。坏处是：你得在 22 张表 + FTS5 + vec0 + WAL 之间手工编排一个本应用多模型数据库才能解决的问题。

---

## 三个问题

1. **单机单文件数据库，如何同时担任关系存储（图节点/边）、全文搜索（FTS5）和向量检索（vec0）三种角色？**
2. **5000 个文件的索引过程中，插入 100 万条记录的同时还要响应查询，SQLite 的锁机制如何处理？**
3. **增量索引需要检测文件变更、只重建变更部分，而 SQLite 本身没有文件系统监听能力——这个缺口怎么填？**

---

## 28 张表的设计

先看完整的 Schema 全景。28 张表按索引阶段划分：

```
Phase A（快速扫描 — 毫秒级 就绪）
├── projects              // 根表，存储项目根路径和名称
├── files                 // 文件元数据（路径、语言、内容哈希）
├── modules               // 模块/目录层次结构
├── symbols               // 符号定义（核心）
├── symbol_status         // 每个符号的就绪标志
├── entry_points          // 入口点标记
├── file_scan_state       // 增量索引的 mtime/哈希跟踪
├── index_tasks           // Tokio 后台任务状态

Phase B（知识增强 — 异步后台）
├── call_edges            // 调用图边（caller → callee）
├── dependency_edges      // 模块间依赖
├── metrics               // 圈复杂度、认知复杂度
├── search_index          // FTS5 带摘要和正文

Phase C（深度索引）
├── ir_nodes              // 遗留 IR 节点（旧管道）
├── ir_semantic_edges     // 遗留语义边
├── graph_nodes           // 图节点（最终产物）
├── graph_edges           // 图边（调用/包含/继承）
├── semantic_records      // 新管道扁平记录（DB-first）
├── node_complexity       // 复杂度分数
├── node_vectors          // n-gram 哈希向量 BLOB
├── code_fts              // FTS5 全文搜索索引
├── fts_node_map          // FTS 节点ID映射
├── embeddings            // vec0 384维嵌入表（可选）
├── type_info             // 类型定义（struct/enum/trait/interface）
├── type_ref              // 类型引用（变量 : 类型 映射）
├── routes                // HTTP 路由注册（method + path + handler）
├── capabilities          // 模块能力声明
├── contracts             // 契约/接口实现声明
├── findings              // 验证证据链持久化
```

这些表不是同时创建的。`engine_init()` 在启动时创建全部 Schema，但填充数据的时机不同——Phase A 产生 `symbols` 和 `modules`，Phase B 填充 `call_edges` 和 `metrics`，Phase C 建立完整的 `graph_nodes` + `graph_edges`。

---

## 打开数据库的那几行代码

存储层的一切设计都围绕这几行 PRAGMA：

```cpp
// engine/src/store/store.cpp — GraphStore::open()
bool GraphStore::open(const char *db_path) {
    sqlite3_open(db_path, &db_);

    exec("PRAGMA journal_mode=WAL");           // 并发读取 + 写入
    exec("PRAGMA synchronous=OFF");            // 批量插入最大吞吐量
    exec("PRAGMA temp_store=MEMORY");          // 临时表在 RAM 中
    exec("PRAGMA cache_size=-64000");           // 64 MB 页缓存
    exec("PRAGMA mmap_size=268435456");         // 256 MB 内存映射 I/O
}
```

**WAL（Write-Ahead Logging）** 是整个并发方案的基础。没有 WAL，SQLite 默认的 journal 模式会在写入时锁定整个数据库——索引过程中所有查询都得等着。WAL 允许：
- 写入者在 WAL 文件中追加记录
- 读取者继续从原数据库文件读取
- 写入完成后，checkpoint 将 WAL 合并到主数据库

**synchronous=OFF** 是性能的关键。在批量索引场景中，每次 fsync 的代价是 ~2-10ms。关闭它后每次插入降为 ~0.5ms。代价是：如果机器在事务中崩溃，可能丢失最后一批数据。但对于代码索引——数据可以从源代码重新生成——这个权衡是可以接受的。

**mmap_size=256MB** 启用内存映射 I/O。读取时 SQLite 可以直接访问内核的页缓存，避免 `read()` 系统调用和用户态内存拷贝。对大表全表扫描（如 `node_vectors` 的暴力搜索）有显著提升。

---

## 核心模式：Batch Insert

批量插入使用了固定的模式——prepare 一次，bind/step/reset 循环：

```cpp
void GraphStore::insertGraphNodes(uint64_t project_id,
                                   const std::vector<graph::GraphNode> &nodes) {
    const char *sql = "INSERT INTO graph_nodes (...) VALUES (?, ?, ...)";
    sqlite3_stmt *stmt = nullptr;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

    for (auto &node : nodes) {
        sqlite3_bind_int64(stmt, 1, node.id);
        sqlite3_bind_text(stmt, 2, node.name.c_str(), -1, SQLITE_TRANSIENT);
        // ... bind all fields
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
}
```

注意这里没有显式的 `BEGIN TRANSACTION` / `COMMIT`。SQLite 在 `sqlite3_step()` 返回 `SQLITE_ROW` 或 `SQLITE_DONE` 时会自动开始一个隐式事务——也就是说上面这个循环，每插入一条节点就提交一次。

对于批量构建场景，这种模式被显式事务替代：

```cpp
exec("BEGIN");
for (auto file's semantic records) {
    // 复用已准备的 stmt，在一次事务中插入所有记录
}
exec("COMMIT");
```

代码注释记录了这个问题：

```
// Previously each storeVector auto-committed — 261K transactions.
// With explicit begin/commit, all inserts are one transaction.
```

261K 个事务 vs 1 个事务——差了几个数量级的性能。

---

## buildGraph()：从扁平记录到多维图

这是存储层中最有意思的 SQL 操作。`buildGraph()` 接受 `semantic_records`（扁平记录列表）并构建 `graph_nodes` + `graph_edges`。

核心是三步 SQL：

**Step 1: 记录到节点的映射**

```sql
CREATE TEMP TABLE _r2n AS
SELECT sr.rowid as rid, sr.original_id, sr.file_path, sr.name,
       CAST(ROW_NUMBER() OVER ()
         + COALESCE((SELECT MAX(id) FROM graph_nodes WHERE project_id=?), 0)
         AS INTEGER) as node_id
FROM semantic_records sr
WHERE sr.project_id = ?
  AND sr.kind IN (0,1,2,3,4,5,6,9,10)   -- 只取语义节点
  AND sr.file_path IN (SELECT file_path FROM _rf)
```

注释特别说明了 `ROW_NUMBER() OVER ()` 的用意：

```
// Note: ROW_NUMBER() OVER () avoids ORDER BY sort cost.
// Node IDs are sequential but not sorted by file_path — sorting is
// not required for correctness since JOINs use indexes, not sequential scans.
```

**Step 2: 从声明插入图节点**

```sql
INSERT INTO graph_nodes (id, project_id, ..., node_type, ...)
SELECT r2n.node_id, ..., CASE sr.kind
    WHEN 0 THEN 0     -- Function
    WHEN 1 THEN 1     -- Method
    WHEN 2 THEN 2     -- Class
    WHEN 4 THEN 4     -- Interface
    WHEN 9 THEN 9     -- CallExpr
    ELSE 7            -- TypeAlias (fallback)
END, ...
FROM semantic_records sr
JOIN _r2n r2n ON sr.rowid = r2n.rid
```

**Step 3: 调用边（跨文件名称匹配）**

```sql
INSERT INTO graph_edges (id, project_id, source_node_id, target_node_id,
                          edge_type, graph_type, call_site_file, call_site_line)
SELECT [...], 1, caller.node_id, callee.node_id, [...]
FROM semantic_records sr
JOIN _r2n callee ON SUBSTR(sr.name, -LENGTH(callee.name)) = callee.name
JOIN _r2n caller ON sr.parent_id = caller.original_id AND [...]
WHERE sr.kind = 9    -- CallExpr
```

这个跨文件调用解析的精髓在 `SUBSTR(sr.name, -LENGTH(callee.name)) = callee.name`——用后缀匹配做名称解析。不需要类型检查，不需要重载解析，甚至不需要知道函数在哪个文件定义。只要调用表达式的名称结尾等于目标函数的名称，就建立调用边。

**一个 42 个文件的 JPetStore 构建时间**：

```
buildGraph: 42 files | file_list=5ms delete=12ms rf=3ms r2n=45ms nodes=120ms edges=30ms calls=80ms total=295ms
```

从 42 个文件、数千条 `semantic_records` 到完整的图结构——总共 **295ms**。

---

## 三重搜索体系

存储层维护了三套独立的搜索索引，对应不同的查询场景：

### 1. FTS5 全文搜索

```sql
-- 遗留索引
CREATE VIRTUAL TABLE code_fts USING fts5(
    name, qualified_name, file_path, content,
    project_id UNINDEXED, node_id UNINDEXED, node_kind UNINDEXED,
    tokenize='unicode61'
);

-- 新索引（带排序的摘要/正文）
CREATE VIRTUAL TABLE search_index USING fts5(
    title, summary, body,
    project_id UNINDEXED, symbol_id UNINDEXED,
    tokenize='unicode61'
);
```

`unicode61` 分词器支持 Unicode 文本，包括中文 CJK 字符的分词——这对分析包含中英文混杂的代码库很重要。

`searchCode()` 的查询逻辑将用户输入转为 FTS5 前缀匹配：

```
用户输入: "vector_store"
FTS5 查询: "vector_store*"
匹配结果: "vector_store", "vector_store_impl", "vector_store_test"
```

结果按节点类型排序——函数/类声明优先，然后按 FTS5 `rank`（BM25 相关性评分）。

### 2. n-gram 哈希向量搜索

这是自制的语义搜索，不需要外部嵌入模型：

```cpp
// engine/src/query/vector_search.h
const int VECTOR_DIM = 64;

void stringToVector(const std::string &str, float *vec) {
    // 提取所有 2-gram 和 3-gram
    // 哈希到 [0, VECTOR_DIM) 索引
    // 构建频率向量 → L2 归一化
}
```

搜索逻辑是暴力扫描：

```cpp
// store.cpp — searchSemantic()
for (each node_vector) {
    float sim = cosineSimilarity(query_vec, node_vec);
    if (sim > 0.1f) candidates.push_back({node_id, sim});
}
partial_sort(candidates, topK);  // O(N log K)
```

`partial_sort` 将复杂度从 O(N log N) 降到 O(N log K)。在 <100K 节点时暴力扫描是可行的。

### 3. vec0 嵌入搜索（可选）

```sql
CREATE VIRTUAL TABLE IF NOT EXISTS embeddings USING vec0(
    symbol_id INTEGER PRIMARY KEY,
    vector FLOAT[384]
);
```

这个表不是无条件创建的。`engine_init()` 尝试通过 `sqlite3_load_extension()` 加载 `vec0.dylib`——如果用户的系统上没有这个扩展（比如 Windows 或非标准安装），核心功能照样工作，只是回退到 n-gram 哈希搜索。

---

## 增量索引的设计

存储层通过 `file_scan_state` 表实现增量索引：

```sql
CREATE TABLE file_scan_state (
    project_id INTEGER,
    file_path TEXT,
    file_mtime INTEGER,
    file_size INTEGER DEFAULT 0,
    content_hash TEXT,
    scanned_at TEXT,
    PRIMARY KEY(project_id, file_path)
);
```

三个关键方法：

```cpp
// 检查文件是否未变更
bool GraphStore::isFileUnchanged(project_id, file_path, mtime, size) {
    // SELECT 1 FROM file_scan_state WHERE
    //   project_id=? AND file_path=?
    //   AND file_mtime=? AND file_size=?
}

// 更新扫描状态
void GraphStore::updateFileScanState(project_id, file_path, mtime, size) {
    // INSERT OR REPLACE INTO file_scan_state
}

// 清理已删除的文件
void GraphStore::cleanupStaleFiles(project_id, active_files) {
    // CREATE TEMP TABLE _active_files (path TEXT PRIMARY KEY)
    // INSERT active files
    // DELETE FROM file_scan_state WHERE path NOT IN _active_files
}
```

`buildGraph()` 接受一个可选的 `changed_files` 参数——只重建变更文件的图节点和边，而不是全量重建。

这个设计有个隐含的前提：**文件的 mtime 和 size 是足够可靠的变更检测器**。对于 git checkout 切换分支的场景，被恢复的文件 mtime 会变，size 不变——但仍然会触发重建（mtime 变了）。`content_hash` 字段存在但实际使用中优先级低于 mtime+size（查一次哈希需要读整个文件，代价远高于 stat）。

---

## 就绪系统与自适应查询

五个就绪标志分散在 `project_readiness` 和 `symbol_status` 两张表中：

```sql
-- 项目级就绪标志
CREATE TABLE project_readiness (
    project_id INTEGER PRIMARY KEY,
    fast_ready INTEGER DEFAULT 0,
    normal_ready INTEGER DEFAULT 0,
    deep_ready INTEGER DEFAULT 0,
    fts_ready INTEGER DEFAULT 0,
    vector_ready INTEGER DEFAULT 0
);

-- 每个符号的细粒度就绪标志
CREATE TABLE symbol_status (
    symbol_id INTEGER PRIMARY KEY,
    callgraph_ready INTEGER DEFAULT 0,
    metrics_ready INTEGER DEFAULT 0,
    embedding_ready INTEGER DEFAULT 0,
    is_stub INTEGER DEFAULT 0
);
```

这种设计允许查询引擎在"项目级"和"符号级"两个粒度上做 fallback：

- 如果项目 `fast_ready=0`：不返回结果（扫描都没完成）
- 如果项目 `fast_ready=1` 但 `callgraph_ready=0`：返回符号信息但不返回调用者/被调用者
- 如果符号 `is_stub=1`：该函数只有声明没有实现（作为占位符标记）

`getReadyRatio()` 计算就绪进度：

```sql
SELECT CAST(SUM(ss.callgraph_ready) AS REAL) / COUNT(*)
FROM symbols s JOIN symbol_status ss ON ss.symbol_id = s.id
WHERE s.project_id = ?
```

这用来支持查询引擎的 "渐进式就绪" 特性——Phase A 完成后就告诉用户"我扫描完了，30% 的符号有调用图信息"，而不用等到 Phase B 全部完成。

---

## 导出/导入：VACUUM INTO + zstd

共享产物的设计走了捷径——但也足够聪明：

**导出**：
```
1. VACUUM INTO 'temp_path'    -- SQLite 3.27.0+，创建原子快照
2. fork + execlp('zstd', '--compress', ..., temp_path)
3. 移除临时文件
```

**导入**：
```
1. fork + execlp('zstd', '-d', ..., input_path)
2. ATTACH DATABASE artifact AS import_db
3. INSERT OR IGNORE INTO semantic_records, graph_nodes, graph_edges
4. DETACH, 清理
```

`VACUUM INTO` 的美妙之处在于：它创建一个原子的、去碎片化的数据库副本，不需要锁定原始数据库。整个过程中原始数据库仍然可读可写。

防御注入的方式是检查路径中的 shell 元字符：

```cpp
bool pathHasMeta(const std::string &path) {
    return path.find_first_of(";&|$`\\'\"") != std::string::npos;
}
```

并使用 `fork+execvp` 而非 `system()`——避免 shell 解释。

---

## 设计取舍

### 优点

1. **零部署依赖**。不需要 MySQL、PostgreSQL、Neo4j。`brew install sqlite` 就够了——甚至 macOS 自带 sqlite3。
2. **单文件备份**。整个项目的数据在一个 `.db` 文件中。迁移、复制、分享都非常简单。
3. **并发读取**。WAL 模式下，Phase B/C 在后台写入，Phase A 查询完全不受影响。
4. **SQL 的力量**。`buildGraph()` 的核心逻辑用 5 条 SQL 语句完成，不到 100 行代码。如果用 Rust 手写同样逻辑，至少 500 行。

### 痛点

1. **没有真正的并发写入**。SQLite 的 WAL 虽然支持读取+写入并发，但不支持两个写入者。在多 Worker 场景下需要序列化写入操作。
2. **暴力向量搜索不可扩展**。`searchSemantic()` 的全表扫描在 <100K 节点可行，但超过百万节点就会显著变慢。vec0 扩展缓解了这个问题，但它是可选依赖。
3. **Schema 缺乏迁移机制**。当前没有版本化的 Schema 迁移。如果表结构变化，只能删库重建。这在开发阶段可以接受，但生产部署会是个问题。
4. **22 张表的冗余**。`ir_nodes` 和 `semantic_records` 存储相似但不相同的数据——前者是旧管道的产物，后者是新管道的。从工程角度看应该只保留一个。

---

## 坦诚反思

**两条管道共存的问题**。

你可能会注意到，`ir_nodes` + `ir_semantic_edges` 和 `semantic_records` + `graph_nodes` 的功能高度重叠。前者是旧管道的产物（通过 `Translator` 接口），后者是新管道的（通过 `JsVisitor` 接口）。

在查询时，`engine_find_callers` 的代码需要写上：

```cpp
// 先尝试新管道（semantic_records）
if (getCallersFromRecords(...)) return;
// 回退到旧管道（ir_nodes）
if (getCallersFromLegacy(...)) return;
```

这反映了从旧系统到新系统的过渡状态。理想情况下应该只有一套表结构。但现实是两条管道并行在线上运行，各自的测试都通过——重构它们的工作量大于让其共存的风险。

---

## 系列导航

| 文章 | 主题 |
|---|---|
| (一) 开篇 | 56KB vs 629 bytes，CodeScope 要解决什么问题 |
| (二) 渐进式就绪 | 毫秒级让 AI 开始理解你的代码 |
| (三) Worker 隔离 | 为什么索引不会拖垮 MCP Server |
| (四) 零冗余响应 | 精简响应，按需返回 |
| (五) C++ 引擎拆解 | 从源码到多维代码图的管线 |
| (六) MCP 协议层 | 35+ 工具的设计哲学 |
| (七) 语言翻译器 | 10 种语言 → 统一 IR |
| **(八) 存储层** | **SQLite WAL + FTS5 + vec0 ← 本文** |
| (九) 自适应查询 | Fallback 机制与就绪检测 |
| (十) 性能真相 | 从 200 到 60,000 文件的实测 |
| (十一) 验证层 | 让 AI 对自己的话负责 |
| (十二) Model Engine | 从事实到理解 |
| (十三) Parser + GraphBuilder | 解析与建图 |

---

下一篇我们拆解 **自适应查询**——当一个查询到达时，引擎如何根据当前的就绪状态选择最优的数据源（Phase A 直查 symbols，Phase B 走 semantic_records，Phase C 走 graph_nodes），以及 fallback 链的详细设计。
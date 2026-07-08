# enhance_project（全量增强）性能优化分析

> 状态：分析完成，第一波改造待实施（ADR 见文末，Status: Proposed）
> 范围：`engine/src/engine_queries.cpp` 的 `engine_enhance_project` 及其依赖的 `store` 写入路径
> 方法：基于源码逐行核实，非推测。所有结论均标注文件:行号。

---

## 1. 它在干什么

`enhance_project` 是 Phase B 的后台全量增强：对项目中**尚未增强**的文件，依次做 4 件事：

1. **调用图**：tree-sitter 解析 → IR 翻译 → 正则提取调用点 → 写 `call_edges`
2. **复杂度指标**：cyclomatic / nesting / cognitive + 行数/参数/分支/循环计数 → 写 `metrics`
3. **向量嵌入**：`name + doc_comment` → n-gram 向量 → 写 `node_vectors`（依赖 sqlite-vec 的 `vec0`）
4. **FTS 索引**：写 `search_index`

入口链路：`tools/mod.rs:h_enhance_project` → `ffi::enhance_project` → C FFI `engine_enhance_project`（`engine_queries.cpp:194`）。Rust 侧 `spawn_enhancement` 在阻塞线程里**同步**跑完整个项目。

**增量性**：通过 `getUnreadyFiles(project_id, "callgraph_ready")`（`engine_queries.cpp:244`）只取未完成的文件；每文件处理完把 `callgraph_ready / metrics_ready / embedding_ready` 三位置 1。因此**首次全量跑是成本主体**，二次调用基本是 no-op。

**已知基线**（README）：Linux 内核 v6.13 全量增强约 **27s**、产生 **45,573 条 call_edges**。下文瓶颈以该量级为参照。

---

## 2. 已确认的瓶颈（逐条按行号核实）

### B1 — 每条调用边一个独立事务【最致命】

- `engine_queries.cpp:447–457`：每个 `insertCallEdge` 外面都包 `beginTransaction()` / `commitTransaction()`。
- `beginTransaction()` = `BEGIN TRANSACTION`、`commitTransaction()` = `COMMIT`（`store.cpp:346 / 350`，已确认）。
- 调用边循环（`346–465`）跑在 metrics 事务（`:472` 的 `beginTransaction`）**之前**，所以调用边根本没并进那个事务。
- 影响：45,573 条边 = **45,573 次 BEGIN/COMMIT**。即便已开 WAL + `synchronous=OFF`（`store_core.cpp:62–66`），每次 COMMIT 仍需拿/放写锁、写 WAL、并可能触发 checkpoint。这是墙钟时间的大头。

### B2 — 所有 store 写入方法每次都重新 `prepare_v2`

- `insertCallEdge`（`store.cpp:1193`）、`insertMetric`、`insertEmbedding`、`insertIntoSearchIndex`、`findSymbolJson` 均在函数开头 `sqlite3_prepare_v2`。
- 标准做法是**编译一次、复用**（`sqlite3_reset` + `sqlite3_clear_bindings` + rebind）。S 个符号处理下来是 S×N 次重复编译，还浪费 SQLite 的计划缓存。

### B3 — 跨文件被调用方查找做了 JSON 往返

- `engine_queries.cpp:421–445`：当前文件缓存找不到 callee 时，调 `findSymbolJson`（`store.cpp:1064`，把整条符号记录序列化成 JSON），随后用 `json.find("\"id\":")` + `strtoull` 把 id 抠回来。
- 既拼出完整 JSON 又用字符串搜回去，纯浪费；且 `findSymbolJson` 可能返回多行全部序列化、只取第一个 id。
- 改法：直接 `SELECT id FROM symbols WHERE project_id=? AND name=? LIMIT 1` 取整数；更彻底是 enhance 启动时建一个**全局 name→id 索引**（per-file 的 `sym_cache` 已有雏形，全局版可消除所有跨文件 DB 查询）。

### B4 — 每个符号 3 次 UPDATE 设就绪位

- `engine_queries.cpp:611/613/615`（函数声明分支）与 `:649/651/653`（非函数声明分支）：`setSymbolReady` 被调 3 次（callgraph / metrics / embedding），每次一条 `UPDATE symbol_status`。
- 可合成一条：`UPDATE symbol_status SET callgraph_ready=1, metrics_ready=1, embedding_ready=1 WHERE symbol_id=?`。

### B5 — 单线程整库顺序跑【墙钟大头，但改动大】

- `engine_queries.cpp:260`：文件级 `for` 循环顺序执行，parse + translate + analyze + 写库全在一个线程。
- Linux 内核 27s 基本被单核绑死。
- 激进改法：文件级线程池并行，每线程独立 SQLite 连接（serialized 模式允许并发连接，写会自动串行化）。收益大但复杂度高（并发安全、部分失败、进度上报）。

### B6 — 重复解析

- `engine_queries.cpp:269`：每个文件用 tree-sitter 从头 `parse`，而 Phase A scan 其实已 parse 过一遍（IR 未落盘）。大文件 parse 占 CPU，相当于翻倍。
- 根治要落盘 IR，属更大的架构改动。

---

## 3. 改造方案与权衡

### 第一波（低风险、高收益，建议直接做）

| ID | 改动 | 收益 | 风险 |
|----|------|------|------|
| **A** | 把单个文件的全部写入合并成一个事务：调用边 + metrics + embedding + FTS + 就绪位，文件处理完一次性 commit | 45K 边场景预计**数量级级别**加速（消除 45K 次提交） | 单文件失败从"只丢那条边的提交"变为"整文件回滚"——但 enhance 幂等可重跑，可接受 |
| **B** | store 写入方法改复用 prepared statement（或 enhance 内把常用语句 prepare 一次） | 消除 S×N 次重复编译 | 低；需保证 reset/clear_bindings 正确 |
| **C** | 跨文件 callee 改 `SELECT id ... LIMIT 1`；可选建全局 name→id 索引 | 消除 JSON 序列化 + 字符串解析；全局索引可省掉所有跨文件查询 | 低；全局索引需考虑同名多符号（取首个/聚合） |
| **D** | 就绪位合成一条 UPDATE | 每符号少 2 次 UPDATE | 极低 |

### 第二波（测完第一波看真实收益再决定）

| ID | 改动 | 收益 | 风险 / 代价 |
|----|------|------|-------------|
| **E** | 文件级并行（线程池 + 每线程独立连接） | 墙钟随核数近线性下降 | 高：并发安全、部分失败恢复、status 上报对齐 |
| **F** | IR 落盘避免重复 parse | 大文件 parse 减半 | 高：存储格式、版本演进、磁盘体积 |

---

## 4. 代价与权衡（总览）

- **A 的事务合并**：失败粒度从"单条边"变"单文件"。可接受，因为 enhance 按 `callgraph_ready` 增量跑、幂等可重跑。
- **B/C/D**：纯内部重构，不改变外部行为，风险极低。
- **E/F**：显著增加系统复杂度，且需要把 status 上报、错误恢复一并对齐。**建议先落地 A–D 实测真实加速，再决定是否上并行。**

---

## 5. ADR：enhance_project 写入路径优化（第一波）

```markdown
# ADR-005: enhance_project 写入路径批处理优化

## Status
Proposed

## Context
全量增强（enhance_project）在 Linux 内核量级（~45K 调用边）下约 27s。
根因分析定位到：每条调用边各自开/关事务（engine_queries.cpp:447-457）、
所有 store 写入每次重新 prepare、跨文件 callee 走 JSON 往返、
每符号 3 次就绪位 UPDATE。瓶颈在 DB 写入方式而非算法。

## Decision
实施第一波（A–D）：
A. 单文件全部写入合并为一次事务，文件处理完统一 commit；
B. store 写入方法复用 prepared statement；
C. 跨文件 callee 改为直接 SELECT id ... LIMIT 1（可选建全局 name→id 索引）；
D. 就绪位三列合并为一条 UPDATE。
暂不做第二波（E 并行 / F IR 落盘），待 A–D 实测后再评估。

## Consequences
- 更易：单次全量增强耗时大幅下降（预期数量级），DB 写入压力显著降低。
- 更难：单文件失败回滚粒度变粗（整文件而非单条边）；需保证幂等重跑正确。
- 不变：enhance 对外行为、增量语义、MCP 接口保持兼容。
```

---

## 6. 验证建议

1. 取一个中大型项目（或 Linux 内核样例）跑 `enhance_project`，记录 `engine_get_enhancement_status` 的 `total / callgraph_ready / metrics_ready / embedding_ready` 与耗时。
2. 实施 A–D 后同条件复测，对比 `call_edges` 数量一致（数据正确性）与耗时（性能）。
3. 确认 sqlite-vec 的 `vec0` 已静态注册（否则 embedding 表建不出，语义检索残废）——见构建阶段已修复的 `HAVE_SQLITE_VEC` + `sqlite3_auto_extension`。

---

## 7. 实施计划（第一波 A–D，可直接照做）

> 编码规范：`plan/rules/code_rules.md`（文件 ≤1000 行、注释用英文、错误禁止静默、禁 `git commit`）。
> 推荐落地顺序：**B（基础设施）→ A → C → D**，因为 C/D 可直接复用 B 的 `getCachedStmt`。
> 注：`insertEmbedding` 已用缓存语句 `stmt_vector_`（`store.cpp:1343`），证明项目本就有复用先例，B 风险低。

### 7.1 B — store 增加语句缓存（基础设施）

**目标**：所有 Phase B 写入方法编译一次 SQL、重复使用，消除 S×N 次 `prepare_v2`。

**`engine/src/store/store.h`**（私有段，紧挨 `stmt_vector_` 声明 `:406–408`）：
```cpp
// Cache of prepared statements keyed by SQL text, reused across calls.
// Guarded by stmt_cache_mutex_ because g_store may be shared across tasks.
std::unordered_map<std::string, sqlite3_stmt*> stmt_cache_;
std::mutex stmt_cache_mutex_;
// Returns a cached + reset prepared statement for `sql`. Prepares on first
// use; caller must bind + step, and must NOT finalize (owned by the cache).
// Returns nullptr on prepare failure (error_ is set).
sqlite3_stmt* getCachedStmt(const char* sql);
```
> 确保头文件已包含 `<mutex>` 与 `<unordered_map>`（若未包含则补）；`sqlite3_stmt` 前向声明已在 `:12`。

**`engine/src/store/store.cpp`**：
1. 实现 `getCachedStmt`（放在事务方法附近）：
```cpp
sqlite3_stmt* GraphStore::getCachedStmt(const char* sql) {
    if (!sql) return nullptr;
    std::lock_guard<std::mutex> lk(stmt_cache_mutex_);
    auto it = stmt_cache_.find(sql);
    if (it != stmt_cache_.end()) {
        sqlite3_reset(it->second);
        sqlite3_clear_bindings(it->second);
        return it->second;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error_ = std::string("getCachedStmt: prepare failed: ") + sqlite3_errmsg(db_);
        return nullptr;
    }
    stmt_cache_[sql] = stmt;
    return stmt;
}
```
2. 在 `~GraphStore()` 析构里 finalize 全部缓存语句（避免句柄泄漏）：
```cpp
for (auto& kv : stmt_cache_) sqlite3_finalize(kv.second);
stmt_cache_.clear();
```
3. 把以下方法的 `sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr)` 替换为 `stmt = getCachedStmt(sql)`，并**删除**各自的 `sqlite3_finalize(stmt)`（语句归缓存所有，不每调用释放）：
   - `insertCallEdge`（`:1193`）
   - `insertMetric`（`:1258`）
   - `insertIntoSearchIndex`（`:1292`）
   - `findSymbolJson`（`:1071`）
   - （可选）`insertEmbedding` 中第二处 `INSERT INTO embeddings` 的 prepare（`:1364`）——主路径已用 `stmt_vector_`，此处影响不大，可顺手改。
4. prepare 失败分支原本 `sqlite3_finalize(stmt); return ...` 改为直接 `return ...`（无需 finalize 缓存句柄）。

**注意点**：缓存受 `mutex` 保护，`g_store` 即便被 scan/enhance 并发共享也安全（当前 enhance 单线程，mutex 只是加固）。

---

### 7.2 A — 单文件写入合并为一个事务

**目标**：消除 45,573 次 BEGIN/COMMIT，改为每文件 1 次。

**`engine/src/engine_queries.cpp`，`engine_enhance_project`**：

1. **删除**调用边循环里的逐边事务（`447–457` 内）：
```cpp
// 删除这两行：
g_store->beginTransaction();
g_store->commitTransaction();
// 保留：
g_store->insertCallEdge(project_id, caller_id, callee_id, "static",
                        static_cast<int>(call_line), 0);
total_edges++;
```
2. **新增**一个事务起点：放在 `if (ir_id_to_symbol.empty()) continue;`（`:319` 之后）紧接着，包住调用边 + func_ranges + metrics + embedding + FTS + 就绪位：
```cpp
// Begin a single transaction for the whole file (call edges + metrics +
// embeddings + FTS + ready flags). All earlier `continue` paths exit before
// this point, so no transaction is ever left dangling.
if (!g_store->beginTransaction()) {
    fprintf(stderr, "enhance: beginTransaction failed: %s\n",
            g_store->error().c_str());
    continue;
}
```
3. **删除** metrics 块自身的 `beginTransaction()`（`:472`）与 `commitTransaction()`（`:639`）。
4. **新增**一个提交点：放在 metrics 块结束、`total_files_processed++`（`:642`）之前：
```cpp
if (!g_store->commitTransaction()) {
    fprintf(stderr, "enhance: commitTransaction failed: %s\n",
            g_store->error().c_str());
    // Partial file data may remain; enhance is idempotent and re-runs
    // via callgraph_ready, so we do not abort the whole pass.
}
total_files_processed++;
```

**正确性**：所有 `continue`（解析失败等）都在事务开始前（`:319` 之后才 begin），不会留下悬挂事务；文件块内只有子循环 `continue`（跳符号，不跳出文件）。

---

### 7.3 C — 跨文件 callee 直接查 id（去掉 JSON 往返）

**目标**：`engine_queries.cpp:421–445` 的 `findSymbolJson` + `json.find("\"id\":")` 替换为一条整数查询。

**`engine/src/store/store.h`**（公共方法段，紧跟 `findSymbolJson` 声明 `:212`）：
```cpp
// Resolve a symbol id by exact name within a project (cross-file calls).
// Returns the first matching id, or 0 if none. Uses LIMIT 1.
uint64_t findSymbolIdByName(uint64_t project_id, const char* name);
```

**`engine/src/store/store.cpp`**（紧挨 `findSymbolJson` `:1064`）：
```cpp
uint64_t GraphStore::findSymbolIdByName(uint64_t project_id, const char* name) {
    if (!name) return 0;
    const char* sql =
        "SELECT id FROM symbols WHERE project_id = ? AND name = ? LIMIT 1";
    sqlite3_stmt* stmt = getCachedStmt(sql);
    if (!stmt) {
        error_ = "findSymbolIdByName: prepare failed";
        return 0;
    }
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    uint64_t id = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    return id;
}
```

**`engine/src/engine_queries.cpp`**，把 `421–445` 整段替换为：
```cpp
if (callee_id == 0) {
    // Cross-file callee: direct id lookup (no JSON round-trip).
    callee_id = g_store->findSymbolIdByName(project_id, name.c_str());
}
```

---

### 7.4 D — 就绪位三列合并为一条 UPDATE

**目标**：`engine_queries.cpp:611/613/615` 与 `:649/651/653` 的 3×`setSymbolReady` 合并为 1 次。

**`engine/src/store/store.h`**：
```cpp
// Mark a symbol fully enhanced: sets callgraph_ready, metrics_ready and
// embedding_ready in one UPDATE. Replaces three setSymbolReady calls.
bool markSymbolEnhanced(uint64_t symbol_id);
```

**`engine/src/store/store.cpp`**（紧挨 `setSymbolReady` `:1383`）：
```cpp
bool GraphStore::markSymbolEnhanced(uint64_t symbol_id) {
    const char* sql =
        "UPDATE symbol_status SET callgraph_ready=1, metrics_ready=1, "
        "embedding_ready=1 WHERE symbol_id = ?";
    sqlite3_stmt* stmt = getCachedStmt(sql);
    if (!stmt) {
        error_ = "markSymbolEnhanced: prepare failed";
        return false;
    }
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(symbol_id));
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        error_ = "markSymbolEnhanced: step failed";
        return false;
    }
    return true;
}
```

**`engine/src/engine_queries.cpp`**：
- 函数声明分支（原 `611/613/615`）替换为：
```cpp
g_store->markSymbolEnhanced(sym_id);
total_enhanced++;
```
- 非函数声明分支（原 `649/651/653`）替换为同样一行 `g_store->markSymbolEnhanced(sym_id);`。
- 原 `setSymbolReady` 方法保留（仍服务于 `fast_ready`/`fts_ready` 等其它字段），本改不动它。

---

### 7.5 改动清单汇总

| 文件 | 方法 / 位置 | 改动 |
|------|------------|------|
| `engine/src/store/store.h` | 私有段 `:406`、公共段 `:212/:248` | 增 `stmt_cache_`/`stmt_cache_mutex_`/`getCachedStmt`；增 `findSymbolIdByName`、`markSymbolEnhanced` 声明 |
| `engine/src/store/store.cpp` | 析构、`getCachedStmt`、5 个写入方法、`findSymbolIdByName`、`markSymbolEnhanced` | 实现缓存 + 两个新方法；写入方法改用 `getCachedStmt` 并去 `finalize` |
| `engine/src/engine_queries.cpp` | `engine_enhance_project` `:319/:447/:472/:611/:639/:649` | 单文件事务；跨文件查 id；就绪位合并 |

### 7.6 自我校验（落地后必做）

1. **编译**：`make build-engine`，确认无未定义符号（`getCachedStmt` 等）。
2. **正确性**：同项目跑 `enhance_project`，对比 `engine_get_enhancement_status` 的 `call_edges` 总数与改前一致（数据不丢）。
3. **性能**：对比 Linux 内核样例或自有大库的耗时（预期数量级下降）。
4. **降级**：确认 `vec0` 不可用时不崩（已有容错），语义检索仍可走 `node_vectors`。

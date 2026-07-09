# enhance_project（全量增强）性能优化分析

> 状态：代码核实完成（2026-07-08）。第一波 A–D **全部未实施**（代码中无 `stmt_cache_` / `getCachedStmt` / `findSymbolIdsByName` / `markSymbolEnhanced`；每条调用边仍独立事务 `engine_queries.cpp:476,486`；metrics 仍独立事务 `:501,668`）。**ADR-005 已 Accepted**（A 改为 A3 每文件事务+每符号 SAVEPOINT，详见 §13 与 §7.2）；ADR-006 仍为 Proposed。
> 范围：`engine/src/engine_queries.cpp` 的 `engine_enhance_project`、`engine/src/store/store.cpp` 的 `buildGraph`、`engine/src/engine_index.cpp` 的并行索引路径
> 方法：基于源码逐行核实（本次会话已 grep 核实 store.h/.cpp/engine_queries.cpp/store_core.cpp/engine_index.cpp 全部关键行），非推测。

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

> **关于大型项目（§8–§13 详细展开）**：27s 是 ~45K 边 + 单线程产生的基线。当项目增长到 30K–70K 文件时（Chromium / LLVM / Linux kernel），墙钟主体变为 **enhance 单线程 parse CPU**（A–D 碰不到）与 **buildGraph 边索引维护**。真正要命的不是文件数量，而是**单大文件的内存爆炸**——详见 §8–§13。

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

- `engine_queries.cpp:289`：文件级 `for` 循环顺序执行，parse + translate + analyze + 写库全在一个线程。
- Linux 内核 27s 基本被单核绑死。
- 激进改法：文件级线程池并行，每线程独立 SQLite 连接（serialized 模式允许并发连接，写会自动串行化）。收益大但复杂度高（并发安全、部分失败、进度上报）。

### B6 — 重复解析

- `engine_queries.cpp:298`：每个文件用 tree-sitter 从头 `parse`，而 Phase A scan 其实已 parse 过一遍（IR 未落盘）。大文件 parse 占 CPU，相当于翻倍。
- 根治要落盘 IR，属更大的架构改动。

### B7 — g_store 全局单例，并行 enhance 没有独立连接【E 的前置阻塞项】

- `g_store` 定义在 `engine.cpp:39`：`std::unique_ptr<store::GraphStore> g_store`，整个进程共享一个 SQLite 连接。
- 当前 enhance 单线程，共享一个连接没有竞争问题。
- 但要上并行 enhance（§10-E），每个 worker 需要独立 SQLite 连接：
  - 同一个 `db_` 句柄不能并发跑多个 `sqlite3_step`（SQLite 不是线程安全的 per-connection）。
  - 即使 `SQLITE_CONFIG_SERIALIZED` 已开，也只保证不同连接之间的安全，不解决同连接内并发 step。
  - `beginTransaction()` 是裸 `BEGIN TRANSACTION`，不支持嵌套——worker A 开始事务后，worker B 再在同一连接上 `BEGIN` 会报错 `cannot start a transaction within a transaction`。
  - 解决方案：`GraphStore` 增加 `clone()` 方法（或独立构造器）打开一个指向同一个 `.db` 文件的新连接；每 worker 持自己的 `GraphStore` 实例。`g_store` 保留给查询（只读路径）。
- 归入 §10-E 的前置条件：**并行 enhance 必须先解耦 `g_store` 单连接限制。**

---

## 3. 改造方案与权衡

### 第一波（低风险、高收益，建议直接做）

| ID | 改动 | 收益 | 风险 |
|----|------|------|------|
| **A** | **A3：每文件一个外层事务（BEGIN）+ 每个符号一个 SAVEPOINT**（符号级硬失败 ROLLBACK TO 该符号、同文件其他符号不受影响）；文件尾一次性 COMMIT | 45K 边场景预计**数量级级别**加速；符号级容错优于 A1 整文件回滚 | 代码复杂度略增（需显式管理 savepoint 生命周期）；vec0 savepoint 回滚需验证，否则 embedding 写置于 savepoint 外、靠重跑自愈 |
| **B** | store 写入方法改复用 prepared statement（或 enhance 内把常用语句 prepare 一次） | 消除 S×N 次重复编译 | 低；需保证 reset/clear_bindings 正确 |
| **C** | 跨文件 callee：启动时建全局 `name→list<SymbolRef>` in-memory 多值映射（O(1)、容重名），候选按"同文件>同namespace>全局"排序；≤K 全插、>K 只插同文件/namespace；超大项目回退 `findSymbolIdsByName` | 消除 JSON 序列化 + 字符串解析 + 每次 DB 查询；正确解析 static 跨文件/重载 | 低；in-memory 映射在超大项目占数百 MB（结束释放） |
| **D** | 就绪位合成一条 UPDATE | 每符号少 2 次 UPDATE | 极低 |

### 第二波（测完第一波看真实收益再决定）

| ID | 改动 | 收益 | 风险 / 代价 |
|----|------|------|-------------|
| **E** | 文件级并行（线程池 + 每线程独立连接） | 墙钟随核数近线性下降 | 高：并发安全、部分失败恢复、status 上报对齐 |
| **F** | IR 落盘避免重复 parse | 大文件 parse 减半 | 高：存储格式、版本演进、磁盘体积 |

---

## 4. 代价与权衡（总览）

- **A 改为 A3（每文件事务 + 每符号 SAVEPOINT）**：失败粒度从"单条边"收紧到"单符号"（ROLLBACK TO 该符号、同文件其他符号不受影响），且仍只有 1 次 COMMIT/文件。代价：需显式管理 savepoint 生命周期 + 验证 vec0 的 savepoint 回滚（不支持则 embedding 写置于 savepoint 之外、靠重跑自愈）。
- **B/C/D**：纯内部重构，不改变外部行为，风险极低。
- **E/F**：显著增加系统复杂度，且需要把 status 上报、错误恢复一并对齐。**建议先落地 A–D 实测真实加速，再决定是否上并行。**

---

## 5. ADR：enhance_project 写入路径优化（第一波）

```markdown
# ADR-005: enhance_project 写入路径批处理优化（A3 + 复用语句 + 同名鲁棒）

## Status
Accepted

## Context
全量增强（enhance_project）在 Linux 内核量级（~45K 调用边）下约 27s。
根因分析定位到：每条调用边各自开/关事务（engine_queries.cpp:447-457）、
所有 store 写入每次重新 prepare、跨文件 callee 走 JSON 往返、
每符号 3 次就绪位 UPDATE。瓶颈在 DB 写入方式而非算法。
实施中暴露三个争议点：事务粒度（整文件 vs 每符号）、语句缓存生命周期、
同名多符号的 callee 解析准确性。

## Decision
实施第一波（A–D），并锁定三项子决议：
- A 采用 **A3**：每文件一个外层事务（BEGIN）+ 每个符号一个 SAVEPOINT；
  符号级硬失败时 ROLLBACK TO 该符号、同文件其他符号不受影响；文件尾一次性 COMMIT。
  embedding（vec0）写入置于 SAVEPOINT 之外（待验证 vec0 savepoint 支持），
  靠 enhance 幂等重跑自愈。
- B 复用 prepared statement：store 增 `stmt_cache_`（mutex 保护 + kStmtCacheMax=16 cap），
  `getCachedStmt` 编译一次复用；析构 finalize。
- C 跨文件 callee：启动时建全局 `name→list<SymbolRef>` in-memory 多值映射（O(1)、容重名），
  候选按"同文件 > 同 namespace > 全局"排序；0→跳过、1→用、≤K→全插、>K→只插同文件/namespace。
  超大项目回退 DB 查询版 `findSymbolIdsByName`。
- D 就绪位三列合并为一条 UPDATE（`markSymbolEnhanced`）。
暂不做第二波（E 并行 / F IR 落盘），待 A–D 实测后再评估。

## Consequences
- 更易：单次全量增强耗时大幅下降（预期数量级），DB 写入压力显著降低；
  符号级容错优于整文件回滚；同名/重载不再静默连错目标。
- 更难：需显式管理 SAVEPOINT 生命周期（忘记 RELEASE 会阻止外层 COMMIT）；
  vec0 savepoint 兼容性需验证；in-memory 映射在超大项目占用数百 MB（结束释放）。
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

**`engine/src/store/store.h`**（私有段，紧挨 `stmt_vector_` 声明 `:408`）：
```cpp
// Cache of prepared statements keyed by SQL text, reused across calls.
// Guarded by stmt_cache_mutex_ because g_store may be shared across tasks.
// Bounded: only a handful of distinct SQL strings (~8) are ever prepared in
// enhance. If the cap is exceeded it signals a bug (new unregistered SQL at
// runtime), so getCachedStmt warns + refuses to cache rather than evicting.
static constexpr size_t kStmtCacheMax = 16;
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
    // The working set is a fixed, known set of SQL strings (≈8). If we ever
    // hit the cap it means someone introduced a NEW, unregistered SQL at
    // runtime (a bug), not normal growth. Fail loud: warn and refuse to cache
    // (return nullptr) so the bug surfaces in dev/test instead of being
    // silently masked by eviction. Callers already handle a nullptr stmt.
    if (stmt_cache_.size() >= kStmtCacheMax) {
        fprintf(stderr, "warn: stmt_cache exceeded %zu entries; refusing to cache new SQL (likely a bug)\n", kStmtCacheMax);
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

### 7.2 A — 每文件事务 + 每符号 SAVEPOINT（A3）

**目标**：消除 45,573 次 BEGIN/COMMIT（改为每文件 1 次 COMMIT），同时把失败粒度从"整文件回滚"收紧到"单符号"，兼顾性能与鲁棒性。

**设计（A3）**：
- 外层：`if (!ir_id_to_symbol.empty()) g_store->beginTransaction();`（放在 `:319` 之后，包住整文件写入）。
- 内层：每个符号处理前 `g_store->savepoint("sym_N")`；若本符号任一写入返回 false（约束冲突等硬失败），`g_store->rollbackToSavepoint("sym_N")` 后 `releaseSavepoint`，记日志，`continue` 到下一符号——**同文件其他符号不受影响**。
- 文件尾：`g_store->commitTransaction()`（`:642` 之前）。

**为什么 A3 而非 A1/A2**：
- vs A1（整文件一事务）：A3 在几乎零额外成本下获得符号级容错；A1 一旦某符号硬失败即整文件回滚，丢失该文件全部已完成写入。
- vs A2（每符号一事务）：A3 只有 1 次 COMMIT/文件，写锁与 WAL 开销远低于 A2 的 N 次 COMMIT；SAVEPOINT 是纯内存记账，无 fsync。A3 在性能上≈A1、在鲁棒性上≈A2，**严格占优**。

**两个必要约束（否则 A3 的鲁棒性落空或炸库）**：
1. **vec0 与 SAVEPOINT 的兼容性需一次性验证**。sqlite-vec 的 vec0 是虚拟表，是否实现 `xSavepoint`/`xRollbackTo` 未知。风险不在 vec0 是否失败（`insertEmbedding` 对 vec0 失败已容错、返回 true 不报错），而在：若某符号因别的原因要 `ROLLBACK TO sym_N`，而该 savepoint 内曾写过 vec0，vec0 不支持回滚会导致 `ROLLBACK TO` 本身报 `SQLITE_ERROR`。**默认把 embedding 写入放在符号 SAVEPOINT 之内**（与其他写入一致）；实施时跑一次验证（见下）。若验证失败，则将 embedding 写入移到 SAVEPOINT 之外（它本就 best-effort，失败只少一条向量，下次 rerun 补）。无论哪种，enhance 幂等增量 + `embedding_ready` 标志保证重跑自愈。
   - 验证方法（独立小测试）：打开加载了 vec0 的 DB → `BEGIN` → `SAVEPOINT s` → 插一条向量 → `ROLLBACK TO s` → 断言返回 `SQLITE_OK` 且向量不存在。若 `ROLLBACK TO` 报错，则 vec0 不支持 savepoint，按 fallback 处理。
2. **失败处理必须从"log and continue"改成"显式 rollback to savepoint"**。现有 `insertCallEdge/insertMetric/insertEmbedding` 是 catch-return-false 而非抛异常，SAVEPOINT 不会自动触发。因此检测到 false 时必须主动 `ROLLBACK TO sym_N` + `RELEASE`，否则该符号的半截写入会留在事务里。

**前置：GraphStore 需新增三个薄封装**（基于 SQLite C API）：
```cpp
// thin wrappers over sqlite3_savepoint / sqlite3_rollback_to_savepoint / sqlite3_release
bool savepoint(const char* name);
bool rollbackToSavepoint(const char* name);
bool releaseSavepoint(const char* name);
```

**`engine/src/engine_queries.cpp`，`engine_enhance_project`**：

1. **删除**调用边循环里的逐边事务（`447–457`）：
```cpp
// 删除：g_store->beginTransaction(); 与 g_store->commitTransaction();
// 保留：
g_store->insertCallEdge(project_id, caller_id, callee_id, "static",
                        static_cast<int>(call_line), 0);
total_edges++;
```
2. **新增**外层事务起点（`:319` 之后）：
```cpp
if (!g_store->beginTransaction()) {
    fprintf(stderr, "enhance: beginTransaction failed: %s\n", g_store->error().c_str());
    continue;
}
```
3. **符号级 SAVEPOINT**（在每符号处理循环开头）：
```cpp
std::string sp = "sym_" + std::to_string(sym_id);
if (!g_store->savepoint(sp.c_str())) { fprintf(stderr, "enhance: savepoint failed\n"); continue; }
bool sym_ok = true;
// ... 调用边 / metrics / embedding / FTS 写入，任一返回 false → sym_ok = false ...
if (!sym_ok) {
    g_store->rollbackToSavepoint(sp.c_str());
    fprintf(stderr, "enhance: symbol %llu rolled back\n", (unsigned long long)sym_id);
}
g_store->releaseSavepoint(sp.c_str());
```
4. **删除** metrics 块自身的 `beginTransaction()`（`:472`）与 `commitTransaction()`（`:639`）。
5. **新增**文件尾提交（`:642` 之前）：
```cpp
if (!g_store->commitTransaction()) {
    fprintf(stderr, "enhance: commitTransaction failed: %s\n", g_store->error().c_str());
}
total_files_processed++;
```

**正确性**：所有 `continue`（解析失败等）都在 `:319` 之后、事务之内；事务内只有符号级 `continue`（跳符号，靠 SAVEPOINT 隔离）。若符号循环用 `break` 跳出文件，需在跳出前 `commitTransaction` 或 `rollback`，避免悬挂事务。

---

### 7.3 C — 跨文件 callee 查 id（去 JSON 往返 + 同名多符号鲁棒处理）

**目标**：`engine_queries.cpp:421–445` 的 `findSymbolJson` + `json.find("\"id\":")` 替换为 O(1) 查表；并正确应对 C++ 同名（static 跨文件、重载）导致的多匹配。

**设计（性能 + 准确性双优）**：

1. **启动时建全局 `name → list<SymbolRef>` in-memory 多值映射**（一次 `SELECT id, module_id, file_path, name FROM symbols WHERE project_id=?`，复用 per-file `sym_cache` 思路）。`SymbolRef = {id, module_id, file_path}`。一次构建、全程 O(1) 查、天然容纳重名、零 DB 查询 → 性能吊打现在每次 DB 查 + 拼 JSON。
   - 内存权衡：Linux kernel 35M 符号 → 多值映射可能数百 MB，enhance 期间可接受（结束释放）。
2. **关键前提**：跨文件查找只在 `lookupSymbolId`（按 `file_path` 精准匹配）找不到时才发生。即**同文件内的调用已被 O(1) 哈希缓存解决，永远不会进入跨文件逻辑**。因此跨文件候选集里**"同文件优先"实际上永远不会命中**——排序优先级实际是 **同 module > 同 namespace（前缀匹配）> 其余**。
3. **解析调用边时**（`callee_id == 0` 即进入此逻辑），取 `global_name_map[name]` 候选，按 **同 module > 同 namespace > 其余** 排序后处理：
   - 0 候选 → 跳过（同现状）。
   - 恰好 1 → 用。
   - 2 … K（K=16）→ **全部插入 edge**（recall 安全：绝不漏掉真实调用目标；代价是少量假边，对代码理解工具可接受）。
   - > K（如 `init/get/set/free` 这类 ubiquitous 名可能有数百同名）→ `fprintf(stderr, warn)` **只插前 K 条**（按 module>ns>rest 排序后的 top-K）。避免边数爆炸（45K→45M）与图污染。

**为什么 ≥2→all 但要 cap**：代码理解工具的核心价值是**不漏掉真实调用目标**（recall > precision）。对常见短名若无脑插全部，边数会随同名数线性膨胀并污染图；cap + module/namespace 优先在"召回真实目标"与"控制噪音/性能"间取得平衡。

**DB 查询回退版（超大项目内存敏感时）**——`findSymbolIdsByName` 用 **SQL `LIMIT 16` + `ORDER BY (module_id = ?) DESC`** 一次性取回"同 module 优先、最多 16 条"的候选，无需在 C++ 里二次 cap（SQL 侧已封顶）。调用方语义与 in-memory 版一致。

**`engine/src/store/store.h`**（公共方法段，紧跟 `findSymbolJson` `:212`）：
```cpp
// Cross-file callee fallback (huge-project / memory-sensitive mode).
// Returns up to `limit` matching symbol ids, ordered same-module-first.
// Used only when the in-memory global_name_map is disabled.
std::vector<std::pair<uint64_t, uint64_t>>
findSymbolIdsByName(uint64_t project_id, const char* name, uint64_t caller_module_id, int limit = 16);
```

**`engine/src/engine_queries.cpp`**：在 `engine_enhance_project` 开头构建全局映射（简化伪代码），并把 `421–445` 整段替换为：
```cpp
// Build once at enhance start (or skip + use findSymbolIdsByName in memory mode):
//   SELECT id, module_id, file_path, name FROM symbols WHERE project_id = ?
//   → unordered_map<string, vector<SymbolRef>> global_name_map;
if (callee_id == 0) {
    callee_id = resolveCallee(global_name_map, project_id, name,
                              caller_module_id, /*cap=*/16);
}
```
`resolveCallee` 实现上面的"0/1/≤K/>K"四条规则，排序优先级 **module > namespace > 其余**。

**清理（步骤 4）**：删除之前暂存的未完成 enhance 代码；保留 `clearStmtCache()`（析构/显式清理有用）；`findSymbolIdsByName` 保留作回退路径（global_name_map 覆盖主路径，但内存敏感模式仍需它）。

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
| `engine/src/store/store.h` | 私有段 `:406`、公共段 `:212/:248` | 增 `stmt_cache_`/`stmt_cache_mutex_`/`getCachedStmt`（含 `kStmtCacheMax` cap）；增 `findSymbolIdsByName`、`markSymbolEnhanced` 声明；增 `savepoint`/`rollbackToSavepoint`/`releaseSavepoint` 声明 |
| `engine/src/store/store.cpp` | 析构、`getCachedStmt`、5 个写入方法、`findSymbolIdByName`、`markSymbolEnhanced` | 实现缓存 + 两个新方法；写入方法改用 `getCachedStmt` 并去 `finalize` |
| `engine/src/engine_queries.cpp` | `engine_enhance_project` `:319/:447/:472/:611/:639/:649` | 单文件事务；跨文件查 id；就绪位合并 |

### 7.6 自我校验（落地后必做）

1. **编译**：`make build-engine`，确认无未定义符号（`getCachedStmt` 等）。
2. **正确性**：同项目跑 `enhance_project`，对比 `engine_get_enhancement_status` 的 `call_edges` 总数与改前一致（数据不丢）。
3. **性能**：对比 Linux 内核样例或自有大库的耗时（预期数量级下降）。
4. **降级**：确认 `vec0` 不可用时不崩（已有容错），语义检索仍可走 `node_vectors`。

---

## 8. 实测数据下的瓶颈重定位（规模视角）

用户提供的实测量级（索引时间 = scan/index 阶段，enhance 时间 = 全量增强阶段）：

| 规模 | 典型项目 | 文件 | 预估节点 | 索引时间 | enhance 时间 |
|------|----------|------|----------|----------|--------------|
| 小 | 自写库 | ~200 | ~100K | ~2s | ~3s |
| 中 | Redis / SQLite | ~1K | ~500K | ~10s | ~30s |
| 大 | Chromium / LLVM | ~30K | ~15M | ~5-10min | ~30-60min |
| 超大 | Linux kernel | ~70K | ~35M | ~15-30min | ~1-2h |

用户定位的三处瓶颈（已用代码核实，见 §9–§11）：

1. **buildGraph** — `INSERT INTO graph_edges` 的 SQLite 写入。OmniScope 262 文件 770ms，1K ~5-6s，30K ~3-5min。
2. **enhance** — 每文件 tree-sitter 完整 parse + translator，纯 CPU。262 文件 3.8s → ~15ms/file，30K ~7-8min。
3. **Worker 内存** — 每 worker 持一个文件的 `semantic_units`；worker 数 = `hardware_concurrency()`（14 核机器即 14 worker）。大文件（如 sqlite3.c amalgamation 数十万行）吃数百 MB。

### 关键重定位（推翻"只优化 enhance DB 写入就够"的假设）

我之前的 A–D（§7）只解决 **enhance 的 DB 写入**部分。从实测数据看：

- **小/中项目**：A–D + buildGraph 索引优化收益显著（瓶颈确在 DB 写入）。
- **大/超大项目**：墙钟主体变成 **(a) enhance 单线程 parse（CPU，A–D 碰不到）** 与 **(b) buildGraph 边索引维护**。A–D 必要但**不充分**——必须并行化 enhance（E）。
- 用户判断成立：**真正要命的是单大文件（内存爆炸），不是文件数量**。一个接近阈值的密集大文件就能撑爆一个 worker；而 100MB 文件被 `max_file_size` 跳过虽不炸，却留下覆盖缺口。

---

## 9. buildGraph：边写入的索引维护（§8-瓶颈1）

**代码事实（已读 `engine/src/store/store.cpp`）：**

- `buildGraph` 定义在 `:2614`。
- containment edges 走 bulk `INSERT INTO graph_edges SELECT ...`（`:2739-2753`）——单语句、单隐式事务。
- call edges **已**改为 C++ 哈希表 O(1) 解析（`callee_by_name` / `callee_by_short` / `caller_idx`，`:2779-2819`）+ 500/批批量 INSERT（`:2821-2929`，`kBatchSize=500`）。
- 因此 buildGraph **早已批量**，残留 stall 是 `graph_edges` 上的 **5 个二级索引**（`engine/src/store/store_core.cpp:222-229`：`idx_graph_edges_src` / `_tgt` / `_project` / `idx_ge_callers` / `idx_ge_callees`）在每次 INSERT 时同步维护。30K 文件→数百万边，每边 ×5 索引更新即量级成本。
- 另有 Step 1 把**全项目**声明载入 C++ 哈希表（`:2779-2819`）：35M 节点时这部分本身也是数 GB 内存风险。

**优化方案：**

| ID | 改动 | 收益 | 风险 / 代价 |
|----|------|------|-------------|
| **A** | 在 2d/2e 批量插入前 `DROP INDEX` 这 5 个索引，插完 `CREATE INDEX` 重建（SQLite bulk-load 标准大招） | 千万级边通常数倍加速（消除插入期索引维护） | 插期间图查询（impact_analysis 等）变慢/失效；buildGraph 是离线构建阶段，可接受 |
| **B** | 评估 5 个索引是否冗余：`idx_ge_callers`/`idx_ge_callees`（edge_type+node）与 src/tgt 是否可合并为复合索引 | 减少维护的索引数 | 需核对所有查询路径（graph_query / impact_analysis / community_detection） |
| **C** | Step 1 全量哈希表按 `changed_files` 范围加载；全量重建时考虑分块加载 | 避免一次性数 GB 驻留 | 增量时本就只载变更文件，全量路径需改 |

---

## 10. enhance：解析 CPU 与单线程（§8-瓶颈2）

**代码事实（已读 `engine/src/engine_queries.cpp`）：**

- `engine_enhance_project` 在 `:194`，**单线程**顺序 `for` 循环（`:260`）。
- 每文件：tree-sitter 完整 parse（`:269`）+ translator 翻译（`:281`）+ `ComplexityAnalyzer`（`:471`）。
- 实测 262 文件 3.8s → ~15ms/file（纯 parse+translate CPU）；与文件数线性，30K → ~7-8min 纯 CPU。

**优化方案：**

| ID | 改动 | 收益 | 风险 / 代价 |
|----|------|------|-------------|
| **E** | enhance 复用 `engine_index.cpp:643` 的 worker 池模式做文件级并行；每 worker **独立 SQLite 连接**（serialized 模式允许并发连接，写自动串行化）；每文件一个事务（即 §7.2 的单文件事务恰好适配并行——各 worker 在自己连接上开/关事务，互不干扰） | 墙钟随核数近线性下降（7-8min→~1min @8 核） | 需处理并发安全、进度上报（get_enhancement_status 聚合）、部分失败恢复 |
| **F** | IR 复用：Phase A scan（engine_index）已 parse 每个文件并产出 SemanticUnit；把 IR/关键节点（函数范围、调用点）落盘，enhance 跳过 re-parse | parse CPU 趋近零，仅剩 DB 写入 | 改动大：存储格式、版本演进、磁盘体积 |

> 注意：§7 的 A–D（DB 写入优化）与 E（并行）是**互补**的——A–D 压低单文件的 DB 成本，E 压低墙钟的 CPU 维度。大/超大项目两者都得上。

---

## 11. 大文件 / Worker 内存（§8-瓶颈3）—— 真正的风险点

**代码事实（已读 `engine/src/engine_index.cpp`）：**

- Phase 2 每 worker `readFile` 把整文件读入 `source`（`:570`），tree-sitter parse 出 `SemanticUnit`，存入 `semantic_units[local_idx]`（`:529,617`）。
- **整个 batch 的 `semantic_units` 一直驻留内存，直到 Phase 3（`:717`）统一 `beginTransaction` 后持久化**。峰值内存 = 一个 batch 内所有 unit 之和。
- 文件跳过：`max_file_size` 默认 5MB（`:335,563-568`），>`max_file_size` 直接 `continue` 不 parse → 100MB 文件**不会被 parse（不炸，但也不被索引，是覆盖缺口）**。
- 内存预算（`:672-708`）：RSS 超预算只 `sleep` 节流，**不真正释放/限制内存**。

**用户判断成立：真正的问题是单大文件，不是文件数。** 一个接近上限的密集大文件（如 4MB 但百万 AST 节点）仍让单 worker 的 `SemanticUnit` 爆内存；多文件中文件还会在 batch 内叠加。

**优化方案：**

| ID | 改动 | 收益 | 风险 / 代价 |
|----|------|------|-------------|
| **A** | 流式/分块持久化：parse 完一个文件立即把它的 records 写入并释放该 `SemanticUnit`，而非整 batch 攒着 | 峰值内存 = O(单文件 AST 深度)，与 batch 大小/文件总数解耦（治本） | 需重构 Phase 2→3 的数据流（当前是"全攒完再统一插"） |
| **B** | 大文件分窗：超过阈值时整文件 parse 后**按节点分批遍历**并边遍历边落库，峰值内存仅与 AST 深度成正比 | 支持任意大文件不爆内存 | tree-sitter 不直接支持子区间 parse，需遍历层控制 |
| **C** | `max_file_size` 从「跳过」改为「超过阈值走分窗流式」 | 兼顾覆盖与内存，消除 100MB 文件的零覆盖缺口 | 需要 B 的分窗能力先就位 |
| **D** | 把 RSS sleep 节流升级为「超预算时立即把已完成 batch 落库并清空 `semantic_units`」 | 真正限制驻留内存，而非仅减速 | 实现简单，收益直接 |

---

## 12. 缩放视角下的优先级重排

| 规模 | 主导瓶颈 | 最有效改动（按优先级） |
|------|----------|------------------------|
| 小 (~200) | enhance DB 写入（边/指标/FTS） | A–D（单文件事务 + 复用语句） |
| 中 (~1K) | enhance DB 写入 + buildGraph 边索引 | A–D + §9-A（删/建索引） |
| 大 (~30K) | enhance 单线程 parse + buildGraph 边索引 | **E（并行 enhance）** + §9-A + A–D |
| 超大 (~70K) | 解析 CPU + 边索引 + 大文件内存 | **E + F（IR 复用）** + §9-A + §11（大文件流式） |

**结论：**
- A–D 是底座，小/中项目必做。
- **大/超大项目必须上 E（并行 enhance）**——这是把 30-60min 压下来的主力。
- **大文件内存要当稳定性一等公民**：从"跳过"升级为"流式"（§11-C），否则超大项目随时 OOM。

---

## 13. 三个子问题的决议（A3 / stmt_cache 护栏 / C 同名多符号）

围绕"性能 + 准确性 + 高容错 + 强鲁棒"的目标，对 A–D 实施中暴露的三个争议点做如下决议（均已写入对应 §7 实施计划）：

### 13.1 事务粒度 → 采用 A3（每文件事务 + 每符号 SAVEPOINT）
- **否决 A0**（逐边事务，45K 提交，性能杀手）与 **A1**（整文件一事务，老鼠屎坏一锅粥）。
- **A3 严格占优 A2**（每符号事务）：性能≈A1（1 COMMIT/文件），鲁棒性≈A2（符号级隔离），SAVEPOINT 无 fsync 开销。
- **两个硬约束**：① 验证 vec0 是否支持 savepoint 回滚，否则 embedding 写置于 savepoint 外、靠重跑自愈；② 失败处理从"log and continue"改为"显式 ROLLBACK TO + RELEASE"，否则 SAVEPOINT 不触发。

### 13.2 stmt_cache_ 直到 close() 才 finalize → 不担心泄漏，加护栏即可
- **泄漏**：固定 ~8 条 SQL、进程退出 OS 回收，`sqlite3_reset` 在错误态也安全 → 无风险（用户的三个担心里这两项都不是问题）。
- **护栏改为 warn + 拒绝（不 evict）**：`kStmtCacheMax=16` 是已知固定 SQL 集合，正常运行永远碰不到。若真超过，说明有人在运行时动态拼出新 SQL（bug），**静默 evict 会掩盖 bug**；故 `getCachedStmt` 在超限时 `fprintf(stderr, warn)` 并返回 `nullptr`（走调用方既有的失败路径），让 bug 在 dev/test  loudly 暴露。并发仍由 `mutex` 保护（为未来并行 E 铺路）。

### 13.3 同名多符号（static 跨文件 / 重载）→ 内存多值映射 + module>ns>其余 + fan-out cap
- **方向采纳用户的 ≥2→all**（recall > precision，对代码理解工具更值），但**必须加 cap K=16**：`init/get/set/free` 等 ubiquitous 名可能有数百同名，无 cap 会边数爆炸（45K→45M）并污染图。
- **实现**：启动时 `SELECT id, module_id, file_path, name FROM symbols WHERE project_id=?` 建 `name → list<SymbolRef{id,module_id,file_path}>` in-memory 多值映射（O(1)、零 DB 查询、天然容重名）。
- **排序优先级修正**：跨文件查找只在 `lookupSymbolId`（按 `file_path` 精准匹配）找不到时才发生，故**同文件候选永远不会进入此逻辑**，"同文件优先"实际不命中。真实优先级是 **同 module > 同 namespace（前缀） > 其余**。
- **cap 规则**：0→跳过、1→用、2…K→全插（recall 安全）、>K→`fprintf warn` 只插前 K 条（module>ns>其余排序后 top-K）。
- **回退版**：超大项目内存敏感时禁用 in-memory 映射，改用 `findSymbolIdsByName`，其用 **SQL `LIMIT 16` + `ORDER BY (module_id = ?) DESC`** 在数据库侧一次性封顶并同 module 优先，无需 C++ 二次 cap。
- 内存权衡：35M 符号映射数百 MB，enhance 期间可接受、结束释放。

---

## 14. ADR-006：规模感知的增强/索引性能优化

```markdown
# ADR-006: Scale-Aware Enhancement & Indexing Performance

## Status
Proposed

## Context
实测显示：小/中项目瓶颈在 enhance 的 DB 写入（已由 ADR-005 A–D 覆盖）；
但大/超大项目（30K–70K 文件、15M–35M 节点）墙钟主体变为
(a) enhance 单线程 parse（CPU，A–D 碰不到）、
(b) buildGraph 边写入时维护 graph_edges 的 5 个二级索引、
(c) 单大文件撑爆 worker 内存（当前 max_file_size 跳过→覆盖缺口）。
用户强调：真正的问题是单大文件，不是文件数量。

## Decision
在 ADR-005（A–D）之上追加：
- buildGraph：批量插边前 DROP 5 个二级索引、插后重建（§9-A）；评估索引合并（§9-B）。
- enhance：文件级并行 + 每 worker 独立 SQLite 连接（§10-E）；长期评估 IR 复用（§10-F）。
- 大文件：流式/分窗持久化，将 max_file_size 的「跳过」改为「流式」（§11-A/B/C）；
  内存预算从 sleep 节流升级为超限即落库清空（§11-D）。
暂不做第二波侵入式改动（F 的 IR 落盘格式）直至 E + 索引 + 流式验证收益。

## Consequences
- 更易：大/超大项目 enhance 从 30-60min / 1-2h 下降到近线性随核数收敛；
  消除单大文件 OOM 与覆盖缺口。
- 更难：引入并行 enhance 的并发安全/进度聚合/部分失败恢复；
  流式持久化需重构 Phase 2→3 数据流。
- 不变：enhance 对外行为、增量语义、MCP 接口保持兼容。
```


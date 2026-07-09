# Code Review — `enhance_project` 优化实现

**Reviewer**: 火眼眼 👁️
**Date**: 2026-07-08
**Scope**: `engine/src/engine_queries.cpp` (`engine_enhance_project`)、`engine/src/store/store.cpp`、`store.h`、`store_core.cpp`
**方法**: 逐行核对当前代码 + 对照 `ENHANCE_OPTIMIZATION.md` 既定方案

---

## 总体印象

方向**对了**,而且踩中了对话最后那个关键结论——**Phase B 不再自己解析跨文件 callee,改为复用 Phase A 已经建好的图**。A3 单文件事务 + `stmt_cache_` 直接干掉了最致命的 B1/B2 瓶颈,`markSymbolEnhanced` 合并三列也漂亮。

但有几处必须摊开说(最后一轮复核已更新):

1. **🔴1 已闭环**:跨文件复制已改用 `node_id` 直接 JOIN(`engine_queries.cpp:690-692`),`symbols.node_id` 由 `buildGraph` 末尾回填(`store.cpp:3173-3189`)保证有值——不再是 `(name, file_path)` 重解。
2. **🔴2 反而更实锤**:enhance 现在同时依赖 buildGraph ①建 `graph_edges` ②回填 `node_id`,两者任一未完整跑 → `node_id` 保持 NULL → JOIN 命中 0 行 → **跨文件图静默归零、无报错**(比报错更糟)。
3. **🔴3 跨文件准确性没动**:buildGraph 跨文件仍按名字+50-cap,黄金 `ir_semantic_edges` 仍零 SELECT 消费。
4. **🟡4 A3 的"符号级容错"实际没接上**——写入返回值全被忽略,savepoint 永不回滚,错误被静默吞掉(违反 `plan/rules/code_rules.md` 的"不静默吞错")。
5. **`ENHANCE_OPTIMIZATION.md` 已和代码脱节**,仍在描述被否决的 `global_name_map` 方案,顶部状态行还写"A–D 全部未实施"——假的。

下面按优先级展开。

---

## 🔴 Blockers(准确性 / 架构)

### ✅ 1. (已闭环)跨文件复制改用 `node_id` 直接 JOIN

初版评审曾把"跨文件复制用 `(name, file_path)` 重解 symbol"列为 🔴1。复核当前代码发现**已修复**:

- `symbols` 表已加 `node_id` 列(`store_core.cpp:324`);
- `buildGraph` 末尾(`store.cpp:3173-3189`)回填:
  ```sql
  UPDATE symbols SET node_id = (
    SELECT gn.id FROM graph_nodes gn
    WHERE gn.project_id = symbols.project_id
      AND gn.name = symbols.name
      AND gn.file_path = symbols.file_path
      AND gn.start_row = symbols.line
      AND gn.start_col = symbols.column
      AND gn.node_type IN (0,1,2,3,4,6)
    LIMIT 1
  ) WHERE project_id=? AND node_id IS NULL
  ```
- 复制 SQL 已改为 `JOIN symbols s1 ON s1.node_id = ge.source_node_id`(`engine_queries.cpp:690-692`)。

**结论:node↔symbol 直接对应已固化,✅ 不再受同名干扰。**

**遗留两点(属 🔴2 范畴):**
- 回填是**相关子查询**,大项目(symbols×graph_nodes 量级)有性能代价,建议改 `JOIN`/临时索引;
- `WHERE node_id IS NULL` 在重索引后旧 `node_id` 不更新 → 陈旧,建议 re-index 先清空 `node_id` 再回填。

---

### 🔴 2. Phase B 现在隐式依赖 `buildGraph` 先跑——否则跨文件图静默归零

原代码的 Phase B 会**自己**做跨文件 callee 解析(`findSymbolJson` + 名字查),所以不依赖 `graph_edges`。现在跨文件边完全来自对 `graph_edges` 的复制(:684)。

- 正常流水线 `index → enhance`:`buildGraph` 在 `engine_index.cpp:823` 已填充 `graph_edges`,复制能拿到边 ✅。
- **但**若直接调 `enhance` 而 `graph_edges` 为空(没先 buildGraph / 旧库 / 单独触发),复制 0 行 → 跨文件调用图**整片缺失**,且无任何报错。

**Suggestion:** 在复制前加一个守卫/告警:若 `graph_edges` 中本项目 `edge_type=1` 为 0,`fprintf(stderr, warn)` 提示"cross-file edges missing: buildGraph may not have run",或回退到自建跨文件边。至少让失败可见,别静默。

---

### 🔴 3. 跨文件解析依旧是"按名字",黄金数据 `ir_semantic_edges` 仍无人消费

`buildGraph` 的 2e 段(`store.cpp:2892-3032`)跨文件解析仍是:

- Priority 1:同文件用 `ref_original_id`(`:2996`)✅ 精确;
- Priority 2:跨文件用 `callee_by_name` / `callee_by_short` 哈希表按**名字**捞(`:3012-3032`),且短名 `size() > 50` 直接**丢弃**(`:3027`)。

全仓 grep 确认 `ir_semantic_edges`(translator 解析好的、带真实跨文件 `target_node_id` 的黄金数据)**只有建表/插入/删除,零 SELECT 消费**(`store.cpp:75` 写、`:97` 删,`store_core.cpp:171` 建表)。

**Why:** 这意味着"Phase B 读 Phase A 的图"只是把**名字解析**从 Phase B 挪到了 Phase A,**跨文件准确性没有任何提升**——`get`/`set`/`new` 这类 ubiquitous 短名在 buildGraph 阶段就被 50-cap 砍掉了,Phase B 复制也只会拿到被砍过的边。`ir_semantic_edges` 这批真正精确的数据还在库里睡大觉。

**Suggestion:** 这是下一步要做的真功夫——让 `buildGraph` 在 Priority 2 之前先消费 `ir_semantic_edges`(`edge.target_node_id` 已是解析好的 node id,直接 JOIN `graph_nodes` 落 `graph_edges`)。做掉这步,跨文件准确性才会本质改善,而不只是"少写了一遍解析"。

---

## 🟡 Suggestions(应该修)

### 🟡 4. A3 的 savepoint 是"空转"的——错误被静默吞掉

`engine_queries.cpp:497-666` 建了 per-symbol savepoint,但:

- `insertMetric` / `insertEmbedding` / `insertIntoSearchIndex` / `markSymbolEnhanced` 的返回值**全都没检查**(grep 确认无 `if (!g_store->insert...)`);
- `else { sym_ok = false; }`(:651)对 `ir_id_to_symbol` 里的符号**永远走不到**(那 6 种 kind 已被 `if/else if` 全覆盖),所以 `sym_ok` 实际恒为 true。

结果:savepoint 永远走 `releaseSavepoint`(:657),**从不 `rollbackToSavepoint`**。某条写入失败 → 错误被忽略 → 该 symbol 仍被 `markSymbolEnhanced` 标成 ready(:618/:650)→ **失败被静默提交 + 标记完成**,违反 code_rules 的"不静默吞错"。

**Suggestion(二选一,明确取舍):**
- **(a) 接上**:检查关键写入返回值,失败置 `sym_ok=false` 触发回滚。但要注意 `insertEmbedding` 是 best-effort(vec0 不可用时应允许失败而不回滚整符号)——所以只让"硬失败"(metric/FTS/flag)驱动 `sym_ok`,embedding 失败不回滚。
- **(b) 删掉**:如果写入就是 best-effort,那 per-symbol savepoint 是纯粹的复杂度+每符号两次额外 exec(对 35M 符号是 70M 次无意义 exec),不如只保留单文件事务,清爽。

当前"建了却从不触发"最糟——既没拿到容错,又背着开销,还制造"已有容错"的错觉。

> 附带结构不一致:调用边(`:466`)写在 per-symbol savepoint **之外**(savepoint 在 :503 的指标循环里才开始),所以"符号级隔离"其实只罩住了 metric/embedding/fts,罩不住 call edge。要做就做全,否则部分隔离意义不大。

---

### 🟡 5. `getCachedStmt` 的 cap 兜底会泄漏语句,且与既定方案相反

`store.cpp:391-402`:cap 触发时打印 BUG 后 **`prepare` 一个一次性语句并返回**,但调用方约定"语句归缓存所有、不 finalize"。这个一次性语句**永远没人 finalize → 泄漏**。

既定方案(`ENHANCE_OPTIMIZATION.md §13.2`)是 **"warn + 拒绝,返回 nullptr"**,走调用方既有失败路径。实现改成了"warn + 偷偷 prepare 继续",既泄漏又掩盖了 bug。

**Suggestion:** 改回返回 `nullptr`(与文档一致):
```cpp
if (stmt_cache_.size() >= kStmtCacheMax) {
    fprintf(stderr, "BUG: stmt_cache exceeded %zu ...\n", kStmtCacheMax, sql);
    return nullptr;   // 走调用方失败路径,别泄漏
}
```

---

### 🟡 6. `close()` 没有 finalize `stmt_cache_` → 语句泄漏 / 重建后悬空

`~GraphStore()`(`store_core.cpp:26`)→ `close()`,但 `close()` 里**没有调用 `clearStmtCache()`**(:414 定义了却零调用)。缓存的 ~8 条 prepared statement 在关闭/重建时不会被 `sqlite3_finalize`。

- 进程级单例:退出时 OS 回收,影响小;
- 但若 `g_store` 被 close 后**重新 open**,旧缓存语句指向已关闭的 `db_` 句柄 → 下次复用即 `SQLITE_MISUSE` 悬空。

**Suggestion:** 在 `close()` 开头调用 `clearStmtCache()`(务必在 `sqlite3_close(db_)` **之前**)。

---

### 🟡 7. `findSymbolIdsByName` 是死代码

`store.cpp:1264` 定义了、`store.h:230` 声明了,但**全仓零调用**(原 §7.3 的 in-memory 方案被否决后没接上)。

**Suggestion:** 要么删掉(遵循"不留死代码"),要么作为 buildGraph/Phase B 的显式回退路径接上。现在它挂着只会增加阅读负担。

---

### 🟡 8. `call_edges` 无唯一约束 → rerun 幂等性存疑

`store_core.cpp:371-381`:`call_edges` 只有 `id` 自增主键 + 两个单列索引,**无** `(caller, callee, provenance, line, col)` 唯一约束。

- 跨文件复制用 `INSERT OR IGNORE`,但没有唯一键时 `OR IGNORE` 永不触发 → 重置 ready 标志后重跑会**重复插入全部跨文件边**;
- 文档 §1 声称"enhance 幂等、重跑自愈",当前实现在该路径下不成立。

**Suggestion:** 复制加 `WHERE NOT EXISTS (...)` 守卫,或对 `(caller_symbol_id, callee_symbol_id, provenance, line, col)` 建唯一索引。

---

### 🟡 9. `markSymbolEnhanced` 在 embedding 失败时仍置 `embedding_ready=1`

`:618` / `:650` 无条件调用 `markSymbolEnhanced`,即便前面 `insertEmbedding` 已失败(vec0 不可用)。结果:语义检索**静默残缺**,且因 `embedding_ready=1` 不会被重跑补齐。

**Suggestion:** embedding 失败时应**不**置 `embedding_ready`(只置 callgraph/metrics),让增量重跑能自愈——这也和 §7.2 里"embedding 失败靠重跑自愈"的承诺一致。

---

## 💭 Nits

- **`static const char *skip[]`(:409) 含 `else`/`case`/`break`/`continue`**:这些一般不接 `(`,不会误匹配;无害,但列表含义略含糊,可加注释说明"控制流关键字后不跟函数调用"。
- **跨文件复制 `total_edges += sqlite3_changes`(:705)**:复制的边数靠 `sqlite3_changes` 累计,与 `SELECT COUNT(*) FROM call_edges` 在 rerun 下可能不一致,仅影响上报数字,不影响数据。
- **复制每次 enhance 都扫全项目 `graph_edges`(:684)**:即便只增量增强了 1 个文件,也重跑整库跨文件复制(靠 `OR IGNORE` 去重)。增量场景下偏浪费,可接受。

---

## ✅ 做得好的地方

- **单文件事务(`:353` / `:671`)**:直接消灭 B1(45K 次 BEGIN/COMMIT → 每文件 1 次),这是墙钟最大的单项收益,干得漂亮。
- **`stmt_cache_`(`:377`)**:复用 prepared statement 解决 B2,且带 `mutex` 为未来并行 enhance 铺路,设计有前瞻性。
- **`markSymbolEnhanced` 合并三列 UPDATE**:解决 B4,少 2 次写/符号。
- **`line_starts` 二分查找(`:387-435`)**:把每行 call-site 定位从 O(N) 降到 O(log N),细节到位。
- **`provenance='graph'` 标注复制边**:血缘清晰,后续排查/统计能区分来源。
- **顶层 symbols 空时从 `graph_nodes` 快速填充(`:218-267`)**:合理的健壮性兜底。

---

## 下一步建议(按优先级)

1. **先修 🔴1 / 🔴2**:把 node↔symbol 对应固化 + 复制加守卫,否则跨文件图在重载/命名空间场景下会错连、且依赖隐式前置。
2. **修 🔴3(消费 `ir_semantic_edges`)**:这是跨文件准确性真正的突破口,值得单独排期。
3. **处理 🟡4**:savepoint 要么接上失败检测,要么删掉——别留空转的复杂度。
4. **收尾 🟡5/6/7/8/9**:删死代码、补 `close()` finalize、修 cap 兜底、加幂等守卫、embedding 失败不置 ready。
5. **同步文档**:`ENHANCE_OPTIMIZATION.md` 顶部状态行改成"已实施 + 待办 🔴3",并**删除/撤回 §7.3、§13.3 的 `global_name_map` 方案**(已否决),补一节记录"Phase B 复制 graph_edges"的真实实现与已知限制。文档和代码不同步,比代码 bug 更坑下一个接手的人。

---

### 一句话总结

事务/batch/缓存这层"性能底座"做对了,但**跨文件准确性这一层没动**——名字解析 + 50-cap 还在 buildGraph,黄金 `ir_semantic_edges` 还没被用;而新增的复制路径又用 `(name, file_path)` 引入了同名错连风险。先把 🔴1/🔴2/🔴3 这三块补齐,这次重构才算真正闭环。

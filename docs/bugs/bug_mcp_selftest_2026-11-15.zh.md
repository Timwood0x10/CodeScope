# MCP 全工具实测记录：四条差异（全部处理完毕）

## Meta

| 项目 | 内容 |
|---|---|
| 发现时间 | 2026-11-15 |
| 状态 | **第 1、3、4 项已修复；第 2 项核对后确认为非缺陷（同日）** |
| 测试方式 | 手动 stdio 会话：`printf` 逐条发送 JSON-RPC，每行响应打时间戳；未使用脚本文件 |
| 被测版本 | `bin/codescope` 0.2.7（2026-11-15 构建） |
| 测试项目 | 本仓库（1122 文件 / 14842 节点）、`~/go/src/goagent`（1908 文件 / 53792 节点）、`~/code/rustcode/memscope-rs`（297 文件 / 10049 节点） |
| 覆盖 | 48 工具 × 3 项目 = 144 次调用，另加 6 次 `analyze_project` 视图 |

结论先行：48 个工具全部可调用，仅 1 次 `isError`（第 3 条的参数不适配，已修复为可操作的错误体）。以下四条都不属于协议错误。

---

## 1. `verify_summary` 把 `implements`/`supports` 的短语当符号名 —— **已修复（2026-11-15）**

**现象（修复前）**：三个项目均返回 `Contradicted 0.5`，而输入是普通描述句。

```
verify_summary("The project implements a memory tracker for Rust.")
→ claims_parsed=1, verdict=Contradicted, confidence=0.5,
  detail: "Capability 'AMemoryTrackerForRust' is neither declared ... nor implemented"
```

**根因**：只有 `has/contains` 的捕获被当作散文。`implements`/`supports` 的捕获被 `toPascalCase` 拼成 subject，`CapabilityVerifier` 用名字查不到就判否。

**修复**：parser 在三个分支（`supports` / `implements` / `has`）设置 `Claim::captured_prose`；`CapabilityVerifier` 只读这个标记，把“未声明且名字匹配不到”的散文降为 `Unknown 0`。结构化入口（`verify_claim` / `verify_integrity`）不设置标记，因此：

- `verify_claim(type=capability_exists, subject=不存在的名字)` 仍是 `Contradicted 0.5`；
- 已声明的能力继续走实现证据判定（无调用且未导出 → `Contradicted 0.7`，有调用 → `Supported`）。

**为什么不用谓词推断**：`verify_claim` 的默认谓词同样是 `implemented_by`（`engine_verify_ffi.cpp:703`），按谓词名判断会把标识符 claim 一起降级。

**验证**：`test_mcp_selftest_regressions` 的 `CheckProseVerdicts` 覆盖四个方向——散文 → `Unknown 0`（含 `summary.status=no_verdicts` 与 `unknowns[0].reason=no_verdicts`）、已声明且有调用 → `Supported`、结构化省略 predicate → `Contradicted 0.5`、`verify_integrity` 的孤儿能力 → `Contradicted 0.7`。

---

## 2. Rust 的同名方法 —— **核对后确认不是缺陷（2026-11-15）**

**原判断**：以为 `snapshot_json` 是“trait 声明 + impl 实现”，因此应由 callable facts 配对。

**核对结果**：查 entity 表与源码后，两行是两个不同的 callable：

```
7649  snapshot_json                lifecycle.rs:223-246   free function
10069 MemScopeGuard::snapshot_json guard.rs:86-88         method (wrapper)
```

`guard.rs:87` 的实现体是 `lifecycle::snapshot_json()`，即方法包装自由函数。两者 `qualified_name` 不同，`resolveCallableGroups` 按规则让它们各自成组——`ambiguous:true` 是**正确**答案，合并反而会掩盖“同名但不同作用域”的事实。

**真正的 trait/impl 情况**同样不应合并：`Analyzer::analyze`（trait 契约）与 `GlobalTracker::analyze`（具体实现）在语言语义上是两个 item，`qualified_name` 也不同；合并会让 `find_callers("analyze")` 把 10 个实现混成一个。

**结论**：不新增 Rust/Go 的 `CallableFacts`，避免为不存在的问题写代码（`code_rules` §3）。C/C++ 仍是唯一“声明与定义共享同一身份”的语言，这一点已写进 CHANGELOG。

### 2.1 实测复核（2026-11-15，三个已索引项目）

只读 SQL，判定“同一 callable 两行”的两个可能来源：重复索引、以及同 `(name, qualified_name)` 的多行。

| 检查 | memscope-rs | goagent | CodeScope |
|---|---|---|---|
| 同文件同 `(name, qname, start_row)` 重复行（索引重复） | 0 | 0 | 0 |
| 非 C/C++ 的同 `(language, name, qname)` 多行组 | rust 195 | go 369 | python 146、rust 50、js 11、ts 8、go 4、java 1 |
| 非 C/C++ 有 `callable_signature` 的行 | 0 / 5676 | 0 / 16539 | 0 / 4166 |
| C/C++ 有签名的行 | — | — | cpp 3526/3602、c 2209/2215 |

抽样显示多行组全部是**不同文件/包的同名方法**，不是同一 callable：

```
memscope: get_data_size  tests/accuracy_accounting_cut.rs:25  |  tests/accuracy_boundaries.rs:31
goagent:  Count (3)      ares_callbacks/callbacks.go:141  |  knowledge/skills/registry.go:161  |  tools/resources/core/registry.go:112
goagent:  Close (4)      ares_bootstrap/...:133  |  ares_events/...:172  |  sdk/builder.go:170  |  sdk/knowledge.go:174
```

当前行为正确：`find_callers("Count")`、`find_callers("Close")` 均返回 `ambiguous:true`，各自成组，没有被静默合并。

### 2.2 将来补 facts 的前置条件（重要）

风险方向与最初设想相反：**直接给 Rust/Go 加签名会造成误合并**。

- 这些语言的 visitor 产出的 `qualified_name` 不含接收者/所属类型/包路径（`Count` 在三个包里都叫 `Count`）；
- `kSameCallable` 的三元组是 `(language family, qualified_name, signature)`，一旦签名存在且相同（Go 的 `func (r *Registry) Count() int` 与 `func (s *Store) Count() int` 签名都是 `()`），不同包/不同 impl 块的方法会被判成同一 callable。

因此“重新评估”的前置条件是：**先让 `qualified_name` 带上接收者/类型/包（Rust `Type::method`，Go `pkg.Type.method`），再考虑签名**。只加签名的改动应视为回归风险，而不是改进。

复现查询：

```sql
-- 索引重复（应为 0）
select count(*) from (select id from entity where kind in (0,1)
  group by name, qualified_name, file_path, start_row having count(*)>1);
-- 同 (name, qname) 多行组
select language, count(*) from (select language, name,
  coalesce(nullif(qualified_name,''),name) qn, count(*) c
  from entity where kind in (0,1) and language not in ('c','cpp','c++')
  group by language, name, qn having c > 1) group by language;
```

---

## 3. `codescope_trace` 传类型名返回 `isError` —— **已修复（2026-11-15）**

**现象（修复前）**：

```
codescope_trace({function_name: "MemScopeGuard"})
→ isError: true, text: {"error":"function 'MemScopeGuard' not found",
                        "name":"MemScopeGuard","callers":[],"callees":[]}
```

**根因**：该工具只接受函数名（`MemScopeGuard` 是 `kind=2` 的 struct），错误体只有 `error`/`name`。

**修复**：`exploreFunctionImpl` 的错误体改由 `functionNotFoundJson()` 构造（`engine_queries_context.cpp`），保留原有字段并追加：

- `entity_id` / `entity_kind`：名字命中了一个非函数实体时给出它的 id 与 kind（未命中时均为 0，两个分支字段集一致）；
- `hint`：命中非函数实体时指向 `get_type_info` / `get_subgraph`；完全未命中时提示用 `find_symbol` 检查拼写；
- `lookup_error`：实体查询本身失败时给出原因，此时 `hint` 明确写"查询失败不等于名字不存在"，不把故障归因为拼写错误。查询成功但未命中与查询失败是两种答案（`NamedEntity::ok`）。

首次 callable 查询的状态同样进入判定：`DiagnoseMissingCallable(callable_ok, entity_ok, entity_id)` 里 `callable_ok=false` 优先于兜底结果，因此"首次失败、兜底查到行"不会被报告成"非函数实体"。该纯函数被单测直接覆盖（表级故障无法造出这种组合——两条 SQL 读同一张表）。

`exploreFunctionImpl` 由 169 行降到 161 行（`code_rules` §1.1 只允许缩短）。

**验证**：`test_mcp_selftest_regressions::CheckTraceDiagnostics` 断言类型名返回 `entity_kind=2` 且 hint 含 `get_type_info`，不存在的名字返回 `entity_id=0` 且 hint 含 `find_symbol`。

---

## 4. `shortest_path` 不区分“不可达”与“没找到节点” —— **已修复（2026-11-15）**

**现象（修复前）**：三个项目均为：

```
{"path":[{"node_id":<起点>}],"found":false,"approximation":"heuristic",
 "note":"Call graph edges are resolved by name matching; ..."}
```

**根因**：响应只回答“有没有路径”，不回答“搜索是否离开过起点”。

**修复**：`findShortestPath` 的 not-found 分支追加：

- `resolved: {from, to, from_in_graph, to_in_graph}`：两个 id 是否真的存在于本项目（只回显 id 会把不存在的端点说成“已解析”）；
- `endpoints_verified`：端点查询本身是否成功，失败时为 `false`，不冒称已验证；
- `visited`：BFS 实际到达的节点数（1 = 起点没有出边）；
- `max_depth`：深度预算（10）；
- `depth_truncated`：是否因预算截断（存在未访问的越界边）。仅凭 `visited`/`max_depth` 无法区分“不可达”与“截断”。

邻接读取失败不再伪装成“没有边”：`getCalleeIds(node_id, bool *ok)` 在 prepare/step 失败时置 `ok=false`，BFS 立即返回 `endpoints_verified:false` + `error`（`adjacency lookup failed`），不会给出一个与正常不可达无法区分的空结果。

名字未解析的情况本就在 MCP 层显式报错（`server/src/tools/mod.rs` 的 `resolve_endpoint`），因此引擎侧只需区分“不可达 / 预算耗尽 / 端点不存在”。`findShortestPath` 保持 120 行以内。

**验证**：`CheckTraceDiagnostics` 断言 `to_id=999` 时 `to_in_graph=false`、`from_in_graph=true`、`endpoints_verified=true`；`CheckDepthBudget` 构造两种图——12 跳链（`visited=11`、`depth_truncated=true`）与 10 叶星形图（`visited=11`、`depth_truncated=false`，目标 `250` 存在但不可达）——断言节点数相同而诊断不同。

---

## 附：本次实测的时间基线（毫秒）

| 工具 | 本仓库 | goagent | memscope-rs |
|---|---|---|---|
| index（force_index_files） | 9770 | 6666 | 1452 |
| enhance_project | 19513 | 1377 | 3222 |
| verify_integrity（固定预算） | 10001 | 10001 | 10001 |
| analyze_project(conformance) | 171 | 80 | 41 |
| find_callers / find_callees | 1.6 / 1.3 | 4.1 / 4.1 | 0.8 / 0.8 |
| graph_query | 38 | 35 | 5 |
| detect_ffi_boundaries | 59 | 183 | 34 |

除三个预算型工具外，其余工具在 5.4 万节点图上均低于 200 ms。

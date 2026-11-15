# MCP 全工具实测记录：四条已知限制（第 1 项已修复，其余待处理）

## Meta

| 项目 | 内容 |
|---|---|
| 发现时间 | 2026-11-15 |
| 状态 | **第 1 项已修复（2026-11-15）；第 2–4 项仅记录**，后续单独立项 |
| 测试方式 | 手动 stdio 会话：`printf` 逐条发送 JSON-RPC，每行响应打时间戳；未使用脚本文件 |
| 被测版本 | `bin/codescope` 0.2.7（2026-11-15 15:47 构建） |
| 测试项目 | 本仓库（1122 文件 / 14842 节点）、`~/go/src/goagent`（1908 文件 / 53792 节点）、`~/code/rustcode/memscope-rs`（297 文件 / 10049 节点） |
| 覆盖 | 48 工具 × 3 项目 = 144 次调用，另加 6 次 `analyze_project` 视图 |

结论先行：48 个工具全部可调用，仅 1 次 `isError`（第 3 条的参数不适配）。以下四条是“结果与预期不符”的差异，都不属于协议错误。第 1 项已在同一轮修复（见该节）。

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

## 2. Rust 的 trait 声明与 impl 实现仍报 `ambiguous`

**现象**：

```
find_symbol("snapshot_json")   → 7574 (trait 声明, guard.rs:86), 7674 (impl)
trace_flow("snapshot_json")    → {"callers":[],"callees":[],"ambiguous":true,...}
analyze_project(view=symbol_context, scope={symbol:"snapshot_json"})
                               → {"ambiguous":true, "candidates":[2 项]}
find_callees("snapshot_json")  → 有结果（边合并生效）
```

**根因**：`callable_signature` / `is_definition` 只有 C/C++ visitor 写入。Rust 的 trait 声明与 impl 方法没有签名事实，`resolveCallableGroups` 按规则让它们各自成组（保守，不猜）。

**影响**：C/C++ 之外的“一个符号两行”结构（Rust trait/impl、Go interface/实现）无法收敛，`symbol_context` 返回候选而非定义，`trace_flow` 的 homonym 探针直接拒绝。

**建议**：Rust/Go visitor 补 `CallableFacts`（Rust 可用 `impl Trait for Type` 的 self type + 参数类型；Go 可用接收者类型 + 参数类型），或至少在文档与工具描述里说明“配对仅覆盖 C/C++”。

---

## 3. `codescope_trace` 传类型名返回 `isError`

**现象**：

```
codescope_trace({function_name: "MemScopeGuard"})
→ isError: true, text: {"error":"function 'MemScopeGuard' not found",
                        "name":"MemScopeGuard","callers":[],"callees":[]}
```

**根因**：该工具只接受函数名（`MemScopeGuard` 是 `kind=2` 的 struct）。

**影响**：错误信息本身准确，但 MCP 客户端看到 `isError` 会按“工具故障”处理；同时没有提示“类型请用 `get_type_info`”。

**建议**：错误体补 `hint`（“this name is a type; use get_type_info or get_subgraph with its id”），或在找到类型实体时自动改走类型路径。

---

## 4. `shortest_path` 不区分“不可达”与“没找到节点”

**现象**：三个项目均为：

```
{"path":[{"node_id":<起点>}],"found":false,"approximation":"heuristic",
 "note":"Call graph edges are resolved by name matching; ..."}
```

已核对源码，所选端点确实不可达（例如 `upsertFile` → `engine_create` 之间没有调用链），因此 `found:false` 是正确结果。

**根因**：响应只回答“有没有路径”，不回答“两个端点是否都解析到了节点”“是否触碰深度上限”。

**建议**：补 `resolved: {"from_id":…, "to_id":…}`（或 `unresolved: ["<name>"]`）与 `max_depth_reached`，让 `found:false` 可归因。

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

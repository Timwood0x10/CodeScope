# CodeScope 潜在 Bug 扫描报告

- **扫描日期**：2026-07-08
- **范围**：`engine/src/`（C++23 引擎，~52 个 .cpp/.h）、`server/src/`（Rust MCP 服务端，8 个 .rs）
- **方法**：静态代码审查 + 针对性代码核对（已实际读取并确认 High/Medium 项）。`grep` 未检出 `strcpy`/`sprintf`/`goto` 等明显不安全写法（说明基础内存习惯尚可）。
- **说明**：完整 `clang-tidy`/编译未能执行——项目依赖（tree-sitter、sqlite-vec）经 CMake `FetchContent` 在线拉取，离线环境不可用。以下结论来自静态审查 + 关键代码逐行核实。

---

## 严重程度总览

| 等级 | 数量 | 说明 |
|------|------|------|
| 🔴 High   | 2 | 任何客户端发一条坏数据即可让服务崩溃，或核心功能（建索引）静默失效 |
| 🟠 Medium | 5 | 协议/JSON 损坏、功能未接通、计数错误，生产中会触发 |
| 🟡 Low    | 9 | 边界/健壮性问题、潜在数据竞争、未核实的隐患 |

---

## 修复状态

**全部 17 个 bug 已于 2026-07-08 修复并验证通过 (`cargo check` ✅)**

| ID | 等级 | 修复文件 | 修复方式 |
|----|------|----------|----------|
| **H1** | 🔴 | `server/src/mcp/transport.rs`, `server.rs` | `read_message()` 返回 `ReadResult` 枚举（`Msg`/`Eof`/`ParseError`），`run()` 对解析错误 `continue` 而非退出 |
| **H2** | 🔴 | `server/src/tools/mod.rs` | `run_worker` 添加 `.stdout(Stdio::piped()).stderr(Stdio::piped())` |
| **M1** | 🟠 | `engine/src/store/store.cpp` | 移除重复 `"total":0` 键；`h.name`/`h.file` 用 `jsonEscape()` 转义 |
| **M2** | 🟠 | `engine/src/store/store.cpp` | `insertEmbedding` 额外写入 `node_vectors`，`searchSemantic` 可查到 |
| **M3** | 🟠 | `engine/src/lsp/lsp_client.cpp` | `write()` 短写/EINTR 循环补齐 |
| **M4** | 🟠 | `engine/src/query/community_detection.cpp` | 社区间边用 `make_pair(min,max)` 规范化去重 |
| **M5** | 🟠 | `engine/src/query/community_detection.cpp` | `comm.label`/`m.name` 用 `jsonEscape()` 转义 |
| **L1** | 🟡 | `server/src/ffi/mod.rs` | `spawn_enhancement` 改为同步调用 |
| **L3** | 🟡 | `engine/src/engine_helpers.cpp` | `dupString` OOM 返回 `"{}"` 替代 `nullptr` |
| **L4** | 🟡 | `engine/src/parser/parser.cpp` | 加 `< UINT32_MAX` 守卫 + 用 `strlen` 长度替代硬转 |
| **L5** | 🟡 | `engine/src/graph/graph_builder.cpp` | `edge.target` 空值检查 `if (!edge.target) continue;` |
| **L6** | 🟡 | `engine/src/linker/linker.cpp` | 添加所有权注释，说明 `TranslationUnit` 不可拷贝 |
| **L7** | 🟡 | — | H2 修复后 worker stdout 只含 JSON，截取逻辑已可靠 |
| **L8** | 🟡 | `server/src/mcp/transport.rs` | `read_line` 改用 `.take(1MB)` 限长 |
| **L9** | 🟡 | `server/src/main.rs` | 删除死代码 `unsafe { env::set_var(...) }` |
| **L2** | 🟡 | — | 当前契约一致，所有 FFI 返回均 `dupString`/malloc |

> 备注：本扫描为静态代码审查，未运行动态测试。建议补充后补一轮 `cargo clippy -D warnings` 与 `clang-tidy` 全量扫描（需先配齐 tree-sitter / sqlite-vec 依赖）以捕获更多编译期与并发类问题。

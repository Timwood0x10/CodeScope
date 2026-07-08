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

## 🔴 High

### H1. 一条非法 JSON 行即可让整个 MCP 服务退出
- **位置**：`server/src/mcp/transport.rs:39` + `server/src/mcp/server.rs:28`
- **问题**：`read_message()` 在 JSON 解析失败时向 stdout 写出 `-32700` 错误后 `return Ok(None)`；而 `run()` 把 `None` 一律当作 EOF 处理（`None => return Ok(())`），直接退出主循环。
- **后果**：任何客户端发送一行无法解析为 `Request` 的 JSON（网络拥塞、恶意/客户端 bug），服务端立即终止，所有在途查询丢失。对常驻服务而言是可用性硬伤。
- **修复**：区分「解析错误」与「EOF」。让 `read_message` 返回枚举（如 `Eof | ParseError | Msg`），解析错误时 `continue` 主循环而不是退出。

### H2. 索引 worker 子进程 stdout 未接管 → MCP 协议污染 + 结果丢失
- **位置**：`server/src/tools/mod.rs:108-113`（`run_worker`）
- **问题**：`Command::new(exe)` 未设置 `.stdout(Stdio::piped())`/`.stderr(...)`，Unix 下默认 **继承** 父进程 stdio。worker 用 `println!` 输出结果 JSON（`main.rs:63`）→ 该 JSON 直接混入服务端 MCP stdout 传输流（破坏协议帧），同时 `child.wait_with_output()` 因 fd 被继承而读不到任何内容 → 返回空 `Output`。
- **后果**：`h_index_project`（每次 `index_project`）拿到空/垃圾结果，且服务端 stdout 被污染，客户端可能解析失败或死锁。
- **修复**：`cmd.stdout(Stdio::piped()).stderr(Stdio::piped())` 后再 `spawn()`；并让 worker 只向 stdout 打印最终 JSON（不要掺杂日志）。

---

## 🟠 Medium

### M1. `searchSemantic` 输出重复 `total` 键 + 未转义字段
- **位置**：`engine/src/store/store.cpp:780, 788, 790, 793`
- **问题**：JSON 开头写 `{"total":0,"results":[`，结尾又写 `],"total":<n>}`——**重复 `total` 键**（严格 JSON 解析器会报错）。同时 `h.name` / `h.file` 直接拼接，未做 `jsonEscape` 转义，符号名或路径含 `"`、控制字符时产生**非法 JSON**。
- **修复**：去掉开头的 `"total":0`；对 `name`/`file` 用 `jsonEscape(...)` 包裹。

### M2. enhance 流水线写入的 `embeddings` 表从未被读取（功能未接通）
- **位置**：`engine/src/store/store.cpp:1319`（`insertEmbedding` → `INSERT INTO embeddings`）vs `store.cpp:710-715`（`searchSemantic` 读取 `node_vectors`）
- **问题**：增强阶段（`engine_queries.cpp` 的 enhance 路径）把向量写入 `embeddings` 表，但 `searchSemantic` 查询的是 `node_vectors` 表（由 `storeVector` 写入，见 `engine_index.cpp:218`、`engine_ffi.cpp:527`）。`embeddings` 在整个代码树中**没有任何 `SELECT`**。
- **后果**：增强阶段的向量计算与落库是**死写**——语义搜索实际上忽略了这部分结果，浪费算力且功能名不副实。
- **修复**：二选一——让 enhance 路径改调 `storeVector()` 写入 `node_vectors`，或让 `searchSemantic` 改为 `UNION embeddings`。

### M3. LSP 写出把「短写/EINTR」当致命失败 → 协议失步
- **位置**：`engine/src/lsp/lsp_client.cpp:390-397`
- **问题**：`if (written < 0 || written != msg.size()) return false;` 对管道而言，`write()` 在缓冲区满或收到 `EINTR` 时**允许返回短写**（< 完整长度且 ≥0），代码把它判为失败，导致后续消息截断→LSP 服务端协议失步/死锁。无 `EINTR` 重试、无循环补齐。
- **修复**：用 `while (offset < msg.size()) { ssize_t n = write(...); if (n<0 && errno!=EINTR) break; if (n>0) offset+=n; }` 循环补齐。

### M4. 社区间边被重复计数
- **位置**：`engine/src/query/community_detection.cpp:255-267`
- **问题**：`adjacency` 是无向图，每条边会同时产生 `(comm_a,comm_b)` 与 `(comm_b,comm_a)`。经 `sort`+`unique` 后二者仍是不同有序对 → 全部保留，社区间边在输出列表中**被计两次**。
- **修复**：插入前做 `make_pair(min,max)` 规范化，或用 `set` 去重。

### M5. 社区 JSON 中未转义的名字字段
- **位置**：`engine/src/query/community_detection.cpp:297, 312`
- **问题**：`comm.label` 与 `m.name` 直接拼接进 JSON（`"name\":\"" << m.name`），名称中含 `"` 时生成非法 JSON。
- **修复**：统一经 `jsonEscape(...)` 处理。

---

## 🟡 Low

| # | 位置 | 问题 | 建议 |
|---|------|------|------|
| L1 | `server/src/ffi/mod.rs:434` | `spawn_enhancement` 在 `std::thread` 上并发访问 C++ 全局 `g_store`，而主循环也在服务查询——代码注释已承认存在数据竞争（UB）。 | 用全局 `Mutex` 串行化引擎访问，或把 enhance 放到主线程执行。 |
| L2 | `server/src/ffi/mod.rs:124,137-144` | 所有返回的 `*mut c_char` 都无条件 `engine_free_string`；若任一 C++ 函数返回静态/字面量指针则 UB。 | 在 C++ 侧用文档/断言固定契约，并加单测。 |
| L3 | `engine/src/engine_ffi.cpp` / `engine_helpers.cpp:111` | `dupString` 在 OOM 时返回 `nullptr`，多个 FFI 入口直接把该指针返回给调用方（可能解引用）。 | OOM 时返回静态 `"{}"` 或空 JSON 字面量。 |
| L4 | `engine/src/parser/parser.cpp:103`、`engine_index.cpp:588` | `ts_parser_parse_string(..., static_cast<uint32_t>(strlen(source)))`：遇内嵌 `\0` 截断，且 >4GiB 时溢出。单文件 `Parser::parse` 路径无 `max_file_size` 守卫。 | 显式传长度并对 `source.size() > UINT32_MAX` 做拒绝/分块。 |
| L5 | `engine/src/graph/graph_builder.cpp:150,161` | `edge.target->id` 无条件解引用；若某 `SemanticEdge` 的 target 为 `nullptr` 会空指针解引用（目前 translator 均提供有效 target，属潜在问题）。 | 加 `if (!edge.target) continue;`。 |
| L6 | `engine/src/linker/linker.cpp:185-197` | `new ir::Node()` 裸指针 stub 依赖 `~TranslationUnit` 析构释放；若 `TranslationUnit` 被值拷贝会双重释放。 | 改用 `std::unique_ptr`/`vector<unique_ptr>` 显式所有权。 |
| L7 | `server/src/tools/mod.rs:218-221` | 从 worker 输出截取 JSON 用「第一个 `{`」到「最后一个 `}``」，前置日志含 `{...}` 会使 `candidate` 非法。 | worker 仅输出 JSON；或用 `Content-Length` 帧解析。 |
| L8 | `server/src/mcp/transport.rs:11` | `read_line` 不限长，超长行可耗尽内存（DoS）。 | 使用有上限的读取或 `Content-Length` 分帧。 |
| L9 | `server/src/main.rs:33-35` | `unsafe env::set_var` 实际未被使用（`ffi::init(&db_path)` 已显式传路径）。 | 删除该死代码。 |

---

## 建议优先级

1. **立即修**：H1、H2（崩溃/核心功能失效，影响所有请求）。
2. **尽快修**：M1、M2、M3（协议/JSON 损坏、功能未接通）。
3. **排期修**：M4、M5、L1–L9（健壮性、竞争、潜在空指针）。

> 备注：本扫描为静态代码审查，未运行动态测试。建议补充后补一轮 `cargo clippy -D warnings` 与 `clang-tidy` 全量扫描（需先配齐 tree-sitter / sqlite-vec 依赖）以捕获更多编译期与并发类问题。

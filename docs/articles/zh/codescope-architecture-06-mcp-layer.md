# CodeScope 架构拆解（六）：MCP 协议层——35+ 工具的设计哲学

> 我犯过一个错误。早期设计 CodeScope 的时候，我给每个 C++ 函数都配了一个 MCP 工具——一个函数对应一个工具，看起来完美映射。结果工具列表膨胀到 60+，AI 根本不知道该用哪个。用户反馈说："你工具太多了，AI 随机挑一个调用，经常选错。"
>
> 后来我做了三件事：砍掉重复的工具，用前缀分组，给每一个工具写**像人话一样**的描述。工具列表从 60+ 降到 35+，AI 的调用准确率翻了一倍。

---

## 三个问题

MCP 协议层的设计，本质上要回答三个问题：

1. **AI 怎么知道有哪些工具可用？**（工具发现）
2. **AI 怎么选对工具？**（工具路由）
3. **AI 怎么理解工具返回什么？**（响应契约）

这三个问题，每一个都在做同一个权衡：**给 AI 的信息越多，token 越贵；给的信息越少，AI 越可能犯错。**

---

## 整体架构

```
 MCP Client (Claude Desktop / Cursor / 自定义客户端)
        │
        │  JSON-RPC 2.0 over stdin/stdout
        │  {"jsonrpc":"2.0","method":"tools/call",
        │   "params":{"name":"find_callers","arguments":{...}}}
        ▼
┌─────────────────────────────────────┐
│  transport.rs                       │
│  stdin/stdout 逐行 JSON-RPC 2.0      │
└──────────┬──────────────────────────┘
           ▼
┌─────────────────────────────────────┐
│  mcp/server.rs                      │
│  dispatch():                        │
│    "initialize" → 自动索引项目       │
│    "tools/list"  → 返回 35+ 工具    │
│    "tools/call"  → 路由到 handler   │
└──────────┬──────────────────────────┘
           │  tools::execute(pid, name, args)
           ▼
┌─────────────────────────────────────┐
│  tools/mod.rs                       │
│  TOOL_HANDLERS: HashMap<&str, fn>   │
│  ┌────────────────────────────┐    │
│  │  35+ handler functions     │    │
│  │  h_find_callers()          │    │
│  │  h_search()                │    │
│  │  h_codescope_trace()       │    │
│  │  h_build_context()         │    │
│  └────────┬───────────────────┘    │
└───────────┼────────────────────────┘
            │ ffi::scan_project(), ffi::index_project(), etc.
            ▼
┌─────────────────────────────────────┐
│  ffi/mod.rs (Rust ↔ C++ FFI)       │
│  unsafe extern "C" → 安全包装       │
│  take_string() → 自动 free C 内存   │
└──────────┬──────────────────────────┘
           │ C ABI (动态链接)
           ▼
┌─────────────────────────────────────┐
│  C++ Engine (engine.so)             │
│  engine_scan_project()              │
│  engine_index_project()             │
│  engine_find_callers_adaptive()     │
│  → 所有函数返回 char* (JSON)        │
└─────────────────────────────────────┘
```

---

## 工具分类：三阶段就绪

35+ 个工具不做平铺，而是按**就绪阶段**分类：

### Phase A：亚秒级工具

| 工具 | 做什么 | 响应时间 |
|---|---|---|
| `scan_project` | 扫描项目，提取声明 | ~367ms |
| `find_symbol` | 精确符号匹配 | 即时 |
| `get_module_tree` | 模块树 | 即时 |
| `get_entry_points` | 入口点（main/initcall/probe） | 即时 |

这些工具只依赖 Phase A 数据，永远立即可用。AI 可以**在任何时候**调用它们。

### Phase B：后台增强工具

| 工具 | 做什么 | 响应时间 |
|---|---|---|
| `enhance_project` | 全量解析 → 调用图 → 复杂度 → 向量嵌入 | ~数秒 |
| `get_enhancement_status` | 查询各能力就绪度 | 即时 |

`enhance_project` 是唯一**异步执行**的工具。它被调用后立即返回，真正的全量解析在后台线程执行。AI 通过 `get_enhancement_status` 轮询进度。

### Phase C：自适应统一工具

| 工具 | 做什么 | 自适应逻辑 |
|---|---|---|
| `search` | 统一搜索 | Phase B 未完成 → FTS5；已完成 → 向量搜索 |
| `find_callers` | 调用者查询 | Phase B 未完成 → 旧图查询；已完成 → 新 call_edges 表 |
| `project_overview` | 项目概览 | 综合所有就绪数据 |

这些工具**内部检查就绪度**，自动降级为可用的数据源。AI 不用关心"调用图有没有建好"，工具自己会处理。

### 特色工具

| 工具 | 描述 |
|---|---|
| `codescope_trace` | 递归调用链探索 / 最短路径 BFS |
| `codescope_build_context` | **主工具**：自然语言查询 → 智能上下文组装 |
| `codescope_capabilities` | 标准化能力就绪报告 |
| `count_tokens` | Token 估算（纯 Rust，不跨 FFI） |

`codescope_build_context` 是唯一被标注为 **PRIMARY** 的工具——它把"需要多次调用才能完成的任务"打包成一次调用。AI 描述它为：

> "AI 模型的首选工具。使用自然语言描述你需要的代码上下文，返回符号定义、引用、调用图、文件内容等的智能组合。"

---

## 工具描述的艺术

MCP 协议里，工具的描述是 AI 选择工具的唯一依据。CodeScope 为每个工具撰写了高度精确的描述：

```rust
// server/src/tools/mod.rs
Tool {
    name: "codescope_build_context",
    description:
        "AI model's primary tool. Use natural language to describe \
         the code context you need: 'how does module X handle errors', \
         'why is function Y returning null', etc. Returns a smart \
         assembly of symbol definitions, references, call graphs, \
         and file contents relevant to your question.",
    input_schema: ...,
}
```

关键原则：
- **第一句说清楚"这是干什么的"**——AI 快速匹配意图
- **给调用示例**——"how does module X handle errors" 这种自然语言示例，告诉 AI 这个工具可以理解自然语言
- **明确输出内容**——"returns symbol definitions, references, call graphs, file contents"
- **不写实现细节**——AI 不需要知道背后是 C++ 还是 SQLite

对比坏描述：

```
find_callers: "Query the function call edges table to find all caller functions
              that call the specified callee function. Uses graph traversal
              on the symbol call edge table."
```

好描述：

```
find_callers: "Return all functions that call a given function.
              Use this when you need to know who invokes a specific function."
```

AI 不需要知道"call_edges table"或"graph traversal"，它只需要知道"谁调用了这个函数"。

---

## FFI 桥：Rust 如何驱动 C++

所有 MCP 工具最终都要调用 C++ 引擎。这个调用链的关键是 **FFI（Foreign Function Interface）桥**，定义在 `server/src/ffi/mod.rs`：

### 三层设计

**第一层：C ABI 声明**

```rust
// server/src/ffi/mod.rs
unsafe extern "C" {
    fn engine_init(db_path: *const c_char) -> i32;
    fn engine_scan_project(pid: u64, dir: *const c_char, 
                           lang: *const c_char) -> *mut c_char;
    fn engine_free_string(ptr: *mut c_char);
    // ... 约 50 个函数
}
```

每个 C++ 函数返回 `*mut c_char`——一个堆分配的 C 字符串。Rust 读取后必须用 `engine_free_string` 释放。

**第二层：内存管理辅助**

```rust
fn cstr(s: &str) -> CString {
    CString::new(s).unwrap_or_else(|_| CString::new("").unwrap())
}

fn take_string(ptr: *mut c_char) -> String {
    if ptr.is_null() { return String::new(); }
    let s = unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() };
    unsafe { engine_free_string(ptr) };  // 自动释放 C 内存
    s
}
```

`take_string` 是关键的"内存契约"——Rust 借用 C++ 的内存，读取后立即归还。不会发生内存泄漏，也不会 double free。

**第三层：安全包装**

```rust
pub fn scan_project(project_id: u64, dir_path: &str, 
                    language_filter: Option<&str>) -> String {
    let lf = language_filter.map(cstr);
    take_string(unsafe {
        engine_scan_project(
            project_id,
            cstr(dir_path).as_ptr(),
            lf.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
        )
    })
}
```

所有 `unsafe` 调用被封装在安全函数内。`Option<&str>` 映射为 null 指针。Rust 侧看不到任何 `extern "C"` 调用。

### 为什么不用 bindgen？

手动写 FFI 包装而不是用 bindgen 自动生成，原因是：
- **极少的函数签名**（~50 个），不值得引入构建依赖
- **需要精细控制内存释放时机**——`take_string` 模式
- **C++ 端的 JSON 字符串是自描述的**——不需要 Rust 侧的复杂类型映射

---

## 统一的响应契约

所有工具遵循相同的响应格式：

```json
// 成功
{"ok": true, "total": 3, "results": [...]}

// 错误
{"error": "function not found"}
```

MCP Server 自动检测错误：

```rust
// server/src/mcp/server.rs
let is_error = serde_json::from_str::<Value>(&result)
    .ok()
    .and_then(|v| v.get("error").cloned())
    .and_then(|e| if e.is_null() { None } else { Some(true) });
```

如果响应 JSON 中包含非 null 的 `error` 字段，自动标记为 `is_error: true`，MCP 客户端会知道这次调用失败了。

这种设计的好处是：**C++ 侧不需要知道 MCP 协议**。它只需要返回 JSON 字符串。错误的检测和协议适配完全由 Rust 侧处理。

---

## Worker 子进程

唯一不在进程内执行的工具是 `index_project`。它的 handler 是特殊的：

```rust
// server/src/tools/mod.rs (简化)
fn h_index_project(project_id: u64, args: &Value) -> String {
    ffi::shutdown();  // 释放 SQLite 锁
    let child = Command::new("codescope")
        .args(["worker", db_path, dir, lang, name, &pid])
        .spawn();
    // 300 秒超时监控
    // kill -9 如果超时
    // 最多重试 3 次
    ffi::init();  // 重新初始化引擎
    // ...
}
```

其他 34+ 个工具全是进程内 FFI 调用。这意味着：

- **查询路径零延迟**——没有进程间通信
- **共享 SQLite 连接**——无需序列化/反序列化
- **唯一的风险点** —— `index_project` 意外退出会导致引擎状态不一致

这也是为什么 `scan_project` 之后会自动触发后台 `enhance_project`（在进程内），而 `index_project` 必须隔离到子进程。

---

## 坦诚反思

**1. 项目 ID 生命周期问题**

`Server` 结构体只存了一个 `project_id`。如果 AI 先扫描项目 A，再扫描项目 B，server 的 `self.project_id` 就变成了项目 B。后续所有查询都会落到项目 B 上，项目 A 的查询自然出错。

**当前解法**：忽略——假设用户只同时处理一个项目。这不是好解法，但实际场景中很少出现"同时处理两个项目"的 MCP 用例。

**2. 重复的 enhancement 触发**

`scan_project` 触发后台 enhancement 的逻辑被**重复实现**了两次——一次在 `execute()` 里，一次在 `handle_call_tool` 里。这意味着某些路径下可能会触发两次 enhancement。虽然 `spawn_enhancement` 内部有去重逻辑（`Mutex<HashSet>`），但这仍然是代码异味。

**3. 工具参数零校验**

所有 handler 都是用 `args["name"].as_str().unwrap_or("")` 获取参数。如果 AI 传错了参数类型（比如传了数字而不是字符串），Rust 不会报错，只是静默地用空字符串代替。这让调试变得困难——你永远不知道是 AI 用错了工具，还是工具用错了参数。

**4. 初始化阻塞**

在 `initialize` 阶段，server 会同步调用 `ffi::create_project()` 和 `ffi::index_project()`。这意味着 MCP Server 在建立连接的那个瞬间会卡住，等到索引完成才返回 `initialized` 响应。对于大型项目，这可能导致客户端超时。

---

## 系列导航

| 文章 | 主题 |
|---|---|
| (一) 开篇 | 56KB vs 629 bytes，CodeScope 要解决什么问题 |
| (二) 渐进式就绪 | 367ms 让 AI 开始理解你的代码 |
| (三) Worker 隔离 | 为什么索引不会拖垮 MCP Server |
| (四) 零冗余响应 | 1 Token 干 35 个 Token 的活 |
| (五) C++ 引擎拆解 | 从源码到多维代码图的管线 |
| **(六) MCP 协议层** | **35+ 工具的设计哲学 ← 本文** |
| (七) 语言翻译器 | 10 种语言 → 统一 IR |
| (八) 存储层 | SQLite WAL + FTS5 + vec0 |
| (九) 自适应查询 | Fallback 机制与就绪检测 |
| (十) 性能真相 | 从 200 到 60,000 文件的实测 |

---

下一篇我们将拆解 **语言翻译器**——tree-sitter 为 10 种语言生成的 CST，如何通过 Visitor 模式翻译成统一的 IR。为什么 Rust 的 visitor 和 JavaScript 的 visitor 长得完全不一样？
# CodeScope v0.2.7 发布前深度 Code Review 报告

**审查日期**: 2026-10-02  
**审查范围**: `engine/` (C++23) + `server/` (Rust 2024) + 构建系统  
**对照规范**: `plan/rules/code_rules.md` v1.0  
**审查人**: AI Agent

> **后续处理状态（2026-10-04 更新）**：本报告的阻塞项与 P1 已在 v0.2.7 发布前及 0.2.8 开发中处理完毕，
> 详见 §6.2 / §6.3 / §6.4 的复选框与提交号。仍开放的是 TD-1（刀 2/3）与 TD-5（Windows 并行索引）。

---

## 目录

1. [总体结论](#1-总体结论)
2. [技术债务](#2-技术债务)
3. [潜在 Bug](#3-潜在-bug)
4. [敷衍的测试](#4-敷衍的测试)
5. [编码规范合规性检查](#5-编码规范合规性检查)
6. [发布就绪度评估](#6-发布就绪度评估)

---

## 1. 总体结论

### 1.1 发布判定：**有条件通过 (Conditional Pass)**

v0.2.7 在正确性和稳定性方面做了大量出色的工作。CHANGELOG 记录了约 60+ 个 bug fix，涵盖了跨语言调用边、事务嵌套破坏 savepoint、use-after-free、线程安全、MCP 协议合规等关键问题。代码质量在 0.2.x 系列中是最好的一个版本。

**但仍存在以下阻碍无条件发布的问题：**

| 严重级别 | 数量 | 说明 |
|---------|------|------|
| P0 (阻塞发布) | 1 | `waitForKnowledgeBuilder` 的递归锁 + 无超时可能死锁 |
| P1 (应修复) | 4 | 全局可变状态、SQL 注入面、unsafe 块缺少安全注释 |
| P2 (建议修复) | 6 | 技术债务、测试覆盖缺口 |
| P3 (低优先级) | 5 | 代码风格、文档完善 |

**建议**：修复 P0 后发布，P1 可在 0.2.8 中跟进。

---

## 2. 技术债务

### TD-1: 全局可变单例模式 (g_store / g_query / g_parser)

**位置**: `engine/src/engine_internal.h:33-35`  
**严重级别**: P1  
**规范引用**: §2 "Avoid global/static mutable state unless strictly necessary"

```cpp
extern std::unique_ptr<store::GraphStore> g_store;
extern std::unique_ptr<query::QueryEngine> g_query;
extern std::unique_ptr<Parser> g_parser;
```

**问题**: 三个全局可变单例贯穿整个引擎。虽然注释声明"Rust MCP server calls FFI functions SEQUENTIALLY from a single thread"，但实际代码中：
- `index-parallel` 会 fork worker 子进程
- `async_knowledge.cpp` 在后台线程中使用 `g_store`
- chunk-worker 模式下多个进程可能竞争

**影响**: 任何未来对并发 FFI 的需求都需要大规模重构。`waitForKnowledgeBuilder()` 使用 `std::recursive_mutex` 在共享连接上序列化所有读写，这是一个权宜之计而非架构方案。

**建议**: 中期应将引擎状态封装为一个 `EngineContext` 结构体，通过 FFI 传递句柄。短期可维持现状但需在文档中明确标注线程模型约束。

### TD-2: SQL 字符串拼接 (SQL Injection 面)

**位置**: `engine/src/engine_ffi.cpp:606-643` (`engine_get_type_info`)  
**严重级别**: P1  
**规范引用**: §5 "No magic numbers", 错误处理规范

```cpp
std::string sql =
    "SELECT ti.name, ti.qualified_name, ti.kind, ti.file_path, "
    " ti.language, ti.start_row, "
    " (SELECT COUNT(*) FROM type_ref tr WHERE tr.type_name = ti.name "
    "  AND tr.project_id = ti.project_id) AS ref_count "
    "FROM type_info ti WHERE ti.project_id=" +
    std::to_string(project_id);
```

**问题**: `project_id` 通过字符串拼接进入 SQL（虽然 `uint64_t` 不含注入风险，但违反了参数化查询原则）。`type_name_filter` 虽然做了 LIKE 转义，但转义逻辑是手写的，容易遗漏。

**同类问题**: `store_graph.cpp:64-66` 中 `std::to_string(project_id)` 拼接 SQL。

**建议**: 统一使用 `sqlite3_prepare_v2` + `sqlite3_bind_int64` 参数化查询。

### TD-3: 手写 JSON 序列化 (C++ 侧)

**位置**: `engine/src/engine_ffi.cpp:148-214` (`engine_get_capabilities`)  
**严重级别**: P2

```cpp
std::ostringstream json;
json << "{"
     << "\"project_id\":" << project_id << ","
     << "\"total_symbols\":" << total << ","
     << "\"capabilities\":{"
     << "\"fast_scan\":{\"available\":true,\"ready\":true,...},"
     // ... 40+ lines of manual JSON
     << "}";
```

**问题**: 引擎侧大量使用 `std::ostringstream` 手写 JSON，容易在特殊字符转义、浮点数格式化上出错。`jsonEscape` 虽存在但需手动调用，遗漏即产生无效 JSON。

**影响范围**: `engine_ffi.cpp`、`engine_verify_ffi.cpp`、`engine_queries.cpp` 等几乎所有 FFI 文件。

**建议**: 引入轻量级 JSON 库（如 nlohmann/json）或构建一个最小化的 JSON builder 工具类，统一处理转义和类型序列化。

### TD-4: Rust 侧 FFI 声明重复

**位置**: `server/src/ffi/decls.rs` vs `server/tests/test_graph_ffi.rs:28-36` vs `server/tests/test_knowledge_ffi.rs:27-36`  
**严重级别**: P2

```rust
// test_graph_ffi.rs
unsafe extern "C" {
    fn engine_init(db_path: *const c_char) -> i32;
    fn engine_shutdown();
    fn engine_create_project(root_path: *const c_char, name: *const c_char) -> u64;
    // ...
}
```

**问题**: 三个地方各自维护 `extern "C"` 声明，新增/修改 FFI 函数时容易遗漏同步。

**建议**: 将 FFI 声明提取为 `pub` 模块，测试文件直接引用。

### TD-5: `index-parallel` 路径下 Windows 支持缺失

**位置**: `server/src/main.rs:89-181`  
**严重级别**: P2

```rust
if args.len() >= 2 && args[1] == "index-parallel" {
    #[cfg(not(windows))]
    { ... }
    #[cfg(windows)]
    {
        eprintln!("error: index-parallel is not available on Windows...");
        std::process::exit(1);
    }
}
```

**问题**: `scheduler` 模块在 Windows 上完全不可用（`mod scheduler` 被 `#[cfg(not(windows))]` 排除），意味着 Windows 用户只能使用单线程 `index`，对于大型项目体验很差。

**建议**: 短期文档明确标注限制；中期考虑基于命名管道/文件的跨平台调度方案。

### TD-6: `store_membulk.cpp` 注释与代码不一致

**位置**: `engine/src/store/store_membulk.cpp:46-53` vs `85-107`  
**严重级别**: P3

```cpp
// dropSemanticRecordIndexes 注释说 "Drop the 11 lookup indexes"
// 但 createSemanticRecordIndexes 注释说 "Recreate the 9 semantic_records indexes"
// 实际两个函数都操作 11 个索引
```

**问题**: 注释中索引数量不一致（11 vs 9），虽然 CHANGELOG 提到修复了类似的不一致，但这里仍然遗留。

---

## 3. 潜在 Bug

### BUG-1: `waitForKnowledgeBuilder` 递归锁 + 无超时 = 潜在死锁 (P0)

**位置**: `engine/src/async_knowledge.h:55-77`  
**严重级别**: **P0 — 阻塞发布**

```cpp
/// Wait for the background knowledge builder before a READ touches the shared
/// store, and hold the shared-connection lock until the returned guard is
/// destroyed.
std::unique_lock<std::recursive_mutex> waitForKnowledgeBuilder();
```

**问题**:
1. **无超时**: `waitForKnowledgeBuilder()` 使用 `std::recursive_mutex` 的 `lock()`，没有超时机制。如果后台知识构建线程挂起（例如 SQLite `_BUSY` 死循环），所有读入口点将永久阻塞，导致 MCP server 完全无响应。
2. **递归锁掩盖设计问题**: 注释说"the mutex is recursive so nested FFI entry points on one thread can re-acquire it"——这实际上是允许在同一线程内重入，但如果后台线程也需要这个锁来完成，而前台线程持有锁等待后台，就会形成死锁（虽然注释说后台线程不经过这些入口点，但这是一个脆弱的隐式约束）。

**影响**: MCP server 在特定时序下可能完全挂死，无法响应任何请求。

**修复建议**:
- 为 `waitForKnowledgeBuilder()` 添加超时参数（如 30s），超时后返回并降级为"知识层不可用"而非永久阻塞。
- 或者改为 `try_lock` + 超时循环，并在超时时记录日志并继续。

### BUG-2: `engine_get_type_info` 中 SQL LIKE 转义不完整

**位置**: `engine/src/engine_ffi.cpp:614-643`  
**严重级别**: P1

```cpp
// Escape special characters for SQL LIKE: % _ and '
std::string filter(type_name_filter);
size_t pos = 0;
while ((pos = filter.find('\\', pos)) != std::string::npos) {
    filter.replace(pos, 1, "\\\\");
    pos += 2;
}
// ... escape %, _, '
sql += " AND ti.name LIKE '%" + filter + "%' ESCAPE '\\'";
```

**问题**: 虽然转义了 `\`, `%`, `_`, `'`，但：
1. `ESCAPE '\\'` 在 C++ 字符串中实际是 `ESCAPE '\'`（单反斜杠），这是正确的。
2. 但如果 `type_name_filter` 包含 NUL 字节，`std::string` 会截断，导致不完整的 SQL。
3. 整个转义逻辑应该在 `sqlite3_bind_text` 之前完成并使用参数化查询，而非拼接。

**修复建议**: 改用参数化查询：`... AND ti.name LIKE ? ESCAPE '\'`，然后 `sqlite3_bind_text(stmt, N, escaped_filter.c_str(), -1, SQLITE_TRANSIENT)`。

### BUG-3: `main.rs` 中 `unsafe { env::set_var(...) }` 数据竞争

**位置**: `server/src/main.rs:707-709`  
**严重级别**: P1  
**规范引用**: §2 "Avoid global/static mutable state"

```rust
// Safety: this runs before any threads are spawned (server.run()
// starts later), so there is no data race on the environment.
unsafe {
    env::set_var("CODESCOPE_DB_PATH", &db);
}
```

**问题**: 注释声称"this runs before any threads are spawned"，但在 Rust 2024 edition 中，`env::set_var` 被标记为 `unsafe` 正是因为多线程环境下的安全问题。虽然在此处确实是单线程阶段，但：
1. 如果未来有人在 `server.run()` 之后通过 MCP 工具间接调用 `env::set_var`，就会产生真正的 data race。
2. `std::env` 在 Unix 上使用 `setenv(3)`，修改全局 `environ`，任何并发的 `getenv` 都是 UB。

**建议**: 在启动时一次性读取所有配置，避免运行时修改环境变量。将 DB path 存为 `Server` 结构体字段而非环境变量。

### BUG-4: chunk-worker 中 `project_id == 0` 的 fallback 可能创建幽灵项目

**位置**: `server/src/main.rs:515-527`  
**严重级别**: P1

```rust
let pid = if project_id > 0 {
    project_id
} else {
    let new_pid = ffi::create_project(".", &format!("chunk-worker-{}", worker_id));
    // ...
    new_pid
};
```

**问题**: 当 `project_id` 为 0 时，chunk-worker 创建一个以 "." 为根路径的项目。这意味着：
1. 多个 chunk-worker 各自创建独立的项目，项目名不同但根路径相同。
2. 合并阶段 `unify_project` 需要处理这些项目 ID 的映射，但如果 `create_project` 返回的 ID 被写入各自 DB 的 `projects` 表，合并时的项目发现可能失败。

**建议**: chunk-worker 不应有 fallback 创建逻辑——`project_id` 必须由调度器统一分配并强制非零，0 值应直接报错退出。

### BUG-5: `run_produced_index` 逻辑反转

**位置**: `server/src/scheduler/mod.rs:141-143`  
**严重级别**: P1

```rust
pub(super) fn run_produced_index(total_nodes: u64, total_files_indexed: u64) -> bool {
    total_nodes > 0 || total_files_indexed == 0
}
```

**问题**: 这个函数名为 "run produced index"，但当 `total_nodes == 0 && total_files_indexed == 0` 时返回 `true`。这意味着一个完全没有文件的空项目被判定为"产生了索引"。虽然注释解释了"a project whose files all yield no symbols... must not report success"，但逻辑表达让人困惑——`total_files_indexed == 0` 应该表示"什么都没做"，而非"成功"。

**实际影响**: 一个目录下没有任何源文件的 `index-parallel` 调用会被判定为"产生了索引"，与 `discover_modules` 返回空列表时的 `ok: false` 处理不一致。

**建议**: 修正为 `total_nodes > 0 || total_files_indexed > 0`，并在上层用单独的标志表示"合法的空项目"。

---

## 4. 敷衍的测试

### TEST-1: Rust 集成测试只验证 JSON 信封，不验证语义

**位置**: `server/tests/test_graph_ffi.rs`, `server/tests/test_knowledge_ffi.rs`  
**严重级别**: P1  
**规范引用**: §4 "Boundary & Edge Cases are mandatory"

```rust
// test_graph_ffi.rs
fn test_find_connected_components_empty_db_returns_envelope() {
    // ...
    assert!(json.get("components").map(|v| v.is_array()).unwrap_or(false),
        "result must contain a components array");
    assert!(json.get("total").map(|v| v.is_number()).unwrap_or(false),
        "result must contain a numeric total");
}
```

**问题**: 所有 Rust 集成测试都在**空数据库**上运行，只验证 JSON 结构（字段存在、类型正确），从不验证：
- 实际索引代码后的查询结果正确性
- 边界条件（超大 limit、负数 depth、超长字符串）
- 并发 FFI 调用的线程安全
- 错误路径（engine 未初始化、DB 损坏）

这些测试虽然比没有好，但给人一种"FFI 已经被测过了"的虚假安全感。CHANGELOG 中提到的大量 FFI 相关 bug（NULL 返回、isError 拼写错误等）说明这些测试不足以捕获真实问题。

**建议**:
- 添加"索引小型项目→查询→验证结果"的端到端测试。
- 对每个 FFI 函数添加 NULL/空输入、超长输入、无效 JSON 输入的测试。
- 考虑 property-based testing（`proptest`）对参数 clamping 的覆盖。

### TEST-2: `test_accuracy_baseline.cpp` 是信息采集而非断言

**位置**: `engine/tests/test_accuracy_baseline.cpp:18-21`  
**严重级别**: P2

```cpp
// This test does NOT assert correctness of the call graph — Step 0 only
// captures state. Step 2 introduces the real TP/FP/FN benchmark.
```

**问题**: 该测试只收集基线数据，不做任何正确性断言。虽然注释说"Step 2 introduces the real benchmark"，但 `test_call_graph_accuracy` 才是真正的精度门控。`test_accuracy_baseline` 作为 `make test-engine` 的一部分运行但只产出报告，如果后续步骤没有验证基线变化，它的存在价值有限。

**建议**: 要么在此测试中加入基本断言（如 TP >= 30, FP == 0），要么将其移到 `manual/` 目录避免在常规测试中产生噪声。

### TEST-3: 测试中使用 `assert()` + Debug 构建依赖

**位置**: `engine/tests/test_communities.cpp:51` (及几乎所有 engine 测试)  
**严重级别**: P1  
**规范引用**: §4 "Boundary & Edge Cases are mandatory"

```cpp
assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
```

**问题**:
1. 所有 C++ 测试使用 `assert()`，在 Release/RelWithDebInfo 构建中会被 `-DNDEBUG` 编译掉，变成空操作。Makefile 中已有警告（`test-engine` target），但这意味着如果 CI 配置错误，所有测试都会"pass"——这是**敷衍测试的最极端形式**。
2. `assert` 失败时不输出有用的诊断信息（不打印哪个值不对、不打印 SQL 错误消息）。

**实际风险**: CHANGELOG 提到"26 passing tests were never run"因为 CI skip list 漂移——如果这些测试在 Release 下运行，即使有 skip list 问题也不会被发现。

**建议**:
- 使用自定义 `check(cond, msg)` 宏（部分测试已有）替代 `assert`，在所有构建模式下生效。
- 在 `CMakeLists.txt` 中为测试目标强制 `(-DNDEBUG OFF)` 或 `-UNDEBUG`。

### TEST-4: `test_e2e.cpp` 固定输入覆盖面极窄

**位置**: `engine/tests/test_e2e.cpp`  
**严重级别**: P2

```cpp
const char* defs[] = {"add", "Calculator", nullptr};
runE2eTest("python", code, "/tmp/calculator.py",
           defs, 2,
           "add", "multiply",
           "add",
           "multiply",
           "main", "Calculator");
```

**问题**: 整个 E2E 测试只验证一个 Python 计算器示例，覆盖面极窄：
- 只测试 Python，不覆盖 C/C++/Go/Rust/Java/JS/TS
- 不测试增量索引、不测试错误恢复
- 不测试大文件、深嵌套、Unicode 标识符

**建议**: 虽然有 `test_cpp_e2e.cpp`、`test_go_e2e.cpp` 等单独的 E2E 测试，但每个都只覆盖一种语言的极简案例。应增加跨语言调用图、增量重索引后的一致性测试。

### TEST-5: `test_fp_cpp.cpp` 等误报测试验证不充分

**位置**: `engine/tests/test_fp_cpp.cpp`  
**严重级别**: P2

```cpp
const char* builtins[] = {
    "printf", "__builtin_expect",
    nullptr
};
runFPVerificationTest("cpp", code, "/tmp/test_fp_cpp.cpp",
                      "mainFunc", "user_function", builtins);
```

**问题**: 测试只验证 `printf` 和 `__builtin_expect` 被过滤，但不验证：
- 复杂的 stdlib 函数（`std::cout`, `std::string` 方法等）
- 模板实例化后的调用
- 运算符重载调用
- Lambda 内的调用

---

## 5. 编码规范合规性检查

### ✅ 合规项

| 规范条目 | 状态 | 说明 |
|---------|------|------|
| §1 文件大小限制 (1000 行) | ✅ 合规 | 最大文件 982 行 (`pipeline.cpp`) |
| §1 注释语言为英文 | ✅ 合规 | 代码注释均为英文 |
| §1 Rust 2024 Edition | ✅ 合规 | `Cargo.toml` 声明 `edition = "2024"` |
| §1 C++23 | ✅ 合规 | `CMakeLists.txt` 设置 `CMAKE_CXX_STANDARD 23` |
| §2 C++ RAII | ✅ 合规 | 使用 `unique_ptr`, `vector`, `string` |
| §3 FFI `extern "C"` | ✅ 合规 | 所有 FFI 函数使用 `extern "C"` |
| §3 FFI 内存所有权 | ✅ 合规 | `dupString` + `engine_free_string` 配对 |
| §3 FFI 异常安全 | ✅ 合规 | 所有 FFI 函数有 try/catch 包裹 |
| §3 FFI 错误处理 | ✅ 合规 | 返回 JSON 错误字符串，带 `[module=, method=]` 标签 |
| §5 错误处理 (Rust) | ✅ 合规 | 使用 `Result<T, E>` |
| §5 无 magic numbers | ⚠️ 部分合规 | 大部分有命名常量，少量硬编码 |

### ⚠️ 不合规项

| 规范条目 | 状态 | 违规详情 |
|---------|------|---------|
| §2 Rust `unsafe` 安全注释 | ⚠️ 部分合规 | `ffi/mod.rs` 中 62 处 `unsafe` 调用，多数有 `# Safety` 文档但部分内联 unsafe 块缺少详细不变量说明 |
| §2 "Avoid global/static mutable state" | ❌ 不合规 | `g_store`, `g_query`, `g_parser` 全局可变单例 (TD-1) |
| §4 "Every public function must have tests" | ⚠️ 部分合规 | C++ 侧测试覆盖较好 (96 个测试文件)，Rust 侧仅 3 个集成测试文件 |
| §4 "Property-based testing where appropriate" | ❌ 不合规 | 未使用 `proptest` |
| §4 "Integration tests must use real dependencies" | ⚠️ 部分合规 | Rust 测试使用空 DB 而非真实索引数据 (TEST-1) |
| §3 "Every FFI function must have a comment block explaining memory ownership" | ⚠️ 部分合规 | `engine_ffi.cpp` 顶部有统一合同注释，但个别函数缺少逐函数的 ownership 说明 |

---

## 6. 发布就绪度评估

### 6.1 评分卡

| 维度 | 评分 (1-5) | 说明 |
|------|-----------|------|
| **正确性** | ⭐⭐⭐⭐ | 0.2.7 修复了大量关键正确性 bug（跨语言边、事务嵌套、UAF 等），精度门控 TP 36/FP 0/FN 0 |
| **稳定性** | ⭐⭐⭐ | `waitForKnowledgeBuilder` 无超时死锁风险 (BUG-1)；chunk-worker 项目 ID fallback 问题 (BUG-4) |
| **测试质量** | ⭐⭐⭐ | C++ 侧 96 个测试文件覆盖面广，但 assert() 在 Release 下失效 (TEST-3)；Rust 侧测试只验证 JSON 信封 (TEST-1) |
| **代码规范** | ⭐⭐⭐⭐ | 文件大小、注释语言、FFI 安全合同基本合规；全局可变状态和 SQL 拼接是主要偏差 |
| **构建系统** | ⭐⭐⭐⭐⭐ | CMake + build.rs 配置完善，vendored 依赖零网络，generator 冲突已修复 |
| **文档** | ⭐⭐⭐⭐ | CHANGELOG 极其详尽（近 700 行），README 有中英双语，但部分注释与代码不一致 (TD-6) |

### 6.2 必须修复 (发布阻塞)

- [x] **BUG-1**: 为 `waitForKnowledgeBuilder()` 添加超时机制，防止 MCP server 永久挂死 —— 已修：`recursive_timed_mutex` 30 s 超时 + 5 分钟后一次 `sqlite3_interrupt()` 自愈（`97253c6`）

### 6.3 强烈建议修复 (发布后立即跟进)

- [x] **BUG-2**: `engine_get_type_info` 改用参数化查询 —— 已修：`sqlite3_bind_*` 绑定 `project_id` 与 LIKE 过滤（`97253c6`）
- [x] **BUG-3**: 消除运行时 `env::set_var`，改为传递参数 —— 已修：DB 路径改为参数传递，`env::set_var` 调用点已删除（`97253c6`）
- [x] **BUG-4**: chunk-worker 的 `project_id == 0` 兜底创建幽灵项目 —— 已修：`project_id == 0` 直接报错退出，必须由调度器分配（`97253c6`）
- [x] **BUG-5**: `run_produced_index` 逻辑反转 —— 已确认并澄清：函数语义改为"运行级空缺守卫"，空项目交由 `run_complete`/`merge` 判定，`test_empty_project_reports_incomplete` 锁定行为（`97253c6`）
- [x] **TEST-3**: 用自定义 `check()` 宏替换所有测试中的 `assert()` —— 已修：新增 `engine/tests/test_check.h`（`CHECK()` 记录失败并继续、`checkFailures()` 计数），**929 处 `assert()` / 35 个文件**全部迁移，`main` 改为 `return checkFailures() ? 1 : 0;`
- [x] **TEST-1**: Rust 集成测试增加真实索引场景的端到端验证 —— 已修：`test_graph_ffi.rs` 单文件索引 e2e（`97253c6`）+ 新增 `server/tests/test_index_e2e.rs`（索引夹具目录 → `get_graph_stats` / `find_definition` / `search_code` 语义断言）

### 6.4 中期技术债务

- [ ] **TD-1**: 引擎状态从全局单例迁移为句柄传递 —— **进行中**：刀 1（接缝）新增 `engine/src/engine_context.{h,cpp}`，三个全局收进 `EngineContext` 并以引用别名保持调用点不变；刀 2 已把 **340 处**调用点全部迁到 `engineContext().store/.query/.parser`、删除别名、把单例改为**函数内静态对象**（不再有全局对象）。每批以 43 工具差分矩阵验证字节一致；重索引差异只有 85 条新增的 `→ engineContext` 边。刀 3（FFI 传句柄、支持多实例）待做
- [x] **TD-3**: C++ 侧引入 JSON 库替代手写序列化 —— 已完成：`util::JsonWriter` 全量迁移，转义收敛到 `util::jsonEscapeString`，重复定义/声明删除（`d44f0fd`、`ed045ff`、`9d48238`、`4bb5e6a`）
- [x] **TD-5**: Windows 平台并行索引支持 —— 代码已完成：新增跨平台映射层 `server/src/scheduler/mapped_file.rs`（POSIX `mmap` / Windows section object，同一契约），`shm.rs` 与 `chunk_queue.rs` 复用并删除重复映射代码；硬编码 `/tmp` 改为 `std::env::temp_dir()`；`main.rs` 解除 `index-parallel` / `chunk-worker` 的 Windows 门禁。交叉编译（`x86_64-pc-windows-gnu`）0 error 且产出 PE32+ `codescope.exe`，宿主端到端两条调度路径均跑通（1766 节点），`dev.yml` 增加 `windows-smoke`。**剩余**：`windows-smoke` 需在真实 Windows runner 上首次跑绿（尚未执行过），且 Windows 上 merge 步骤仍依赖 `sqlite3` CLI
- [x] 增加 `proptest` 覆盖参数 clamping 和边界条件 —— 已完成：`tools/clamp.rs`（depth/radius/edge_type/limit/max_communities/max_members/findings 的"永不截断"性质）、`scheduler/worker.rs`（`CODESCOPE_EXCLUDE_PATHS` 转义往返）、`mcp/protocol.rs`（JSON 可解析 + 转义往返）

### 6.5 最终结论

**v0.2.7 在修复了 0.2.6 系列大量关键 bug 后，正确性达到了该系列的最佳水平。** 代码中的错误处理意识（`[module=, method=]` 标签）、FFI 安全合同、以及 CHANGELOG 中体现的"每个 fix 都有 falsification 验证"的工程文化，都表明这是一个认真维护的项目。

**但 `waitForKnowledgeBuilder` 的无超时锁是一个真实的可用性风险**——在特定时序下（后台构建线程卡在 SQLite BUSY），MCP server 将永久无响应，且没有任何恢复机制。这是一个不应该带进发布的 P0 问题。

**建议**: 修复 BUG-1 后发布 v0.2.7。其余 P1 问题在 0.2.8 中跟进。

---

*本报告基于对以下核心文件的审查：`engine_ffi.cpp`, `store.h`, `store_graph.cpp`, `store_membulk.cpp`, `engine_internal.h`, `async_knowledge.h`, `CMakeLists.txt`, `main.rs`, `ffi/mod.rs`, `mcp/server.rs`, `mcp/protocol.rs`, `tools/mod.rs`, `scheduler/mod.rs`, `scheduler/worker.rs`, `scheduler/chunked.rs`, `build.rs`, `Makefile`, 以及多个测试文件。*

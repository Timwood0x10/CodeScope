# CodeScope v0.1.0 — Bug Report & 优化建议

> 生成日期: 2026-07-07 | 分支: dev

---

## 一、潜在 Bug

### 🔴 Critical

#### 1. `server/main.rs:28` — 不必要的 `unsafe` 且语义误导

```rust
unsafe {
    env::set_var("CODESCOPE_DB_PATH", db_path);
}
```

`std::env::set_var` 本身就是 safe 函数，不需要 `unsafe` 块。但这在 Rust 2024 edition 中可能会触发未定义行为，因为在多线程环境下调用 `set_var` 不是线程安全的。

**修复**: 移除 `unsafe` 块，或使用 `std::env::set_var` 在单线程启动阶段安全调用。

---

#### 2. `server/tools/mod.rs:169-178` — 多线程并发调用 C FFI 不安全

```rust
std::thread::spawn(move || {
    crate::ffi::build_fts(project_id);
});
```

`h_index_project` 中两次 spawn 后台线程调用 `crate::ffi::build_fts(project_id)`（重复代码），该 FFI 函数访问全局 C++ 对象（如 `g_store`），且没有任何同步机制。这会导致 data race，可能造成 SQLite 崩溃或数据损坏。

**修复**: 使用 Tokio 的 `spawn_blocking` 并通过 channel 将结果返回主线程处理，或确保 FFI 内部加锁。

---

#### 3. `engine/src/store/store.cpp:60-76` — 预编译语句 `stmt_fts_map_` 从未被使用

```cpp
sqlite3_prepare_v2(db_,
    "INSERT OR REPLACE INTO fts_node_map (node_id, project_id, file_id) "
    "VALUES (?, ?, 0)", -1, &stmt_fts_map_, nullptr);
```

`stmt_fts_map_` 在 `open()` 时 prepare，在 `close()` 时 finalize，但在整个代码库中 **从未被实际使用**。同时，`stmt_fts_` 和 `stmt_vector_` 的预编译语句也几乎没有被使用（实际的 FTS 插入路径重新 prepare 了新语句）。这是死代码，且占用了 SQLite 的 statement 配额。

---

#### 4. `engine/src/engine_lifecycle.cpp:48-51` — vec0 扩展硬编码 macOS 路径

```cpp
const char *gdir = getenv("GRAMMARS_DIR");
std::string base = gdir ? gdir : "grammars";
std::string vec_path = base + "/vec0.dylib";  // 仅 macOS!
```

`vec0.dylib` 是 macOS 的扩展名，在 Linux 上应该是 `vec0.so`，在 Windows 上是 `vec0.dll`。此外，`GRAMMARS_DIR` 变量名容易误导——它实际用于 vec0 加载而非 grammars（grammars 已静态编译到二进制）。

**修复**: 根据平台选择后缀：
```cpp
#ifdef __APPLE__
    std::string vec_path = base + "/vec0.dylib";
#elif _WIN32
    std::string vec_path = base + "/vec0.dll";
#else
    std::string vec_path = base + "/vec0.so";
#endif
```

---

### 🟠 High

#### 5. `server/build.rs:101-106` — 硬编码 SQLite 版本路径

```rust
let cellars = [
    "/opt/homebrew/Cellar/sqlite/3.53.3/lib",
    "/opt/homebrew/Cellar/sqlite/3.48.0/lib",
    "/opt/homebrew/opt/sqlite/lib",
];
```

任何 SQLite 版本更新都会导致本地编译失败。`/opt/homebrew/opt/sqlite/lib` 最后兜底勉强可用，但前两个硬编码路径完全多余。

---

#### 6. `Makefile:69-71` — 硬编码 Homebrew LLVM@21 编译器

```makefile
-DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm@21/bin/clang
-DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm@21/bin/clang++
```

没有安装 LLVM@21 的 macOS 用户无法编译，Linux 用户也无法使用此 Makefile。

---

#### 7. `install.sh:16-17` — Intel macOS 错误回退

```bash
x86_64|amd64)  ARTIFACT="codescope-x86_64-linux"
               echo "⚠️  Intel macOS not supported — falling back to Linux binary (may not work)" ;;
```

Intel Mac 用户被引导下载 Linux 二进制，该二进制 100% 无法在 macOS 上运行（ELF vs Mach-O）。应该直接报错退出而非碰运气。

---

#### 8. `engine/src/engine_lifecycle.cpp:86-95` — shutdown 顺序可能导致 use-after-free

```cpp
void engine_shutdown() {
    g_query.reset();   // QueryEngine 持有 g_store 裸指针!
    g_parser.reset();
    if (g_store) {
        g_store->close();
        g_store.reset();
    }
}
```

`QueryEngine` 构造函数接收 `g_store.get()` 裸指针。如果 `g_query.reset()` 在析构时访问 `g_store`（例如内部查询未完成），而此时 `g_store` 还活着但即将被 close，存在潜在风险。

**修复**: 先 close store，再 reset query。

---

#### 9. `server/tools/mod.rs:108-110` — `kill -9` 不可移植

```rust
let _ = Command::new("kill").args(["-9", &pid.to_string()]).output();
```

Windows 没有 `kill` 命令。应该使用平台特定的进程终止方法，或用 `child.kill()` 代替。

---

### 🟡 Medium

#### 10. `engine/src/engine_helpers.cpp:124-166` — `detectLanguage()` 无法检测无扩展名脚本

不检查 shebang (`#!/usr/bin/env python3`)，对有 shebang 但无扩展名的 Python/Shell 脚本返回 `nullptr`。同时 `.h` 文件一律识别为 C（而非 C++），可能误判 C++ 头文件。

---

#### 11. `engine/src/parser/parser.cpp:80-104` — 每次 `parse()` 创建新 `TSParser`

```cpp
TSParser *ts_parser = ts_parser_new();
// ... use it ...
ts_parser_delete(ts_parser);
```

批量解析时大量重复创建/销毁 parser 对象，影响性能。应该使用 parser pool 复用。

---

#### 12. `engine/src/linker/linker.cpp:182-188` — stub node 所有权不清晰

```cpp
auto *stub = new ir::Node();
// ... 
u->all_nodes.push_back(stub);
```

Linker pass 创建 raw pointer stubs 并推入 TranslationUnit 的 `all_nodes`。TranslationUnit 析构时 `delete` 这些指针。但如果 Linker pass 和 TranslationUnit 的生命周期管理不当，可能导致 double-free 或 leak。当前代码正确，但极度脆弱。

**修复**: 使用 `std::unique_ptr` 或让 Translator 统一管理 node 所有权。

---

#### 13. `engine/src/engine_scanner.cpp:37-55` — `looksLikeCFunction()` 对类型宏名无感知

`type_keywords` 数组包含 `SQLITE_API`，但不包含如 `__attribute__`、`__declspec` 等常见的编译器属性。Linux 内核代码常用的 `__init`、`__exit` 等也会误判。

---

#### 14. `engine/src/engine_ffi.cpp:236-270` — `engine_find_symbol` 重复检查逻辑分散

该函数中对空结果的智能提示和 language/callgraph 查询在 `engine_ffi.cpp` 硬编码，而这些逻辑应该封装在 `store` 或 `query_engine` 中。

---

#### 15. `server/main.rs:64-98` — CLI 模式每次启动都创建 `.codescope/` 目录

如果用户只是在 `/tmp` 临时测试，会留下 `.codescope/` 目录。

---

#### 16. `engine/src/engine_queries.cpp:43-44` — JSON 中的字符串查找过于脆弱

```cpp
if (result.find("\"results\":[]") != std::string::npos) {
```

如果 JSON 中 `results` 字段有空格（如 `"results": []`），此检测会失败。应该解析 JSON 或使用更健壮的匹配。

---

#### 17. `engine/src/engine_scanner.cpp:28-33` — `trimLeft` 只 trim 空格和 tab

```cpp
static std::string_view trimLeft(std::string_view s) {
    while (!s.empty() && (s[0] == ' ' || s[0] == '\t'))
        s.remove_prefix(1);
```

不处理 `\r`（CR），Windows 行尾或混合行尾的文件会出问题。

---

#### 18. `.github/workflows/_ci.yml:49` — CI 安装所有 Homebrew 包但没有锁版本

```yaml
brew install cmake ninja sqlite node git tree-sitter
```

没有版本锁定，CI 可能会因上游包更新而突然失败。

---

### 🟢 Low

#### 19. 多个 `.cpp` 文件有大量未使用的 `#include`

clangd 诊断显示每个 `engine_*.cpp` 文件都有 9-10 个 unused includes（如 `algorithm`、`cstdio`、`dlfcn_compat.h`、`filesystem`、`fstream`、`mutex`、`thread` 等）。影响编译速度和代码可读性。

---

#### 20. `engine/src/store/store.cpp:47-48` — PRAGMA 失败仅打印警告

```cpp
if (!exec("PRAGMA journal_mode=WAL"))
    fprintf(stderr, "WARN: PRAGMA journal_mode=WAL failed: %s\n", error_.c_str());
```

`synchronous=OFF` 是一个有数据丢失风险的设置。如果设置失败但继续运行（仅 warn），用户不知道数据可能不安全。

---

#### 21. `engine/src/query/query_engine.cpp:53-101` — `queryToJson()` 对 FLOAT/BLOB 类型处理不完整

```cpp
} else {
    const char *text = reinterpret_cast<const char *>(
        sqlite3_column_text(stmt, i));
```

当列类型为 `SQLITE_FLOAT` 或 `SQLITE_BLOB` 时，用 `sqlite3_column_text` 会得到格式可能不一致的字符串。应显式处理 `SQLITE_FLOAT`。

---

#### 22. `server/tools/mod.rs:78` — `WORKER_TIMEOUT` 5分钟对于大项目可能不够

```rust
const WORKER_TIMEOUT: Duration = Duration::from_secs(300);
```

索引 Linux 内核等大项目可能需要更长时间。

---

## 二、Release 自包含问题 & 优化方案

### 当前状态分析

好消息：**grammars 已经静态编译进了二进制**，不再需要 `grammars/*.so`。

`CMakeLists.txt` 正在将 tree-sitter grammar 的 `.c` 源文件编译到 `libastgraph_engine.a` 静态库中：
```cmake
set(GRAMMAR_SOURCES
    ${TREE_SITTER_NPM}/tree-sitter-c/src/parser.c
    ${TREE_SITTER_NPM}/tree-sitter-cpp/src/parser.c
    # ... 共 17 个 .c 文件
)
```

`codescope_grammars.h` 声明了所有静态链接的 grammar 函数，`parser.cpp` 通过函数指针直接调用，无需 dlopen。

### 当前 Release 包仍然依赖的动态库

| 依赖 | 来源 | 是否必需 |
|------|------|----------|
| `libtree-sitter` (tree-sitter C 库) | Homebrew/apt | **必需** — parser API |
| `libsqlite3` | Homebrew/apt | **必需** — 数据存储 |
| `libc++` / `libstdc++` | 系统 | **必需** — C++ 运行时 |
| `grammars/*.so` | **已废弃** | **不再需要** |
| `vec0.dylib/so` | sqlite-vec | 可选（向量搜索） |

### 问题

1. **CI 仍在打包 `.so` 文件**：`.github/workflows/_ci.yml:114-117` 将 `grammars/tree-sitter-*.so` 复制进 package，但这些文件已无用处。

2. **`GRAMMARS_DIR` 环境变量名误导**：现在只用于加载 vec0 扩展，不是 grammars。

3. **依赖宿主机的动态库**：用户仍需安装 `tree-sitter`、`sqlite3`、C++ 运行时。

### 自包含 Release 方案

#### 方案一：全静态链接（推荐）

```
目标：单个 `codescope` 二进制，零外部依赖
```

**具体步骤**：

1. **修改 CMakeLists.txt**：将 tree-sitter C 库也编译为静态库并入 engine
```cmake
# 替换 find_package/find_library
include(FetchContent)
FetchContent_Declare(ts_repo
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
    GIT_TAG v0.24.7 GIT_SHALLOW TRUE SOURCE_SUBDIR lib)
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(ts_repo)
set(TREE_SITTER_LIB tree-sitter)
```

2. **静态链接 sqlite3**：类似 Windows 分支的做法，下载 amalgamation 编译进 engine
```cmake
# 对所有平台统一使用 sqlite amalgamation
set(SQLITE3_AMAL_SRC "${CMAKE_BINARY_DIR}/_deps/sqlite3.c")
# 编译为 C 源文件
```

3. **修改 server/build.rs**：移除动态库链接
```rust
// 不再需要 println!("cargo:rustc-link-lib=dylib=tree-sitter");
// 不再需要 println!("cargo:rustc-link-lib=dylib=sqlite3");
// 因为是 engine 静态库的间接依赖，rustc 自动解析
```

4. **MUSL 编译 (Linux)**：在 CI 中使用 `x86_64-unknown-linux-musl` target
```yaml
- name: Build with MUSL
  run: |
    rustup target add x86_64-unknown-linux-musl
    cargo build --release --target x86_64-unknown-linux-musl
```

5. **CI 清理**：移除 grammar `.so` 打包步骤，移除 `GRAMMARS_DIR` 相关代码

**最终效果**：
- Linux: 单个静态二进制，可 `scp` 到任何 Linux 机器直接运行
- macOS: 依赖系统 `libc++.dylib`（macOS 自带），其他全静态
- vec0: 如果不可用，graceful fallback（当前已实现）

#### 方案二：Docker 分发（备选）

```dockerfile
FROM scratch
COPY codescope /codescope
ENTRYPOINT ["/codescope"]
```

#### 方案三：Bundled 动态库（当前折中）

保持当前模式但改进：将所有 `.so/.dylib` 放到 `lib/` 子目录，用 `$ORIGIN/lib` (Linux) / `@executable_path/lib` (macOS) 作为 rpath。

---

### 其它优化建议

#### 1. 简化安装流程

当前用户需要：
```
安装 cmake, ninja, sqlite, tree-sitter, node, npm, rust
npm install -g tree-sitter-cli + 9个grammar包
make build
```

优化为（全静态后）：
```
curl -fsSL https://raw.githubusercontent.com/.../install.sh | bash
# 或
brew install codescope
```

**一键 install.sh 改进**：
```bash
#!/bin/bash
# 自动检测 OS/ARCH → 下载对应二进制 → 解压到 ~/.local/bin
```

#### 2. 构建系统改进

- `Makefile`: 移除硬编码的 `/opt/homebrew/opt/llvm@21`，改用 `$(shell which clang++)` 兜底
- `build.rs`: 移除硬编码 SQLite 版本路径，改用 `pkg-config`
- CI: 使用 `actions/cache` 缓存 Homebrew 包和 Rust crate

#### 3. CI 改进

- `npm install -g tree-sitter-*` 在静态编译方案后可移除
- 添加 Windows CI（MinGW 交叉编译）或 MSVC target
- 添加 ARM Linux (aarch64) 构建

#### 4. 代码质量

- 统一清理 84 个 clangd unused-includes 警告
- `engine_scanner.cpp` 的 `trimLeft` 应处理 `\r`
- `detectLanguage()` 应检查 shebang
- 使用 `-Wall -Wextra -Wpedantic`（目前仅 `-Wall` ?）

#### 5. 文档改进

- `GRAMMARS_DIR` 环境变量名应改为 `CODESCOPE_VEC_DIR` 或废弃
- RELEASE.md 中移除 grammars 相关说明
- README 中标记 grammars 已内置，无需额外安装

---

## 三、总结

| 类别 | 数量 |
|------|------|
| Critical bugs | 4 |
| High bugs | 5 |
| Medium bugs | 8 |
| Low bugs | 4 |
| **合计** | **21** |

**最高优先级修复**：
1. `server/tools/mod.rs` — 多线程并发调用 C FFI（data race）
2. `engine/src/engine_lifecycle.cpp` — vec0 路径 platform 适配
3. `install.sh` — Intel Mac 回退逻辑
4. CI 移除无用的 `.so` 打包
5. 全静态链接消除外部依赖

# CodeScope Windows 适配修改建议

> 评估范围：`engine/`（C++23 引擎）+ `server/`（Rust MCP 服务端，经 `build.rs` 调 CMake 构建 C++ 静态库）
> 评估方式：静态代码分析 + 关键文件逐行核对（无 Windows 真机编译验证）
> 文档定位：可执行的工程改造路线图，非泛泛而谈

---

## 0. TL;DR

CodeScope 对 Windows **是认真做过兼容的**（专用 `platform_win.{h,cpp}`、CMake WIN32 分支、`build_ci.bat/.ps1`、`build.rs` Windows 分支），核心链路（解析 / 索引 / 图构建 / 调用分析 / git 状态）**可工作**。

但有 **3 类必须修的问题**：

| 级别 | 问题 | 影响 |
|---|---|---|
| 🔴 P0 | LSP 在 Windows 完全禁用 | 跳转定义 / 跨文件符号定位不可用 |
| 🔴 P0 | `vec0.dll` 未随包、向量搜索开箱即坏 | 语义检索 / 向量能力缺失 |
| 🟠 P1 | `build.rs` 强制 MinGW，与默认 MSVC Rust 工具链 ABI 冲突 | 普通用户 `cargo build` 直接失败 |
| 🟠 P1 | 构建依赖联网下载（sqlite amalgamation、tree-sitter） | 离线 / 受限 CI 不可用 |
| 🟡 P2 | 硬编码 `/` 路径分隔符、`_popen` shell 风险 | 健壮性 / 注入面 |

**预期工作量**：达到「一等公民」约需 P0 修复（~3-5 人日）+ P1 构建（~1-2 人日）+ P2 健壮性（~1 人日）+ CI 验证（~1 人日）。

---

## 1. 现状支持矩阵

| 能力 | macOS | Linux | Windows | 说明 |
|---|:---:|:---:|:---:|---|
| 核心解析/索引/图 | ✅ | ✅ | ✅ | `std::thread` 跨平台 |
| zstd 导出/导入 | ✅ | ✅ | ✅ | `_spawnlp` 替代 fork/execvp（`store_core.cpp:621,663`） |
| git status 变更检测 | ✅ | ✅ | ⚠️ | `_popen`+cmd.exe，已做 shell 元字符校验（`engine_scanner.cpp:620`） |
| FTS5 全文检索 | ✅ | ✅ | ✅ | sqlite 内建 |
| LSP（跳转/引用） | ✅ | ✅ | ❌ | 全 `#ifndef _WIN32` 空实现（`lsp_client.cpp:266/306/374/495/530`） |
| 向量 / 语义搜索 | ✅ | ✅ | ❌ | 加载 `vec0.dll` 但仓库只带 `vec0.dylib`（`engine_lifecycle.cpp:41`） |
| 构建（默认工具链） | ✅ | ✅ | ❌ | `cargo build` 默认 MSVC，但 `build.rs` 强制 MinGW gcc → ABI 不匹配 |
| 构建（MinGW + gnu target） | — | — | ✅ | 需手动切 `x86_64-pc-windows-gnu` |

---

## 2. P0：功能缺口修复

### 2.1 LSP 客户端 Windows 化（最高价值，工作量最大）

**现状**：`engine/src/lsp/lsp_client.cpp` 全部走 POSIX：

```cpp
// lsp_client.cpp:306  spawnProcess
pid_ = fork();              // 子进程 dup2 重定向 stdin/stdout
execlp(command, command, nullptr);

// lsp_client.cpp:353  readResponse
int flags = fcntl(stdout_fd_, F_GETFL, 0);
fcntl(stdout_fd_, F_SETFL, flags | O_NONBLOCK);
poll(&pfd, 1, timeout_ms);   // 非阻塞等待
```

Windows 分支全是空实现：`spawnProcess` `return false`、`readResponse` `return ""`、`isAvailable` `return false`、`stop` no-op。

**改造方案（三选一，按推荐度排序）**：

#### 方案 A：用 Win32 原生 API 重写（推荐，零新依赖）

用 `CreateProcessW` + 匿名管道（`CreatePipe`）+ 句柄继承替代 fork/exec/pipe：

```cpp
// 伪代码 — spawnProcess 的 Windows 实现
bool LspClient::spawnProcess(const char *command) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };  // 句柄可继承
    HANDLE child_out_read, child_out_write, child_in_read, child_in_write;
    CreatePipe(&child_out_read, &child_out_write, &sa, 0);
    CreatePipe(&child_in_read, &child_in_write, &sa, 0);
    SetHandleInformation(child_out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(child_in_write, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.hStdInput  = child_in_read;
    si.hStdOutput = child_out_write;
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags    = STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi{};

    // 注意：command 可能含空格，CreateProcess 需可写缓冲
    std::wstring wcmd = widen(command);
    CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE,
                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    // 父端关闭子进程用的写端/读端
    CloseHandle(child_out_write);
    CloseHandle(child_in_read);
    // 存句柄 + PID
    stdin_fd_  = _open_osfhandle((intptr_t)child_in_write,  _O_WRONLY);
    stdout_fd_ = _open_osfhandle((intptr_t)child_out_read,  _O_RDONLY);
    pid_       = (int)pi.dwProcessId;
    hProcess_  = pi.hProcess;  // 需在 LspClient 类新增 HANDLE 成员
    return true;
}
```

`readResponse` 的非阻塞等待用 `PeekNamedPipe`（查可读字节）+ `ReadFile` 替代 `poll`+`read`；`stop` 用 `TerminateProcess` + `WaitForSingleObject` 替代 `waitpid`。

> 关键约束：`platform_win.cpp` 现有的 `waitpid` shim 是按 PID `OpenProcess`，对于自己 fork 出来的进程应**直接保存 `HANDLE`** 而非靠 PID 反查，避免权限/句柄表问题。建议在 `LspClient` 类新增 `HANDLE hProcess_` 成员。

#### 方案 B：引入 `reproc` / `Boost.Process`
第三方库封装好跨平台子进程 + 管道。优点是少写代码，缺点是引入依赖（`reproc` 是 C 库，轻量；Boost.Process 重）。若项目要保持零 C++ 依赖的洁癖，不推荐。

#### 方案 C：用 ConPTY
能完整模拟 TTY，但 LSP 不需要 TTY，过度设计，不推荐。

**推荐**：方案 A。工作量集中在 `spawnProcess` + `readResponse` + `stop` 三个方法，约 150 行 Windows 代码。需在 `lsp_client.h` 加 `#ifdef _WIN32` 的 `HANDLE hProcess_` 成员。

---

### 2.2 向量搜索 `vec0.dll` 缺失（影响语义检索）

**现状**：
```cpp
// engine_lifecycle.cpp:40
#ifdef _WIN32
    vec_suffix = "/vec0.dll";        // Windows 找 vec0.dll
#elif __APPLE__
    vec_suffix = "/vec0.dylib";      // 仓库实际只带 .dylib
#endif
```
仓库 `grammars/` 下只有 `vec0.dylib`，Windows 用户拿不到 `vec0.dll`，`sqlite3_load_extension` 失败 → 向量表建不出来 → 语义搜索全废。

**改造方案**：

1. **随包发布预编译 `vec0.dll`**：在 CI 中用 MinGW 编译 sqlite-vec 的 `vec0.c`（sqlite-vec 是 amalgamation 风格，单文件可编），产物丢进 `grammars/vec0.dll`。
   ```bat
   REM 追加到 grammars/build_ci.bat
   if not exist "%~dp0sqlite-vec" git clone https://github.com/asg017/sqlite-vec "%~dp0sqlite-vec" --depth 1
   gcc -shared -O2 -I"%~dp0sqlite-vec" -o "%~dp0vec0.dll" "%~dp0sqlite-vec\sqlite-vec.c" -DSQLITE_VEC_OMIT_LITTLEENDIAN=0
   ```
   注意：sqlite-vec 默认按 `vec0` 符号导出，MinGW `-shared` 出来的 `vec0.dll` 入口名要对齐 `sqlite3_load_extension` 的 `sqlite3_*_init` 约定。

2. **静默降级而非崩溃**：当前加载失败只 `fprintf(stderr)` 后继续，逻辑上已降级（向量查询会在运行时报错）。建议在 `engine_get_capabilities` 的返回里加 `vector_available: false` 字段，让上层 MCP 客户端能感知到向量不可用，而不是发查询才失败。

3. **叠加修复已有的死写 bug**（见 `BUG_SCAN_REPORT.md` M2）：`store.cpp:1319` 写入的 `embeddings` 表从未被 `SELECT`，`searchSemantic` 读的是 `node_vectors`。即便 `vec0.dll` 到位，向量链路仍是断的。**这两个 bug 必须一起修**，否则 Windows 上向量「修好加载也用不了」。

---

## 3. P1：构建链路修复

### 3.1 MinGW vs MSVC 工具链冲突（最隐蔽的坑）

**现状**：`server/build.rs:159` 在 Windows 强制用 MinGW：
```rust
"windows" => {
    eprintln!("build.rs: Windows → gcc/g++ (MinGW)");
    ("gcc".to_string(), "g++".to_string())
}
```
但 `cargo build` 在 Windows 上的**默认** target 是 `x86_64-pc-windows-msvc`（用 MSVC linker + MSVC runtime）。于是出现：

- C++ 静态库 `astgraph_engine.a` 由 MinGW gcc（DWARF2/SJLJ 异常 + libgcc）编译
- Rust 二进制用 MSVC linker 链接，期望 COFF + MSVC runtime
- 结果：链接符号能找到但运行时 ABI（异常模型、`std::thread` 内部、`malloc`/`free` 跨 CRT 边界）会爆**难以排查的崩溃**

**改造方案（必须二选一并文档化）**：

#### 方案 A（推荐，改动小）：强制 `windows-gnu` target
在 `README` / `BUILDING.windows.md` 明确：
```
rustup target add x86_64-pc-windows-gnu
cargo build --target x86_64-pc-windows-gnu --release
```
并在 `build.rs` 启动时校验：
```rust
if target_os == "windows" {
    let env = env::var("CARGO_CFG_TARGET_ENV").unwrap_or_default();
    if env != "gnu" {
        panic!("CodeScope on Windows requires the gnu ABI. Run:\n  rustup target add x86_64-pc-windows-gnu\n  cargo build --target x86_64-pc-windows-gnu");
    }
}
```
`CARGO_CFG_TARGET_ENV` 在 `x86_64-pc-windows-msvc` 下是 `msvc`，在 `x86_64-pc-windows-gnu` 下是 `gnu`，可直接区分。

#### 方案 B（改动大）：支持 MSVC
`build.rs` 在 Windows 改用 `cl.exe`/`clang-cl.exe`，CMakeLists 用 `if(MSVC)` 分支处理 `/std:c++latest`、`/EHsc`、移除 `-fvisibility` 等。工作量显著，且要处理 MinGW 与 MSVC 在 C++23 stdlib（`std::print`、`<expected>`）上的差异。**不推荐现在做**，等 MSVC 用户有真实诉求再投入。

> 注意：方案 A 下，MinGW 的异常模型（DWARF2 vs SJLJ）也要和 `windows-gnu` Rust target 一致。`x86_64-pc-windows-gnu` Rust 用 SEH，MinGW gcc 默认在 x86_64 上也用 SEH（`--with-default-msvcrt` 除外）。建议在 `build_ci.bat` 里固定一个已知好用的 MinGW 发行版（如 w64devkit），写进文档。

---

### 3.2 联网下载依赖 → 离线构建

**现状**：CMake WIN32 分支在线下载 sqlite amalgamation + FetchContent 拉 tree-sitter：
```cmake
# CMakeLists.txt:67
file(DOWNLOAD "https://www.sqlite.org/2025/sqlite-amalgamation-3490100.zip" ...)
# CMakeLists.txt:84
FetchContent_Declare(ts_repo GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git ...)
```
离线 / 受限网络环境直接失败。

**改造方案**：
1. **预置 vendor 目录**：把 sqlite amalgamation zip 和 tree-sitter 源码 tarball 提交到 `engine/vendor/`，CMake 优先用本地：
   ```cmake
   if(WIN32)
       set(SQLITE_ZIP "${CMAKE_SOURCE_DIR}/vendor/sqlite-amalgamation-3490100.zip")
       if(EXISTS "${SQLITE_ZIP}")
           file(ARCHIVE_EXTRACT INPUT "${SQLITE_ZIP}" DESTINATION "${SQLITE3_DEPS_DIR}/")
       else()
           file(DOWNLOAD ... )   # 回退到在线
       endif()
   endif()
   ```
2. **tree-sitter 用 `FetchContent_Declare(... SOURCE_DIR ...)`** 指向 `vendor/tree-sitter/`，避免 git clone。
3. **CI 缓存**：GitHub Actions 用 `actions/cache` 缓存 `engine/build/_deps/`，二次构建免下载。

---

### 3.3 文法源两条路线不统一

**现状**：
- CMake `GRAMMAR_SOURCES`（`CMakeLists.txt:133`）从 npm 全局 `node_modules/tree-sitter-*/src/parser.c` **编译进二进制**（`codescope_grammars.h` 注释「no dlopen, no .so loading」）。
- `grammars/build_ci.bat` 是另一套：clone 文法仓库 → 编 `.dll`。

两条路没对齐。Windows 用户走 CMake 时，若 npm 没全局装文法，`TREE_SITTER_NPM` 为空 → `GRAMMAR_SOURCES` 全部指向不存在的路径 → 编译失败。

**改造方案**：
- 统一到「CMake 从源码编译进二进制」这条路。
- 在 CMake 里加一个 fallback：npm 全局没有时，从 `vendor/grammars/`（git submodule 或预 clone）取 `parser.c`。
- `build_ci.bat` / `build_ci.ps1` 改为只负责「准备 vendor/grammars/」这一前置步骤，不再编 `.dll`（因为主构建已经静态链接文法了）。
- 删除 `grammars/*.so` 和 `vec0.dylib` 从 git 追踪（它们是构建产物，不该入库），改为 CI 产出并发布到 release。

---

## 4. P2：健壮性硬化

### 4.1 路径分隔符

**现状**：硬编码 `/` 散落多处：
- `engine_scanner.cpp:589` `project_dir + "/.git"`
- `engine_scanner.cpp:759` `dir.back() == '/'`（Windows 尾斜杠是 `\\`，此判断不触发）
- `query_analysis.cpp:146` `path.rfind('/')`
- `filter_policy.cpp:576` `auto slash = path.rfind('/')`
- `engine_ffi.cpp:347-348` 路径拼接用 `/`

多数被 `std::filesystem` 和「Windows 接受 `/`」的事实缓解，但 `dir.back() == '/'` 这类**显式判断**会失真。

**改造方案**：
```cpp
// 新增工具函数（放到 engine_helpers.h）
inline bool isPathSep(char c) { return c == '/' || c == '\\'; }
inline std::string stripTrailingSep(std::string p) {
    while (!p.empty() && isPathSep(p.back())) p.pop_back();
    return p;
}
```
把所有 `dir.back() == '/'` / `path.rfind('/')` 替换为 `isPathSep` / 同时查 `\\`。
更彻底的做法：路径操作统一走 `std::filesystem::path`，字符串拼接改用 `operator/`。

> `filter_policy.cpp:650-651` 已经在 `strrchr` 里同时查 `/` 和 `\\`（注释「works on Windows too」），是个好范例，可推广到其他点。

---

### 4.2 `_popen` 的 shell 注入面

**现状**：`engine_scanner.cpp:620` Windows 下 git status 走 cmd.exe：
```cpp
std::string cmd = "git -C \"" + project_dir + "\" status --porcelain -z";
FILE *fp = _popen(cmd.c_str(), "r");
```
已做 `hasShellMeta` 校验（拦 `;|&$\`()\"\n\r%^!`），但**未拦 `\` 和 `>` `<`**。尾部 `\` 可能在 cmd.exe 里转义闭合的 `"`。

**改造方案（推荐）**：跟 zstd 一样改用 `_spawnlp` + 管道，彻底不走 shell：
```cpp
// Windows 也走无 shell 路径，与 POSIX 对称
int stdin_pipe[2], stdout_pipe[2];
if (_pipe(stdin_pipe, 0, _O_BINARY | _O_NOINHERIT) != 0) return changed;
if (_pipe(stdout_pipe, 0, _O_BINARY | _O_NOINHERIT) != 0) return changed;
int pid = _spawnlp(_P_NOWAIT, "git", "git", "-C", project_dir,
                   "status", "--porcelain", "-z", nullptr);
// 用 _read 读 stdout_pipe[0]，_cwait 收尸
```
这样 `project_dir` 永远不经 cmd.exe，`hasShellMeta` 校验可降级为防御性兜底而非主防线。

---

### 4.3 FFI 符号导出（仅当未来改 DLL 构建时需要）

**现状**：`engine.h` 用 `extern "C"` 但**无 `__declspec(dllexport)`**，当前引擎建为**静态库**（`add_library(astgraph_engine STATIC)`），Rust 直接静态链接，所以现在不需要导出宏。

**风险**：若未来想把引擎做成独立 DLL 给其他宿主（非 Rust）调用，Windows 下 `extern "C"` 不带 `dllexport` 不会导出符号。

**改造方案（前瞻，非必须）**：
```c
// engine.h
#if defined(_WIN32) && defined(ENGINE_SHARED)
#  define ENGINE_API __declspec(dllexport)
#else
#  define ENGINE_API
#endif
ENGINE_API int engine_init(const char* db_path);
```
现在保持 `ENGINE_API` 为空即可，留扩展点。

---

## 5. 实施路线图

### Phase 1 — 让 Windows「能用」（~3 人日，P0 + 关键 P1）
1. ✅ `build.rs` 加 `CARGO_CFG_TARGET_ENV` 校验，强制 `windows-gnu`（3.1 方案 A）。
2. ✅ 写 `BUILDING.windows.md`：MinGW 安装（推荐 w64devkit）、`rustup target add`、构建命令。
3. ✅ 修 `vec0.dll` 构建 + 随包（2.2-1）+ 修向量死写 bug（2.2-3，依赖 `BUG_SCAN_REPORT.md` M2）。
4. ✅ 路径分隔符 `isPathSep` 工具函数 + 替换 `engine_scanner.cpp:759` 等高频点（4.1）。

### Phase 2 — 功能对齐（~3 人日，LSP）
5. ⏳ `lsp_client.cpp` 用方案 A 重写 Windows 分支（2.1）：新增 `HANDLE hProcess_`、`spawnProcess` 用 `CreateProcessW`、`readResponse` 用 `PeekNamedPipe`+`ReadFile`、`stop` 用 `TerminateProcess`。
6. ⏳ 单元测试：mock 一个简单 LSP server（echo 脚本）验证 spawn/read/stop 在 Windows 上闭环。

### Phase 3 — 构建健壮（~2 人日，P1 剩余）
7. ⏳ vendor 离线依赖（3.2）：sqlite amalgamation + tree-sitter 源码预置。
8. ⏳ 文法源统一（3.3）：删 `grammars/*.so`/`vec0.dylib` 入库，改 CI 产物发布；CMake fallback 到 `vendor/grammars/`。

### Phase 4 — CI 验证（~1 人日）
9. ⏳ GitHub Actions 加 `windows-latest` + `windows-gnu` target job，跑 `cargo build` + 基础索引冒烟测试。
10. ⏳ 把 `vec0.dll` 构建步骤并进 Windows CI job，产物作为 artifact 上传。

### Phase 5 — 长期（按需）
11. ⏳ MSVC 工具链支持（3.1 方案 B），等真实用户诉求。
12. ⏳ `_popen` → `_spawnlp` 无 shell 化（4.2）。
13. ⏳ FFI `dllexport` 宏预留（4.3）。

---

## 6. 验收标准

- [ ] Windows 上 `cargo build --target x86_64-pc-windows-gnu --release` 一次成功，无需联网。
- [ ] Windows 上索引一个中型 C++ 项目，`calls` 阶段不崩、git status 能识别变更。
- [ ] Windows 上 LSP 跳转定义能命中（用一个真实语言服务器验证）。
- [ ] Windows 上 `engine_search_semantic` 返回非空结果（`vec0.dll` 加载成功 + 死写 bug 已修）。
- [ ] 路径含 `\\` 或盘符（`C:\Users\...`）时，`engine_scanner.cpp:759` 等位置正确处理尾斜杠。
- [ ] GitHub Actions Windows job 绿。

---

## 7. 不需要改的部分（已确认良好）

以下代码经核对在 Windows 上工作正确，**不要动**：

- `engine/include/platform_win.h` + `src/platform_win.cpp`：兼容层质量高（`waitpid` 正确编码 POSIX 退出状态、`PATH_MAX=4096`、不 `#define close _close` 避免污染成员函数名）。
- `store_core.cpp:621,663` zstd 的 `_spawnlp` 替代，与 POSIX 对称。
- `engine_index.cpp:651` `std::thread` 多线程，跨平台。
- `engine_lifecycle.cpp:40` vec 扩展按平台选后缀（`.dll/.dylib/.so`），逻辑正确，缺的只是 `.dll` 文件本身。
- `server/src/tools/mod.rs:136` `#[cfg(windows)] taskkill /F` 杀 worker，Rust 侧平台分支处理得当。
- `server/build.rs:117` Windows 分支正确指向 FetchContent 产出的 `tree-sitter` 静态库路径、sqlite amalgamation 编进 `astgraph_engine.a`。

---

*文档基于静态分析生成，所有 file:line 引用建议在动手前用最新代码复核一次。*

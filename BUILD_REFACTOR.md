# CodeScope 构建系统重构方案

> 日期：2026-07-08
> 状态：Proposed
> 目标：**一个命令、零外部依赖、全部编进二进制**

---

## 一、问题诊断：三套体系并存，互相打架

当前构建系统有 **三套入口**同时活着，互不一致：

| 体系 | 入口 | 依赖要求 | 状态 |
|------|------|---------|------|
| A. 活路径 | `make build` | cmake + clang + rust | ✅ 已正确（FetchContent 零配置） |
| B. 死代码 | `grammars/*.sh` + `*.so` | npm + node + gcc | ✗ 文法已编进二进制，这些从未被加载 |
| C. 误导 | README 旧步骤 | npm 16 包 + brew sqlite + curl sqlite-vec | ⚠ 全是无用操作，和 A 打架 |

### 隐藏矛盾（四处）

| 矛盾 | 位置 | 问题 |
|------|------|------|
| CMakeLists 双分支 | `engine/CMakeLists.txt:82-210` | `USE_SYSTEM_DEPS=ON` 走 npm（含 Swift），`OFF` 走 FetchContent（Swift omitted）——两套行为不一致 |
| build.rs 双路径 | `server/build.rs:114-179` | `USE_SYSTEM_DEPS=ON` 指 homebrew sqlite，`OFF` 指 FetchContent——两套链接逻辑 |
| Makefile DYLD_LIBRARY_PATH | `Makefile:124` | 测试时硬指 `/opt/homebrew/opt/sqlite/lib`——与"全编进二进制"矛盾 |
| Swift translator 半残 | `CMakeLists.txt:245-247` | `swift_visitor.cpp` + `swift_translator.cpp` 仍编译进二进制，但 FetchContent 路径文法已 omit → 编译了却用不了 |

---

## 二、目标态

```
make build（唯一入口）
    ↓
CMake configure（自动 FetchContent）
    ↓
┌─────────────┬──────────────┬─────────────┬──────────────┐
│ tree-sitter  │ sqlite3      │ 8 种语言    │ sqlite-vec   │
│ core v0.24.7 │ amalgamation │ 文法源码    │ 源码         │
│              │ (含 FTS5)    │             │              │
└─────────┬───┴──────┬───────┴──────┬──────┴──────┬───────┘
          ↓          ↓              ↓             ↓
    ┌─────────────────────────────────────────┐
    │  全部编进 astgraph_engine.a（静态库）    │
    └──────────────────┬──────────────────────┘
                       ↓
    ┌─────────────────────────────────────────┐
    │  codescope 二进制（自包含，零 .so/.dylib）│
    └─────────────────────────────────────────┘

新机器只需：cmake + clang + rust → make build → 搞定
```

**核心原则**：
- 唯一构建入口：`make build`
- 唯一依赖路径：CMake FetchContent
- 零系统库、零 npm、零 brew、零 curl sqlite-vec
- 二进制 `ldd`/`otool` 无 sqlite/tree-sitter 动态依赖

---

## 三、逐项清除清单

### 3.1 删除（死代码）

| # | 目标 | 原因 |
|---|------|------|
| D1 | `grammars/build.sh` | 文法已 FetchContent 编进二进制，此脚本编的 `.so` 从未被加载 |
| D2 | `grammars/build_ci.bat` | 同上，Windows CI 旧文法构建 |
| D3 | `grammars/build_ci.ps1` | 同上 |
| D4 | `grammars/build_ci.sh` | 同上 |
| D5 | `grammars/build_tree_sitter.sh` | 同上 |
| D6 | `grammars/clone_grammars.sh` | 同上 |
| D7 | `grammars/build_win_guide.sh` | 同上 |
| D8 | `grammars/*.so`（10 个，12MB） | 从未被加载，`.gitignore` 已忽略 `*.so` 但文件已 tracked |
| D9 | `grammars/vec0.dylib` | 将改为源码编进（见 3.4） |
| D10 | `grammars/` 目录本身 | 清空后删除目录 |

### 3.2 修改 CMakeLists.txt（删双分支，只留 FetchContent）

**删除**：`engine/CMakeLists.txt:82-133`（`USE_SYSTEM_DEPS=ON` 整个分支，含 npm 路径 + Swift 文法）

**保留**：`:134-210`（FetchContent 路径）作为唯一路径，不再有 `if/else`

**修改点**：

| # | 行号 | 改动 |
|---|------|------|
| M1 | `:82` | 删 `option(USE_SYSTEM_DEPS ...)` 和整个 `if(USE_SYSTEM_DEPS)` 分支 |
| M2 | `:180-196` | GRAMMAR_SOURCES 直接作为唯一来源（无需 else） |
| M3 | `:151-167` | sqlite3 amalgamation 下载段**全平台执行**（当前已在 else 内，需提到顶层） |
| M4 | `:156` | sqlite 下载 URL 加 `SQLITE_ENABLE_FTS5` 编译宏 |
| M5 | `:274` | 给 `sqlite3.c` 加 `-DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_RTREE` 编译选项 |
| M6 | `:245-247` | 删除 `swift_visitor.cpp` + `swift_translator.cpp`（文法已 omit，translator 是死代码） |

### 3.3 修改 server/build.rs（删双路径）

| # | 行号 | 改动 |
|---|------|------|
| M7 | `:114-121` | 删 `USE_SYSTEM_DEPS` 检测逻辑 |
| M8 | `:141-178` | 删 `else` 分支（系统依赖路径），只留 `:123-140`（FetchContent 路径） |

简化后 build.rs 链接段：
```rust
// FetchContent mode: all deps compiled into astgraph_engine.a
println!("cargo:rustc-link-lib=static=astgraph_engine");
println!("cargo:rustc-link-lib=static=tree-sitter");
// C++ runtime
match target_os.as_str() {
    "macos" => println!("cargo:rustc-link-lib=dylib=c++"),
    _ => println!("cargo:rustc-link-lib=dylib=stdc++"),
}
```

### 3.4 sqlite-vec 编进二进制（取消 load_extension）

**当前**：`engine_lifecycle.cpp:41` 用 `sqlite3_load_extension` 动态加载 `vec0.dylib/.dll/.so`

**改为**：静态注册

| # | 文件 | 改动 |
|---|------|------|
| M9 | `deps_versions.cmake` | 加 `set(SQLITE_VEC_VERSION "v0.1.7-alpha.2")` |
| M10 | `fetch_grammar.cmake` 或新 `fetch_sqlite_vec.cmake` | `FetchContent_Declare(sqlite_vec GIT_REPOSITORY https://github.com/asg017/sqlite-vec.git GIT_TAG ${SQLITE_VEC_VERSION})` |
| M11 | `CMakeLists.txt` ENGINE_SOURCES | 加 `${sqlite_vec_SOURCE_DIR}/sqlite-vec.c` |
| M12 | `CMakeLists.txt` | `set_source_files_properties(.../sqlite-vec.c PROPERTIES LANGUAGE C)` |
| M13 | `engine_lifecycle.cpp` | 删 `sqlite3_load_extension` 调用，改为：在 `sqlite3_open` 后调用 `sqlite3_auto_extension((void(*)(void))sqlite_vec_init);` |
| M14 | `engine_lifecycle.cpp` | 删 `vec_suffix`/`dlopen`/`LoadLibraryA` 三分支（不再需要） |

**静态注册伪代码**：
```cpp
// engine_lifecycle.cpp — 替代 sqlite3_load_extension
extern int sqlite_vec_init(sqlite3*, char**, const void*);
// 在 sqlite3_open 之后:
sqlite3_auto_extension((void(*)(void))sqlite_vec_init);
```

### 3.5 修改 Makefile

| # | 行号 | 改动 |
|---|------|------|
| M15 | `:42` | help 文本删 `make build-grammars` 行 |
| M16 | `:66-74` | 删 `build-grammars` target 和注释块 |
| M17 | `:124` | 删 `DYLD_LIBRARY_PATH=/opt/homebrew/opt/sqlite/lib:/opt/homebrew/opt/tree-sitter/lib`，改为空或删除（测试二进制已自包含） |
| M18 | `:247` | `distclean` 删 `rm -f $(GRAMMARS_DIR)/*.so`（目录已删） |

### 3.6 重写 README 构建章节

**删除**：
- `:36-37` 依赖表中的 `tree-sitter, sqlite, node` → 改为 `cmake, ninja, rust`
- `:289-294` npm install + build.sh 段落
- `:296-297` curl sqlite-vec 段落
- `:466-471` 重复的 npm + build.sh 段落

**替换为**：
```markdown
## Quick start

### Build from source (zero dependencies)

```bash
make build
```

That's it. CMake automatically downloads and compiles:
- tree-sitter core (v0.24.7) + 8 language grammars
- SQLite3 amalgamation (with FTS5)
- sqlite-vec (vector search)

All compiled into a single self-contained binary. No npm, no brew, no apt.

Prerequisites: cmake, clang (or gcc), rust.

### Install pre-built binary

```bash
curl -fsSL https://raw.githubusercontent.com/Timwood0x10/CodeScope/master/install.sh | bash
```
```

### 3.7 清理 .gitignore 与构建残留

| # | 改动 |
|---|------|
| M19 | `git rm -r --cached grammars/`（从 git 移除已 tracked 的 .so/.dylib/脚本） |
| M20 | `.gitignore` 加 `engine/build_asan/`（已有 `**/build_*/` 覆盖，确认即可） |
| M21 | 删 `BUILDING.windows.md` 中对 `grammars/build_ci.bat` 的引用 |

---

## 四、实施顺序（6 步，每步可独立验证）

### Step 1：删死代码（零风险）
- 删 `grammars/` 目录全部内容
- `git rm -r --cached grammars/`
- 验证：`make build` 仍成功（不依赖 grammars/）

### Step 2：CMakeLists 删双分支
- 删 `USE_SYSTEM_DEPS` option 和 ON 分支
- FetchContent 路径成为唯一路径
- 加 `-DSQLITE_ENABLE_FTS5` 编译宏
- 删 Swift translator
- 验证：`make build` 成功，无 npm 依赖

### Step 3：build.rs 删双路径
- 删 `USE_SYSTEM_DEPS` 检测和系统依赖分支
- 验证：`cargo build --release` 成功

### Step 4：sqlite-vec 编进二进制
- FetchContent 拉 sqlite-vec 源码
- 编进 astgraph_engine.a
- 改 engine_lifecycle.cpp 静态注册
- 验证：向量搜索工作，无 vec0.dylib

### Step 5：Makefile 清理
- 删 build-grammars target
- 删 DYLD_LIBRARY_PATH
- 验证：`make test` 成功

### Step 6：README 重写
- 删 npm/brew/sqlite-vec 步骤
- 替换为 Quick start
- 验证：`grep -rn "npm\|brew\|sqlite-vec\|build.sh" README.md` 零命中

---

## 五、验收标准

| # | 验收项 | 验证方法 |
|---|--------|---------|
| V1 | 干净 macOS 构建 | 新机器 `make build` 成功，无 brew/npm |
| V2 | 干净 Linux 构建 | 新机器 `make build` 成功，无 apt/npm |
| V3 | 零动态依赖 | `otool -L server/bin/codescope`（mac）或 `ldd`（linux）无 sqlite3/tree-sitter |
| V4 | 向量搜索无扩展文件 | 删 vec0.dylib 后 `codescope search_semantic` 仍工作 |
| V5 | FTS5 可用 | `codescope search` 全文检索返回结果 |
| V6 | README 无误导 | `grep -rn "npm\|brew\|sqlite-vec\|build.sh" README.md` 零命中 |
| V7 | grammars/ 不存在 | `ls grammars/` → No such file |
| V8 | 无 USE_SYSTEM_DEPS | `grep -rn "USE_SYSTEM_DEPS"` 零命中 |

---

## 六、权衡说明

### 做了什么
- **构建非封闭**：首次构建需联网（FetchContent 下载），CI cache 缓解。离线场景可 vendor（之前明确不做，保留为后续选项）
- **构建时间增加**：sqlite amalgamation + sqlite-vec 编进，首次多 1-2 分钟，cache 后无感
- **删了 Swift 支持**：文法与 core v0.24.7 不兼容，translator 是死代码。等文法升级后重新加回

### 不做什么
- **不保留 USE_SYSTEM_DEPS**：双路径是"乱"的根因。想要系统库的用户可以手动改 CMake，但默认必须 OFF
- **不 vendor 离线依赖**：50MB 入 git 不值得，CI cache 足够
- **不做 MSVC 支持**：build.rs 刻意 panic 非 gnu 是设计决定

---

## 七、新机器一键命令（目标态）

```bash
# macOS
xcode-select --install  # 装 clang
brew install cmake       # 或 xcode 自带
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh  # 装 rust
make build               # 一键构建

# Linux (Ubuntu/Debian)
sudo apt install cmake build-essential
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
make build

# Windows
# 见 BUILDING.windows.md（需 MinGW + rustup target gnu）
cargo build --release --target x86_64-pc-windows-gnu
```

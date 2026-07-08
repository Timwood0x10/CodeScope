# Code Review — 构建系统 + 当前状态复核（2026-07-08 第二轮）

> 依据 `CODE_REVIEW_STANDARD.md` 七域标准，聚焦本轮**构建系统重大变更**后的真实状态。
> 本文取代 `CODE_REVIEW_FINDINGS.md` 中关于 A2/A1「仍开放」的结论——**它们已被修复**。

---

## 0. 一句话结论

构建系统已落地我们上轮建议的 **FetchContent 零配置方案**（tree-sitter core + sqlite + 文法全自动拉取、版本钉死），**引擎在新机器上的编译便捷度大幅提升**。但有三处拖后腿：① **README 还在教旧的 `npm install` / 编 `.so` 手动步骤**（文档与代码打架）；② **Swift 文法被 omit 但 Swift 翻译器仍编译进二进制**（半残支持 = 陷阱）；③ **sqlite amalgamation 只开了 `LOAD_EXTENSION`，没显式开 `FTS5`**（需核实）。此前判的 🔴 A2/A1 已修复。

---

## 1. 已修复（纠正此前结论）

| 项 | 证据 | 说明 |
|---|---|---|
| 🔴 **A2 单句柄串行化** | `store_core.cpp:39` `sqlite3_config(SQLITE_CONFIG_SERIALIZED)` 在 `sqlite3_open` 之前调用；`:42-49` 妥善处理 `SQLITE_MISUSE`（已初始化场景） | 此前「数据竞争」头号缺陷已闭合。注意：必须在首次 `open` 前调用，代码位置正确。 |
| 🟠 **A1 `busy_timeout`** | `store_core.cpp:72` `PRAGMA busy_timeout=5000` | 并发写冲突不再立即 `SQLITE_BUSY`。✅ |

> 这两条是我之前在 `CODE_REVIEW_FINDINGS.md` 判为「仍开放」的，现在代码里已坐实修复。审查结论必须以当前代码为准。

---

## 2. 构建系统审查（FetchContent 落地）

### ✅ 做得好的

- **版本全部钉死**（`engine/cmake/deps_versions.cmake`）：`TS_CORE_VERSION v0.24.7`、`SQLITE_VERSION 3490100`、8 个文法各自 pin（连 Swift 都用 commit hash `db675450…`）。可复现、可升级、单一文件管理。
- **三平台统一 FetchContent**（`CMakeLists.txt:134-209`）：tree-sitter core、sqlite amalgamation、文法全部自动拉取并编译进 `astgraph_engine.a`。`USE_SYSTEM_DEPS` 默认 `OFF`（`:28`），仅在打包时可选开启。
- **sqlite 全平台自编译**（`CMakeLists.txt:151-174`）：不再依赖系统 sqlite，功能开关可控。
- **文法 FetchContent 正确**（`cmake/fetch_grammar.cmake`）：`fetch_grammar()` 宏封装，`GRAMMAR_SOURCE_DIR_*` 设为 `CACHE INTERNAL`，8 个文法（c/cpp/go/java/javascript/python/rust/typescript）齐全。

### 🔴/🟠 发现问题

#### F1 · Swift 文法被 omit，但 Swift 翻译器仍编译进二进制（🟠 一致性缺陷）
- `fetch_grammar.cmake:30-32` 与 `CMakeLists.txt:179` 注释：「Swift omitted: parser.c incompatible with tree-sitter core v0.24.7」。
- 但 `CMakeLists.txt:245,247,248` **仍在编译** `swift_translator.cpp` / `swift_visitor.cpp`（且 `swift_translator.cpp` 在 `:245` 和 `:247` **重复列了两次**）。`deps_versions.cmake:19` 也仍 pin 着 `TS_SWIFT_VERSION`。
- **后果**：新机器上索引 Swift 文件 → 翻译器运行但无对应 tree-sitter 语言 → 注册/解析失败（null 解引用或静默跳过）。即「编译了却用不了」的半残支持，是个陷阱。
- **修复**：要么 (a) 从 `ENGINE_SOURCES` 移除 Swift 翻译器（及 `deps_versions.cmake` 的 `TS_SWIFT_VERSION`），等文法兼容再加回；要么 (b) 修 Swift 文法与 core v0.24.7 的 ABI 兼容。当前状态两头不靠。

#### F2 · README 仍教旧的手动步骤，与零配置 CMake 打架（🟠 文档缺陷，直接拖垮「便捷」）
- `README.md:288-297`（`install.sh` 步骤 2–4）：`npm install -g tree-sitter-*`、`cd grammars && bash build.sh`、`curl … sqlite-vec install.sh`。
- `README.md:465-471`（「Build from source」§）：同样教 `npm install -g …` + 编 `.so`。
- `README.md:36-37` Prerequisites 仍列「tree-sitter, sqlite」/「libsqlite3-dev」为必需。
- **后果**：新机器用户照 README 做 → 浪费时间装 npm 包、跑已废弃的 `build.sh`（之前已改 no-op）、curl 一个过时步骤。轻则困惑，重则 `build.sh` 失败卡住。
- **修复**：把 README 两处构建说明改为「`make build` 即可，依赖由 CMake FetchContent 自动拉取」；Prerequisites 只保留 cmake/ninja/clang/rust。

#### F3 · sqlite amalgamation 未显式开 `SQLITE_ENABLE_FTS5`（🟠 需核实）
- `CMakeLists.txt:276` 只设 `SQLITE_ENABLE_LOAD_EXTENSION`，没设 `SQLITE_ENABLE_FTS5`。
- 但 `store_core.cpp:81-91` 预编译 `code_fts` / `fts_node_map`（FTS5 虚拟表），全文检索是核心能力。
- **风险**：若下载的 amalgamation 未编入 FTS5，`CREATE VIRTUAL TABLE … USING fts5` 运行时会报 `no such module: fts5`，全文检索静默失效。官方 amalgamation 通常默认含 FTS5，但**显式加 `-DSQLITE_ENABLE_FTS5` 是防御性必做**（无害、文档化意图、消除歧义）。
- **修复**：给 `SQLITE3_AMAL_SRC` 的 `set_source_files_properties` 加 `COMPILE_DEFINITIONS "SQLITE_ENABLE_FTS5"`（或在 `target_compile_definitions` 针对该源）。建议同时确认 `code_fts` 表能成功创建。

#### F4 · sqlite 下载失败无明确报错（🟡 健壮性）
- `CMakeLists.txt:154-167`：`file(DOWNLOAD)` 失败时 `SQLITE3_AMAL_SRC` 不存在，随后 `set_source_files_properties` + `add_library` 会抛出晦涩错误而非清晰提示。
- **修复**：`if(SQLITE_DL_ST EQUAL 0)` 的 `else` 分支加 `message(FATAL_ERROR "无法下载 sqlite amalgamation，请检查网络")`。

#### F5 · `SQLITE_VERSION` URL 硬编码年份（🟡）
- `CMakeLists.txt:156`：`https://www.sqlite.org/2025/sqlite-amalgamation-${SQLITE_VERSION}.zip`。未来换年份需改两处。低优。

---

## 3. 跨平台 / FFI（其余标准域）

- **B1 · FFI 裸 `handle()`**：仍系统性存在（`engine_ffi.cpp:25,473,615,622,657,672`；`engine_index.cpp:246,884,893`；`engine_queries.cpp:51,73,111…`；`linker.cpp:279`；`query/*`）。A2/A1 修复后并发安全性提升，但封装泄漏仍在，属中等规模重构，非紧急。
- **JSON 未转义**：此前定位的 3 处（`store.cpp:579`/`2039`/`2121-2122`）大概率仍开放，低优。
- **LSP Windows**：已实现 `CreateProcessW` 路径，但**无 Windows runner 验证**（见下 CI）。

---

## 4. 新机器编译便捷度评估

| 环节 | 状态 | 说明 |
|---|---|---|
| C++ 引擎（mac/Linux） | ✅ **已便捷** | 仅需 cmake+ninja+clang(C++23)+rust，`make build` 自动 FetchContent 拉齐依赖，无需 brew/apt/npm |
| C++ 引擎（Windows） | ⚠️ 手动可行 | 按 `BUILDING.windows.md`：`rustup target add x86_64-pc-windows-gnu` + `cargo build --target …gnu` 即可；vec0.dll 仍可选手动 |
| **README 指引** | ❌ **拖后腿** | 仍教 `npm install` / 编 `.so` / curl sqlite-vec，与零配置矛盾 → 用户多做且可能失败 |
| **Swift 支持** | ⚠️ **半残** | 翻译器编译进二进制但文法未拉取 → 新机器索引 Swift 必失败 |
| 向量/语义检索 | ⚠️ 降级可用 | 需 `vec0.dylib/.dll` 扩展文件（未随包、未自动构建）；无则仅语义检索降级，核心功能正常 |
| Windows CI | ❌ 红 | `build-windows.yml` 用默认 MSVC target 触发 `build.rs` gnu panic，需补 `--target x86_64-pc-windows-gnu` |
| 构建封闭性 | ⚠️ 非封闭 | 依设计需联网拉取（GitHub/sqlite.org）；CI cache 缓解，无网环境不可编 |

**直接回答「当前新机器编译是否便捷」**：
> 引擎本身的编译**已经便捷**了——只要走 `make build`、别看旧 README，一台只有基础工具链的机器能零配置编出引擎。但 **README 文档还在误导用户做多余/失效的手动步骤**，且 **Swift 是「编译了却用不了」的半残状态**，这两点不修，「便捷」的体感就会被打折。

---

## 5. 建议行动（按性价比）

1. **🔴 F2 修 README**（半小时，纯文档）：删掉 `npm install`/`build.sh`/`curl sqlite-vec` 步骤，Prerequisites 只留工具链。直接释放「零配置」价值。
2. **🟠 F1 解决 Swift 半残**：二选一——移除 Swift 翻译器（及 `deps_versions.cmake` 的 `TS_SWIFT_VERSION` 和 `:245/:247` 重复项），或修文法兼容。消除陷阱。
3. **🟠 F3 显式开 FTS5**：给 `sqlite3.c` 加 `SQLITE_ENABLE_FTS5`，并验证 `code_fts` 能建。
4. **🟡 F4/F5 健壮性**：下载失败 `FATAL_ERROR`、URL 年份参数化。
5. **🟠 修 Windows CI**（一行 `--target`）：让 Windows 真机路径被 CI 覆盖（也顺带给 LSP Windows 代码一个验证机会）。
6. **🟠 B1（长线）**：FFI 收口到 `GraphStore` 方法，消除裸 `handle()`。

---

*复核方式：静态读取 `CMakeLists.txt` / `cmake/deps_versions.cmake` / `cmake/fetch_grammar.cmake` / `store_core.cpp` / `README.md` 当前内容 + 全仓 `sqlite3_prepare_v2`/`jsonEscape`/`SQLITE_CONFIG_SERIALIZED` 计数。未跑实际编译（离线 / FetchContent 需联网）。*

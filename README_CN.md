# CodeScope — Project Truth Engine

**CodeScope 不解释代码，只验证代码。**

它把源码变成可验证的事实、可理解的模型、可检查的证据，让 AI 基于项目真相回答问题，而不是幻觉。

---

### CodeScope 不是什么

CodeScope **不是**代码解释器、语义分析器，也不是替代阅读代码的工具。它不理解 `Arc<T>` 是什么意思，不理解 `Rc<T>` 为什么不是线程安全的，也不理解 JWT 中间件怎么工作。**那是 AI 的事。**

### CodeScope 是什么

CodeScope 是一个 **Project Truth Engine**，回答一个问题：

> **"项目现在到底是什么状态？"**

不是"这段代码是什么意思"，而是"代码真的像你说的那样工作吗？"

### 真实数据

| 指标 | 值 |
|------|-----|
| 支持语言 | Rust, Go, C/C++, Python, Java, JS/TS |
| 索引速度 | 1-10s（100+ 文件） |
| 查询延迟 | 0.3-1.5 ms |
| Token 节省 | **98.5%**（26 万行 → 4 万 token） |
| MCP 工具 | 19 个（Locate / Understand / Verify / Index） |
| 架构 | Facts → Resolution → Models → Verification |

### 能验证什么

| AI 说 | CodeScope 验证 | 数据来源 |
|-------|---------------|---------|
| "登录模块支持 JWT" | JWT 库存在吗？login 调了 jwt 吗？有测试吗？ | `entity` + `relation` + `import` |
| "这个模块已完成" | 所有功能有调用者吗？覆盖率够吗？ | `relation` + `DeadCodeInspector` |
| "PR 修复了内存泄漏" | 有对应的 free 吗？测试覆盖了 error path 吗？ | `relation` + 测试文件检查 |
| "架构是 Controller→Service→Repository" | 代码真的遵守这个分层吗？ | `architecture_edge` |
| "模块支持 6 种语言" | adapter 真的存在吗？ | `entity` + `import` |

### 知识图谱（副产品）

CodeScope 把知识图谱作为验证管线的副产品来构建。从模块级开始，逐步扩展到项目级：

- **模块图**：模块内的实体、引用、导入关系
- **跨模块图**：模块间的调用边、依赖边
- **项目图**：架构层、工作流、能力

知识图谱不是产品，是支撑验证的基础设施。

**CodeScope 不解释代码，只验证代码。** 解释是 AI 的事，验证才是 CodeScope 的事。

## 快速开始

### 60 秒完成第一个索引

```bash
# 1. 一键构建（自动检测系统、安装依赖、编译）
bash <(curl -fsSL https://raw.githubusercontent.com/Timwood0x10/CodeScope/master/bootstrap.sh)

# 2. 索引项目
codescope cli index_project '{"project_path":"/path/to/your/project"}'

# 3. 查询
codescope cli get_graph_stats '{}'
# → {"total_nodes":12345,"total_edges":6789,"total_files":99}

# 4. 启动 MCP 服务（供 AI 客户端使用）
codescope
```

📖 详细指南见 [QUICK_START.md](QUICK_START.md)

### 前置依赖

| 平台 | 依赖 | 一键命令 |
|------|------|---------|
| **macOS** | Xcode CLT, cmake, Rust | `bash bootstrap.sh` |
| **Linux** | build-essential, cmake, Rust | `bash bootstrap.sh` |

> 预编译二进制可在 [Releases 页面](https://github.com/Timwood0x10/CodeScope/releases) 下载。

### 手动构建

```bash
# macOS:
brew install llvm@21 cmake pkg-config sqlite3 ladybug
cargo build --release

# Linux (Ubuntu):
sudo apt-get install -y build-essential cmake llvm-dev libclang-dev libsqlite3-dev
curl -fsSL https://install.ladybugdb.com | sh
cargo build --release
```

### 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `CODESCOPE_DB_PATH` | `.codescope/codescope.db` | SQLite 数据库路径 |
| `CODESCOPE_LSP` | 未设置 | LSP 服务器命令，用于类型增强 |

## 数据目录 `.codescope/`

CodeScope 在首次运行时自动在项目根目录创建 `.codescope/` 目录。  
所有持久化数据都存储在此——无需手动配置。

```
.codescope/
├── codescope.db       ← SQLite 数据库（WAL 模式）：所有事实、索引、图
├── skills.md          ← 快速入门指南和命令参考
└── *.log              ← 分析运行日志（含时间、CPU、内存数据）
```

数据库包含 11 张表：

| 表 | 说明 |
|------|-------------|
| `modules` | 目录模块树 |
| `symbols` | 符号声明（含 `role` 字段） |
| `entry_points` | 入口点（main/probe/initcall） |
| `call_edges` | 函数调用图边 |
| `dependency_edges` | 模块依赖边 |
| `metrics` | 复杂度指标（圈复杂度、认知复杂度等） |
| `search_index` | FTS5 全文搜索索引 |
| `embeddings` | vec0 向量嵌入 |
| `symbol_status` | 逐符号分析进度标志 |
| `index_tasks` | 后台任务跟踪 |
| `file_scan_state` | 文件修改时间戳 |

> **提示**：数据库是可移植的——将 `.codescope/` 随项目一起复制，即可在其他机器上复用分析结果。

## 性能

基准测试在 **Apple M3 Max（36 GB 内存）** 上执行。

### 微基准测试 (test_bench)

| 指标 | 数值 |
|------|------|
| 引擎初始化 | **14.6 ms** |
| 索引吞吐量 | **1,533 KB/s** |
| 符号定义查询 | **0.01–0.03 ms** |
| 调用者/被调用者查询 | **0.01–0.02 ms** |
| 9 次查询（总计） | **0.17 ms** |

### 全量内核索引 (codebase-memory-mcp)

| 指标 | Linux 内核 v6.x (89,465 文件) |
|------|:---------------------------:|
| 总节点数 | **4,877,492** |
| 总边数 | **9,326,238** |
| 数据库大小 | **7.06 GB** |
| 缓存大小 | **6.7 GB** |
| 索引耗时 | **183 s（约 3 分钟）** |
| 峰值内存 | **11.6 GB** |
| 并行度 | **14 workers** |
| 文件处理速度 | **489 files/s** |
| 边生成速度 | **50,966 edges/s** |

### 已知瓶颈（知识图谱查询）

当前 MCP 知识图谱服务在处理 **30-50 万节点以上** 的大型项目时，模糊搜索（`CONTAINS`、BM25 全文检索、正则匹配）会出现 **30 秒超时**。

| 项目 | 节点数 | 精确路径查询 | 模糊搜索 |
|------|--------|:----------:|:--------:|
| 中小型项目（<5 万节点） | eg. goagent(2.3万) | ✅ &lt;10ms | ✅ 流畅 |
| 大型项目（5-30 万节点） | eg. zigcode(33万) | ✅ &lt;10ms | ⚠️ 可能超时 |
| 超大型项目（>50 万节点） | eg. JDK(136万) | ✅ 精确匹配可用 | ❌ 超时 |

> **原因**：服务端对全节点集的文本扫描（`CONTAINS`、`name_pattern` 正则）需要遍历百万级节点，超过了 30 秒超时限制。精确路径匹配（`ENDS WITH`）利用索引可正常工作。
>
> **应对**：计划增加自定义排除路径参数，允许索引时主动跳过 `test/`、`doc/` 等大型非核心目录，将有效节点数控制在 30 万以内。

### Token 节省

| 场景 | 原始源码 | CodeScope | 节省 |
|------|:-------:|:---------:|:----:|
| 查找函数定义 | ~2,265 tokens | ~21 tokens | **99.1%** |
| 追踪函数调用者 | ~2,000 tokens | ~18 tokens | **99.1%** |
| 项目架构概览 | ~1,875 tokens | ~32 tokens | **98.3%** |
| USB 子系统概览 | ~24,000 tokens | ~250 tokens | **99.0%** |
| 调度器分析 | ~15,000 tokens | ~180 tokens | **98.8%** |
| **平均** | **~7,416 tokens** | **~81 tokens** | **98.9%** |

## License

Apache 2.0

***

## 运行时日志

所有基准测试在 **Apple M3 Max（36 GB 内存）** 上执行。\
原始输出日志位于 [`runtimelog/`](runtimelog/)：

| 日志 | 大小 | 内容 |
|------|------|------|
| `scan_goagent.log` | 127 KB | Go agent 工具调度分析 |
| `scan_linux_kernel.log` | 52 KB | Linux kernel/ 核心扫描（40,335 符号） |
| `scan_fs_io.log` | 14 KB | VFS + 页缓存 + 预读分析 |
| `scan_linux_kernel_full.log` | 12 KB | 内核子目录全量扫描汇总 |
| `scan_usb_raw.log` | 11 KB | USB 驱动子系统原始输出 |
| `scan_stub_full.log` | 2.2 KB | 空实现检测（Fast + AST）测试 |
| `scan_linux_full.log` | 1.7 KB | 全量内核扫描尝试 |
| `scan_multilang.log` | 1.1 KB | 多语言架构扫描 |
| `scan_hid.log` | 526 B | USB HID 子系统扫描 |
| `scan_linux_scheduler.log` | 12.8 KB | 进程调度 + 父子进程资源分析 |
| `scan_usb_hid_analysis.log` | 12.8 KB | USB HID 设备识别深度分析 |
| `performance_benchmark.log` | 5.5 KB | 全量性能基准报告 |

---

## 工具使用指南

每个 MCP 工具有其适用的场景和副作用（主要是 Token 消耗）。以下指南帮助你在正确的场合选择合适的工具。

> **工具总数：32 个**。一些旧工具标记为 `[DEPRECATED]`，建议使用推荐替代。

### 核心查询类

| 工具 | 适用场景 | Token 消耗 |
|------|---------|:----------:|
| `project_overview` | **首选**——新接手项目的第一步总览（语言、模块、符号、入口点） | **~71** |
| `get_graph_stats` | 快速了解项目规模（文件数、节点数、边数） | **~18** |
| `get_module_tree` | 了解项目的目录/模块层级结构 | **~4** |
| `get_entry_points` | 找项目入口点（main/init/setup/run/handler） | **~5** |
| `get_routes` | 获取项目注册的 HTTP 路由（支持 Gin/Echo/Chi/net/http） | **~50** |
| `get_type_info` | 查询类型定义（struct/enum/trait）及其引用数 | **~50** |

### 符号查询类

| 工具 | 适用场景 | Token 消耗 | 说明 |
|------|---------|:----------:|------|
| `find_symbol` | **推荐**——按精确名称查找符号（类型、文件、行列） | **~30** |  |
| `find_definition` | `[DEPRECATED]` 查找符号定义位置 | **~20** | 改用 `find_symbol` |
| `find_references` | 搜索符号被哪些位置引用 | **~30** |  |
| `explain_symbol` | 获取符号的完整信息：定义、调用者、被调用者、依赖 | **~50-200** | 一键全方位理解一个函数 |

### 调用图查询类

| 工具 | 适用场景 | Token 消耗 | 副作用 |
|------|---------|:----------:|--------|
| `find_callers` | 调查函数被谁调用——定位 bug 影响范围 | **~10-50** | 依赖 CALLS 边构建 |
| `find_callees` | 调查函数调用了什么——理解函数行为 | **~10-50** | 同上 |
| `codescope_trace` | 交互式递归展开调用者/被调用者（方向+深度控制） | **~50-200** | 深度大时输出膨胀 |
| `trace_flow` | 从函数开始递归追踪执行流（caller→callee 链） | **~50-200** | 同上 |
| `shortest_path` | 两点之间的最短调用路径 | **~50-100** | BFS 近似结果 |
| `connected_components` | 调用图中的连通分量——找独立的功能模块 | **~50** | |

### 搜索类

| 工具 | 适用场景 | Token 消耗 |
|------|---------|:----------:|
| `search` | **推荐**——统一代码搜索（FTS5 + 语义搜索自动选择） | **~300-1000** |
| `search_code` | `[DEPRECATED]` 旧版 FTS 搜索，推荐改用 `search` | **~300-1000** |

### 验证类（Verification Layer）

| 工具 | 适用场景 | Token 消耗 |
|------|---------|:----------:|
| `verify_integrity` | 检查 README 承诺的功能是否在代码中真实存在 | **~100** |
| `verify_claim` | 验证单条声明（capability_exists / contract_holds / architecture_follows） | **~100** |
| `verify_summary` | 解析自然语言摘要并逐条验证所有声明 | **~200-500** |
| `verify_review` | 验证 Code Review 评论中的声明 | **~200-500** |
| `verify_reality` | 验证 AI 对项目的单条陈述是否被代码证据支持 | **~200-500** |

### 漂移检测类（Drift Detection）

| 工具 | 适用场景 | Token 消耗 |
|------|---------|:----------:|
| `detect_drift` | 扫描所有声明的能力与契约，发现文档与代码之间的偏移 | **~200** |
| `detect_documentation_drift` | 检测 README 声明的语言支持与实际代码的差异 | **~150** |
| `detect_capability_drift` | 检测声明的能力是否有实际实现且有调用者 | **~150** |
| `detect_architecture_drift` | 检测调用边中的架构层违规（如 Repository 调 Controller） | **~150** |

### 变更影响分析

| 工具 | 适用场景 | Token 消耗 |
|------|---------|:----------:|
| `detect_changes` | 修改代码后分析影响范围——返回直接/间接调用者 | **~100-500** |
| `explain_module` | 构建模块知识卡：实体、能力、契约、完整性评分 | **~50-200** |

### 辅助工具

| 工具 | 适用场景 | Token 消耗 |
|------|---------|:----------:|
| `index_project` | 索引整个项目目录（解析+IR+图构建） | **N/A** |
| `index_file` | 索引单个源文件 | **N/A** |
| `count_tokens` | 估算文本的 token 数（DeepSeek 公式） | **~10** |

### 不再存在的工具

以下工具曾在旧版 README 中出现，但**实际代码中不存在**，不要使用：

- ❌ `get_hotspots` — 未实现
- ❌ `get_communities` — 社区检测未接入 MCP
- ❌ `graph_query` — 自定义 Cypher 查询未实现
- ❌ `locate_code` — 未实现
- ❌ `get_project_info` — 未实现
- ❌ `enhance_project` / `codescope_build_context` / `codescope_capabilities` — 已移除

### 总结选择策略

```
新接手项目     → project_overview (~71 tok)
查结构         → get_module_tree (~4 tok)
找入口点       → get_entry_points (~5 tok)
搜代码         → search (~300-1000 tok)
查调用链       → find_callers / find_callees (~10-50 tok)
全量理解函数   → explain_symbol (~50-200 tok)
查 HTTP 路由   → get_routes (~50 tok)
查类型定义     → get_type_info (~50 tok)
验证声明       → verify_claim (~100 tok)
查文档偏移     → detect_documentation_drift (~150 tok)
查变更影响     → detect_changes (~100-500 tok)
```


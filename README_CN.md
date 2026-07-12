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

### 前置依赖

- Rust 2024 Edition + 1.85+（`cargo`）
- CMake 3.30+，C++23 编译器（Clang 17+）

> 所有第三方依赖（tree-sitter 核心、SQLite3、sqlite-vec、8 种语言文法）均通过 CMake FetchContent 自动下载并编译进二进制——**零外部依赖**，无需 npm、brew、apt 安装任何包。

### 构建与运行

```bash
# 构建全部 — tree-sitter、SQLite、sqlite-vec、文法
# 均通过 CMake FetchContent 自动下载编译进二进制（零外部依赖）
make build

# 启动 MCP 服务
cargo run --bin codescope
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

### 核心查询类

| 工具 | 适用场景 | 不适用场景 | Token 消耗 | 副作用 |
|------|---------|-----------|-----------|--------|
| `get_graph_stats` | 快速了解项目规模（文件数、节点数、边数） | 不需要知道具体符号时 | **~18** | 无 |
| `get_project_info` | 查看项目元信息（许可证、主语言、依赖数） | 不需要细节时 | **~44** | 无 |
| `get_module_tree` | 了解项目的目录/模块结构 | 项目结构已经清晰时 | **~4** | 无 |
| `project_overview` | 新接手项目的第一步总览 | 只需要统计数字时 | **~71** | 无 |

### 符号查询类

| 工具 | 适用场景 | 不适用场景 | Token 消耗 | 副作用 |
|------|---------|-----------|-----------|--------|
| `find_definition` | 定位符号的定义位置 | 需要查看所有引用时 | **~20** | 无 |
| `find_references` | 搜索符号被哪些地方引用 | 只想知道定义时 | **~30** | 无 |
| `find_symbol` | 模糊匹配符号名称 | 知道精确位置时 | **~30** | 无 |
| `locate_code` | 获取符号附近的代码上下文（含行号） | 只需要文件名时 | **~50-300** | 包含相邻行，量较大 |

### 调用图查询类

| 工具 | 适用场景 | 不适用场景 | Token 消耗 | 副作用 |
|------|---------|-----------|-----------|--------|
| `get_callers` / `find_callers` | 调查函数被谁调用——定位 bug 影响范围 | **CALLS 边未构建时 caller_count=0** | **~10-50** | 依赖 `buildGraph(true)` |
| `get_callees` / `find_callees` | 调查函数调用了什么——理解函数行为 | 不需要递归展开时 | **~10-50** | 同上 |
| `codescope_trace` | 两点之间的最短调用路径——追 data flow | 只需要直接调用者时 | **~50-200** | 路径过长时输出膨胀 |
| `get_hotspots` | 找项目中**最热门的函数**（被调最多） | 项目 <100 个函数、热点不明显 | **~500** | caller_count=0 时说明调用边未构建 |

### 搜索类

| 工具 | 适用场景 | 不适用场景 | Token 消耗 | 副作用 |
|------|---------|-----------|-----------|--------|
| `search` | 按名称/关键词搜索代码**（推荐首选）** | 需要语法精确匹配时 | **~300-1000** | 结果较多时会增加 token |
| `search_code` | 旧版 FTS 搜索，推荐改用 `search` | 已迁移到 `search` | **~300-1000** | 已废弃 |
| `graph_query` | 自定义模式匹配，如 `MATCH (Function)-[Calls]->(Function)` | 标准调用链已覆盖时 | **~50-500** | DSL 语法错误会返回空结果 |

### 社区检测（特殊工具 ⚠️）

> **社区检测通过 Label Propagation 算法将代码图中的节点按关系紧密程度分组。适用于需要理解代码模块边界、检测架构违规的场景，但 Token 消耗可能很大，使用前请确认参数。**

| 场景 | 推荐用法 | 说明 |
|------|---------|------|
| **接手 legacy 项目** | ✅ `get_communities(max_communities=20)` | 快速了解代码模块划分 |
| **架构逆向** | ✅ `get_communities(max_members=5, max_communities=50)` | 看社区之间的边，找模块间依赖 |
| **检测架构违规** | ✅ `include_members=true` 查看社区成员 | 不该在一起的代码出现在同一社区需关注 |
| **monorepo 模块发现** | ✅ 默认参数即可 | 区分各子项目边界 |
| **小型项目 (<500 节点)** | ✅ 适用 | 社区数少，输出可控 |
| **中型项目 (500-10K 节点)** | ✅ 推荐加 `max_communities=20` | 默认 20 社区约 **1K-50K tokens** |
| **大型项目 (>10K 节点)** | ⚠️ **谨慎使用，必须加 `max_communities`** | 123K 节点示例：5社区×10成员 = **199K tokens** 🔴 |

**`get_communities` 的副作用：**

1. **Token 爆炸风险**：123K 节点的项目，即使限制 `max_members=5, max_communities=10`，因 `label` 字段包含完整路径，仍可达 **200K tokens**。**默认参数 (max_members=10, max_communities=20) 约 1K-50K tokens**。
2. **耗时**：社区检测需要全图 Label Propagation 算法，大型项目耗时数百 ms。
3. **信息密度**：对于目录结构清晰的项目，`get_module_tree`（4 tokens）比社区检测（200K tokens）更高效。
4. **屏蔽策略**：
   - `max_communities` 优先调低（10-20），限制社区总数
   - `include_members=false`（默认），只返回摘要，需成员详情时再开启
   - 先用 `get_module_tree` 了解结构，社区检测仅作补充

### 热点分析

| 场景 | 推荐用法 | 说明 |
|------|---------|------|
| **性能优化** | ✅ `get_hotspots(top_n=10)` | 找被调用最多的函数，优先优化 |
| **代码审查** | ✅ `get_hotspots(top_n=20)` | 高复杂度 + 高调用数的函数需要关注 |
| **重构决策** | ✅ 配合 `get_complexity` 交叉分析 | 高复杂度 + 高热点的函数最值得重构 |

**前提条件**：`get_hotspots` 的 `caller_count` 依赖调用边构建。如果索引时 `buildGraph(project_id, false)`，所有 caller_count 为 0。确认方法：检查索引输出中是否有 `calls=XXms`（非零）。

### 变更影响分析

| 工具 | 适用场景 | Token 消耗 |
|------|---------|-----------|
| `detect_changes` | 修改代码后分析影响范围——返回直接/间接调用者 | **~100-500** |

### 增强工具（Phase B）

| 工具 | 适用场景 | 说明 |
|------|---------|------|
| `enhance_project` | 触发后台全量分析（调用图 + 复杂度 + 向量索引） | 异步运行，`get_enhancement_status` 查看进度 |
| `codescope_build_context` | **PRIMARY**：AI 问答上下文构建 | 自动判断需要什么信息 |
| `codescope_capabilities` | 检查当前项目各功能的就绪状态 | 快速诊断"为什么查不到数据" |

### 总结选择策略

```
新接手项目 → project_overview (71 tok) + get_module_tree (4 tok)
找入口点   → get_entry_points (5 tok)
查热点     → get_hotspots (500 tok)
搜代码     → search (300-1000 tok)
查调用链   → find_callers / find_callees (10-50 tok)
做架构分析  → get_module_tree (4 tok) + 可选 get_communities (1K-200K tok)
查变更影响  → detect_changes (100-500 tok)
```


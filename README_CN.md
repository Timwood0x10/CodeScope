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
| 支持语言 | 8 种（Rust, Go, C/C++, Python, Java, JS/TS） |
| 索引速度 | 1-10s（100+ 文件） |
| 查询延迟 | 0.3-1.5 ms |
| Token 节省 | **98.9%**（26 万行 → 4 万 token） |
| MCP 工具 | 37 个（Locate / Understand / Verify / Index） |
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

CodeScope 把知识图谱作为验证管线的副产品来构建。它学到的不是「代码文本」，而是*结构化的「这个项目怎么组织、哪重要、哪冗余、承诺了啥」*——四层叠加：

| 层 | 表 | 告诉你什么 |
|----|----|-----------|
| **调用图**（谁调谁） | `relation`（type=1）、`architecture_edge` | 跨模块调用依赖；驱动 `detect_architecture_drift` |
| **模块健康度**（谁重要/谁有死代码） | `module_summary` | 每模块 `incoming_count`/`outgoing_count`/`dead_entities`/`utilization`/`confidence`；驱动 `explain_module` |
| **模块依赖**（改了影响谁） | `architecture_edge`（边权=调用次数）、`module_edge` | 「改模块 A → 这些模块依赖它」 |
| **文档能力**（声称能干嘛） | `capability` + `document` | 从 README 提取的能力声明；驱动 `detect_capability_drift` / `verify_claim` |

知识图谱不是产品，是支撑验证的基础设施。

#### `module_summary.role`：多信号融合分类器（v0.2.1）

`role` 列由**多信号融合 CASE** 分类器自动填充（`engine/src/model/state_builder.cpp`），不是单源启发式。它把调用图度数与调用图给不了的两类信号融合：

| 信号 | 来源 | 增量信息 |
|------|------|----------|
| `pub_count` | `entity.visibility=1`（各语言 Visitor 域 pub/public/export） | 区分「对外接口层」vs「内部实现层」——调用图度数推不出 |
| `entry_reachable` | `graph_nodes.is_entry_point`（main/init/setup/run/handler） | 该模块是否含入口层 |

规则按**优先级**匹配（命中即停）：

| 优先级 | Role | 规则（多信号） |
|--------|------|----------------|
| 1 | `test` | 模块名含 `test`/`tests`/`_test`/`mod tests` |
| 2 | `api` | `pub_count > 0 AND incoming ≥ 2×outgoing AND incoming ≥ 3 AND utilization ≥ 0.1` |
| 3 | `entry` | `entry_reachable > 0` |
| 4 | `core` | `incoming ≥ 10 AND outgoing ≤ incoming×1.0 AND utilization ≥ 0.05 AND pub_count > 0` |
| 5 | `utility` | `outgoing ≤ 5 AND pub_count > 0 AND utilization ≥ 0.05` |
| 6 | `business` | `pub_count > 0 AND incoming ≥ 10`（实现层——被多模块调且自己也调多；outgoing 偏高不命中 core/api） |
| 7 | `dead` | `incoming=0 AND outgoing=0`，或 `dead_entities = total` |
| 8 | `infra` | 真兜底——没命中任何语义规则 |

请把 `role` 当融合后的线索，不当判决。阈值是 `state_builder.h` 里的 `constexpr`——若你项目的 role 分布看着不对（如 `infra > 30%` 说明阈值过严），凭 `bun` 调参。完整设计 + v0.2.1 阈值重调记录见 `docs/dev_plans/role_classifier_plan.md`。

#### 知识图谱里落了什么（memscope-rs，215 文件）

| 表 | 行数 | 含义 |
|----|-----:|------|
| `entity` | 4,310 | 细粒度代码实体（函数、类型、变量） |
| `relation` | 726 | 实体间关系（type 3 = 包含/定义） |
| `architecture_edge` | 3,351 | 模块/目录级架构依赖边（边权 = 调用次数） |
| `module_edge` | 11 | 跨模块依赖边 |
| `capability` | 3 | 从 README 提取的能力声明 |
| `document` | 1 | README 文档记录 |
| `module_summary` | 42 | 每模块知识卡片 |

能了解到啥：

- **架构依赖**——`architecture_edge` 告诉你哪些模块依赖哪些（如 `analysis/heap_scanner → unsafe_inference`，边权=调用次数）。驱动 `detect_architecture_drift`。
- **能力声明**——`capability` + `document` 把 README 的「能干什么」结构化。驱动 `detect_capability_drift` / `verify_claim(capability_exists)`。
- **模块摘要**——42 个模块的知识卡片，由 `explain_module` 浮出。

#### 直接查询入口（v0.2.1）

过去知识图谱是**隐式**的——`graph_query` 走的是*调用*图（`graph_nodes` / `graph_edges`），不是知识层表（`entity` / `relation` / `architecture_edge`）。你只能通过 `explain_module`、`detect_capability_drift`、`get_module_tree` 间接受益。

v0.2.1 新增 `engine_get_knowledge_graph(project_id, table, limit)` + MCP 工具 `get_knowledge_graph`，现在可**直接浏览**任意知识层表：

```jsonc
get_knowledge_graph {"table":"architecture_edge","limit":5}
// → {"table":"architecture_edge","rows":[{"id":1,"layer_lower":"analysis/heap_scanner","layer_upper":"unsafe_inference","entity_id":42}],"total":3351,"truncated":false}

get_knowledge_graph {"table":"capability","limit":10}
// → {"table":"capability","rows":[{"id":1,"name":"borrow_analysis","summary":"scope-aware borrow checking"}],"total":3,"truncated":false}
```

支持表：`entity`、`relation`、`architecture_edge`、`module_edge`、`capability`、`document`、`module_summary`。块级 FFI——单次调用返回整个结果集，绝不一条边一次调用。

**CodeScope 不解释代码，只验证代码。** 解释是 AI 的事，验证才是 CodeScope 的事。

## 快速开始

### 60 秒完成第一个索引

```bash
# 1. 一键构建（自动检测系统、安装依赖、编译）
bash <(curl -fsSL https://raw.githubusercontent.com/Timwood0x10/CodeScope/main/bootstrap.sh)

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

**Linux / macOS 一键安装**：

```bash
curl -fsSL https://raw.githubusercontent.com/Timwood0x10/CodeScope/master/install.sh | bash
```

安装后 `~/.codescope/bin/` 下包含：

| 文件 | 用途 |
|------|------|
| `codescope` | 主二进制（CLI + MCP server） |
| `codescope-parallel.sh` | **大型项目加速索引脚本**——多进程模块级调度，动态 worker 回收，每文件 quarantine，自动合并 DB。AI 决定要不要加速时调这个脚本即可，详见下方[并行索引脚本](#并行索引脚本大型项目推荐)章节 |

加入 PATH 后即可使用：

```bash
export PATH="$PATH:$HOME/.codescope/bin"

# 中小项目直接索引
codescope cli index_project '{"project_path":"/path/to/project"}'

# 大型项目（数千~数万文件）用并行脚本加速
codescope-parallel.sh /path/to/large/project
```

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
| `GRAMMARS_DIR` | `grammars/` | Grammar .so 文件目录 |
| `CODESCOPE_LSP` | 未设置 | LSP 服务器命令，用于类型增强（如 `pylsp`） |
| `CODESCOPE_INDEX_MODE` | `standard` | 索引模式：`fast` / `standard` / `strict` |
| `CODESCOPE_EXCLUDE_PATHS` | 未设置 | 逗号分隔的排除 glob 模式（如 `test/*,docs/*`） |
| `CODESCOPE_MMAP_SIZE` | `268435456` (256 MB) | SQLite `mmap_size` pragma 值（字节） |
| `CODESCOPE_WORKERS` | `4` | 并行索引 worker 数量 |
| `CODESCOPE_MAX_FILE_SIZE` | 未设置 | 索引的最大源文件大小（字节）；超过则跳过 |
| `CODESCOPE_WORKER_TIMEOUT` | `300` | Worker 子进程超时（秒） |
| `CODESCOPE_VERBOSE` | `0` | 设为 `1` 启用详细日志 |
| `CODESCOPE_EXPLAIN` | 未设置 | 设为 `1` 打印图查询的 SQL `EXPLAIN QUERY PLAN` |

## 数据目录 `.codescope/`

CodeScope 在首次运行时自动在项目根目录创建 `.codescope/` 目录。  
所有持久化数据都存储在此——无需手动配置。

```
.codescope/
├── codescope.db       ← SQLite 数据库（WAL 模式）：所有事实、索引、图
├── skills.md          ← 快速入门指南和命令参考
└── *.log              ← 分析运行日志（含时间、CPU、内存数据）
```

数据库包含 40 张表，按用途分组（见 `engine/src/store/store_schema.cpp`）：

| 类别 | 表 | 用途 |
|------|------|------|
| 核心 / 项目 | `projects`, `project_readiness`, `files`, `modules`, `entry_points`, `index_tasks`, `file_scan_state` | 项目元数据、文件跟踪、索引阶段进度 |
| 图 | `graph_nodes`, `graph_edges`, `entity`, `relation`, `semantic_records`, `adjacency`, `adjacency_rev`, `module_edge`, `module_summary` | 代码图节点/边、CSR BLOB 邻接表、跨模块边 |
| 搜索 | `code_fts` (FTS5), `name_trgm` (FTS5 trigram), `fts_node_map`, `node_vectors` | 全文 + trigram + n-gram 向量搜索 |
| 事实 / 解析 | `reference`, `scope`, `import`, `type_info`, `type_ref`, `route` | 调用事实、作用域树、导入、类型定义、HTTP 路由 |
| 知识 + 证据 | `capability`, `contract`, `claim`, `evidence`, `evidence_fact`, `finding`, `document` | 验证管线：声明、证据链、发现 |
| 模型状态 | `workflow`, `workflow_step`, `architecture_edge`, `capability_state`, `workflow_state`, `architecture_state` | 工作流、架构层、状态缓存 |
| LadybugDB 同步 | `lbug_sync_state` | 到 LadybugDB 图存储的增量同步进度 |

> **提示**：数据库是可移植的——将 `.codescope/` 随项目一起复制，即可在其他机器上复用分析结果。

## 性能

### 真实项目索引实测（v0.2.1，Apple M3 Max）

| 项目 | 文件 | 节点 | 索引耗时 | 峰值 RSS |
|------|----:|----:|--------:|---------:|
| **memscope-rs**（Rust） | 215 | 4,344 | ~2 s | ~200 MB |
| **CodeScope**（自身，C++/Rust） | 168 | 1,001 | 1.3 s | ~150 MB |
| **ARES_POLIS** | 105 | 1,531 | ~2 s | ~180 MB |
| **rustc**（Rust 编译器，单体库） | 6,029 | 81,033 | 81 s | 5.9 GB |

**结论：**

- **中小项目（<300 文件）：** 亚秒~2 秒索引，~200 MB RSS——体验极佳，日常开发速度。
- **超大单体库（数万文件）：** 能用但需注意资源——rustc 耗 81 秒 + 5.9 GB 峰值内存。一次性索引可接受,需留意内存。
- **查询延迟（stdio MCP 模式，含进程启动）：** 单次调用 ~60 ms；常驻 server 模式更低。

基准测试在 **Apple M3 Max（36 GB 内存）** 上执行。

### 微基准测试 (test_bench)

| 指标 | 数值 |
|------|------|
| 引擎初始化 | **14.6 ms** |
| 索引吞吐量 | **1,533 KB/s** |
| 符号定义查询 | **0.01–0.03 ms** |
| 调用者/被调用者查询 | **0.01–0.02 ms** |
| 9 次查询（总计） | **0.17 ms** |

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

## 工具使用指南

每个 MCP 工具有其适用的场景和副作用（主要是 Token 消耗）。以下指南帮助你在正确的场合选择合适的工具。

> **工具总数：37 个**。一些旧工具标记为 `[DEPRECATED]`，建议使用推荐替代。

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
| `force_index_files` | **强制索引**——绕过默认跳过规则（`test/`、`docs/`、`vendored/`、`node_modules/`、`.gitignore` 等）索引指定文件/目录。用户说"去吧 xxx/yyy 给我索引了吧"时使用 | **N/A** |
| `count_tokens` | 估算文本的 token 数（DeepSeek 公式） | **~10** |

#### `force_index_files` — 用户自定义增量索引

当默认 `FilterPolicy` 把某些目录（测试夹具、vendored 依赖、生成代码、示例）整批跳过，而用户又想把这些路径拉进来索引时，用 `force_index_files`。它**绕过**默认 skip 规则，但仍然尊重：

- 文件大小上限（`CODESCOPE_MAX_FILE_SIZE`，默认 5 MB）
- 语言可识别性（`detectLanguage` 必须返回非 null）
- 可选语言白名单（`language_filter`）

**MCP 调用**：

```json
{
  "paths": ["/abs/path/to/dir/or/file", "/another/path"],
  "language_filter": "java,python"
}
```

- `paths`：绝对路径数组，目录会递归展开
- `language_filter`：可选，逗号分隔语言白名单；空 = 全部可识别语言

**CLI 调用**：

```bash
codescope force-index [--lang java,python] [--db /path/to/codescope.db] /path/to/xxx /path/to/yyy
```

返回 `engine_index_files` 的 JSON（`files_indexed`/`nodes`/`edges`/`errors`），并附 `skipped_files`/`skipped_dirs`/`paths_requested` 统计。

#### 并行索引脚本（大型项目推荐）

对于**大型项目**（数千~数万源文件，如 Linux kernel、rustc 这种），单进程多线程索引可能受限于 RSS 峰值和崩溃隔离。`codescope-parallel.sh` 提供**多进程模块级调度**：

- 按模块（顶级目录）拆分，每个模块起独立 `codescope worker` 子进程
- 内存/崩溃隔离：一个模块崩溃不会带垮整个索引
- 动态 worker 回收：模块完成后把 worker 重新分配给剩余模块
- 每文件 quarantine：崩溃模块用二分搜索定位 crashing 文件，跳过后重试
- 最后合并所有模块 DB 到单一项目 DB

**通用入口**——用户或 AI 只需传一个目录，脚本自动探测二进制、grammars 目录、默认输出路径：

```bash
# 默认配置（8 workers），输出到 <dir>/.codescope/codescope.db
./codescope-parallel.sh /path/to/large/project

# 大项目用 16 workers
CODESCOPE_WORKERS=16 ./codescope-parallel.sh /path/to/large/project

# 自定义输出 DB
./codescope-parallel.sh /path/to/large/project /tmp/my.db

# 帮助
./codescope-parallel.sh -h
```

环境变量（全部可选）：

| 变量 | 默认 | 说明 |
|------|------|------|
| `CODESCOPE` | 自动探测 | codescope 二进制路径（按 `bin/codescope`、`target/release/codescope`、`./codescope`、`PATH` 顺序） |
| `GRAMMARS_DIR` | 自动探测 | tree-sitter grammars 目录 |
| `CODESCOPE_WORKERS` | `8` | 总 worker 数 |
| `CODESCOPE_PARALLEL` | `= CODESCOPE_WORKERS` | 最大并发模块数 |

**何时用脚本 vs 直接 `index_project`**：

- 中小项目（<300 文件）：直接 `index_project` 即可，亚秒~2 秒搞定
- 大型项目（数千文件以上）：用 `codescope-parallel.sh`，多进程隔离 + 动态调度更稳


### 为什么 Java 是（唯一的）例外 —— 一段吐槽

CodeScope 对 `test/`、`tests/`、`docs/`、`examples/`、`samples/`、`bench/`、`vendor/`…… 这类目录在路径**任意深度**都跳过。这对任何正常的项目布局都是正确行为：Cargo workspace 会嵌 `crates/<name>/tests/`，Lerna monorepo 会嵌 `packages/<name>/test/`，Gradle 多模块构建会嵌 `subprojects/<name>/src/test/`，用户都期望这些被跳过 —— 它们不是分析目标，索引它们只会让节点数膨胀 3-5x 的噪声。

然后**Java**来了。Java 凭它无尽的智慧，决定 `org/springframework/samples/petclinic` 是一个合法的包名 —— `samples` 在这里是一个包组件，**不是** docs 文件夹。`src/test/java/...` 是 Maven 的标准测试源根，但 `src/main/java/.../test/...` 也可以是一个合法包。`examples`、`integration`、`locale` —— 全都可以当 Java 包标识符。Java 把文件系统布局词汇跟包命名混为一谈，现在生态里每个工具都得绕着它走。

这是一种**反人类的工程设计**。它逼着每个静态分析工具要么 (a) 任意深度跳 test/ 然后坑了 Java，要么 (b) 把 test/ 限到 top-only 然后漏掉其他所有语言 monorepo 里深度嵌套的 test 目录。非 Java 项目上的噪声大得离谱 —— 光 rustc 一个项目就有 `tools/rust-analyzer/crates/*/src/*/tests/` 嵌到第 7 层，全从一个 depth-3 的 top-only 闸门里漏出去。

CodeScope 选了方案 (c)：**Java 项目给一个例外，其他所有项目都拿到正确行为。**

当索引器检测到一个 `.java` 文件时，会把 `FilterPolicy` 切到 Java 模式：`test/`/`docs/`/`samples/`/... 的冲突项被限定到 **top-only（深度 ≤ 3）**，通过 `java_protected_skip_dirs_` 处理，所以 `org/.../samples/petclinic`（深度 5+）**不**跳，但 `<root>/test/`、`<root>/src/test/java/`、`<root>/packages/<name>/tests/` 仍然跳。其他所有语言（Rust、Go、Python、JS/TS、C/C++、Kotlin、Ruby、Scala、...）都保持这些名字在任意深度被跳过 —— 本该如此。

**如果你在索引 Java 项目**：什么都不用做。索引器自动检测 `.java` 文件并把 `FilterPolicy` 切到 Java 模式，把 `test/`/`docs/`/`samples/`/... 通过 `java_protected_skip_dirs_` 限定到 top-only（深度 ≤ 3）—— 嵌套包如 `org/.../samples/petclinic`（深度 5+）保留，`<root>/test/`/`src/test/java/`/`packages/<name>/tests/` 仍然跳。这是唯一能工作的 override 机制。

```bash
# Java 项目 —— 自动检测处理，不需要环境变量：
codescope index <your-java-project>
```

```bash
# 如果你确实想跳掉 Java 项目里嵌套的 test/docs
# （即关掉这个例外，到处都任意深度跳），
# CODESCOPE_EXCLUDE_PATHS 只能在 built-in 列表之上"加"exclude 模式
# —— 它不能"un-skip"嵌套包。干净路径是项目级 .codescopeignore
# pattern 精确匹配你想丢的具体嵌套目录。
```

这个 trade-off 在这里公开记录。Java 的包命名冲突是语言本身的设计缺陷，不是 CodeScope 的，我们拒绝让它拖累其他 99% 项目的体验。


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


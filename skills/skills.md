# CodeScope Skills

CodeScope 是一个基于 MCP 协议的代码理解服务。它将源代码解析为统一 AST IR，构建代码图（调用图+引用图），持久化到 SQLite，通过 MCP 工具暴露查询能力。

---

## 快速开始

```bash
# 索引项目
./skills/index.sh ~/path/to/project

# 查询统计
./skills/stats.sh

# 追踪调用路径
./skills/trace.sh func_a func_b

# 分析热点
./skills/hotspots.sh

# 查看模块树
./skills/modules.sh

# 完整分析流水线
./skills/analyze.sh ~/path/to/project
```

---

## 工具清单

### 索引工具

| 工具 | 用途 | 输入 | Token |
|------|------|------|-------|
| `index_project` | 全量索引（自动 worker 隔离进程） | project_path, language_filter | ~50 |
| `index_file` | 索引单个文件 | file_path | ~30 |
| `index_batch` | 事务内批量索引 | files (JSON 数组) | ~30 |
| `scan_project` | 快速扫描（ms 级，无全量解析） | project_path, language_filter | ~200 |
| `enhance_project` | 后台全量增强（异步） | — | ~20 |

### 符号查询

| 工具 | 用途 | 输入 | Token |
|------|------|------|-------|
| `find_definition` | 查找符号定义位置 | symbol_name | ~20 |
| `find_references` | 查看所有引用 | symbol_name | ~30 |
| `find_symbol` | 精确匹配符号 | symbol_name | ~30 |
| `locate_code` | 获取符号附近上下文 | identifier, context_lines | ~50-300 |
| `get_complexity` | 圈复杂度/认知复杂度 | node_id | ~20 |

### 调用图

| 工具 | 用途 | 输入 | Token | 前提条件 |
|------|------|------|-------|---------|
| `find_callers` | 谁调用了这个函数 | symbol_name | ~10-50 | `buildGraph(true)` |
| `find_callees` | 这个函数调用了什么 | symbol_name | ~10-50 | `buildGraph(true)` |
| `codescope_trace` | 两点间最短调用路径 | from, to | ~50-200 | 同上 |
| `get_hotspots` | 项目中最热门的函数 | top_n | ~500 | 同上 |
| `graph_query` | 自定义图查询 (DSL) | query | ~50-500 | — |

### 项目概览

| 工具 | 用途 | Token |
|------|------|-------|
| `get_graph_stats` | 节点/边/文件计数 | **~18** |
| `get_project_info` | 许可证/语言/文件数 | **~44** |
| `project_overview` | 项目综合摘要（首选入口） | **~71** |
| `get_module_tree` | 分层模块结构 | **~4** |
| `get_entry_points` | 入口点列表 | **~5** |

### 搜索

| 工具 | 用途 | Token |
|------|------|-------|
| `search` **(推荐)** | 统一代码搜索 (FTS + 语义) | ~300-1000 |

### 分析工具

| 工具 | 用途 | Token | 注意 |
|------|------|-------|------|
| `get_communities` ⚠️ | 社区检测 (Label Propagation) | **1K-200K** | 大项目谨慎使用，见下方说明 |
| `detect_changes` | 文件变更影响分析 | ~100-500 | 输入修改文件列表 |
| `codescope_build_context` | AI 上下文构建 **（主工具）** | ~200-1000 | 自动判断需要的信息 |
| `codescope_capabilities` | 功能就绪状态 | ~309 | 诊断"为什么查不到数据" |

### 工具

| 工具 | 用途 | Token |
|------|------|-------|
| `count_tokens` | DeepSeek Token 估算 (ASCII×0.3 + 中文×0.6) | ~10 |
| `get_enhancement_status` | 检查异步增强进度 | ~30 |

---

## 社区检测使用指南 ⚠️

> 通过 Label Propagation 将代码图节点按关系紧密程度分组。适用于理解模块边界、检测架构违规。

| 场景 | 推荐用法 | 预期 Token |
|------|---------|-----------|
| 接手 legacy 项目 | `max_communities=20` | ~1K-10K |
| 架构逆向 | `max_members=5, max_communities=50` | ~5K-50K |
| 检测架构违规 | `include_members=true` | ~10K-200K |
| 小型项目 (<500 节点) | 默认参数 | ~1K |
| 大型项目 (>10K 节点) | ⚠️ `max_communities=10` | ~1K-50K |

**副作用与屏蔽策略：**
1. **Token 爆炸**：123K 节点项目可达 **200K tokens**
2. **耗时**：全图算法数百 ms
3. `get_module_tree`（4 tok）通常已足够，社区检测仅作补充
4. 务必设置 `max_communities`（10-20），保持 `include_members=false`

---

## 选择策略

```
新项目      → project_overview (71 tok) + get_module_tree (4 tok)
找入口点    → get_entry_points (5 tok)
查热点      → get_hotspots (500 tok)
搜代码      → search (300-1000 tok)
查调用链    → find_callers / find_callees (10-50 tok)
架构分析    → get_module_tree (4 tok) + 可选 get_communities (1K-200K tok)
变更影响    → detect_changes (100-500 tok)
AI 问答     → codescope_build_context (200-1000 tok)
```

---

## 环境变量

| 变量 | 默认值 | 说明 |
|------|-------|------|
| `CODESCOPE_DB_PATH` | `.codescope/codescope.db` | SQLite 路径 |
| `GRAMMARS_DIR` | `grammars/` | tree-sitter 语法 .so 目录 |
| `CODESCOPE_INDEX_MODE` | `normal` | 模式: fast/normal/deep |
| `CODESCOPE_VERBOSE` | `1` | 设为 0 关闭批量日志 |
| `CODESCOPE_MAX_FILE_SIZE` | 5MB | 最大索引文件大小 |
| `CODESCOPE_BENCH_JSON` | — | benchmark JSON 输出路径 |

## 索引模式

| 模式 | FTS | 向量 | 速度 | 场景 |
|------|-----|------|------|------|
| `fast` | ❌ | ❌ | 最快 | 快速回答 |
| `normal` | ✅ | ❌ | 正常 | 默认 |
| `deep` | ✅ | ✅ | 较慢 | 全量语义分析 |

## 支持语言

Python, Go, Rust, JavaScript, TypeScript, TSX, C, C++, Java, Kotlin, Ruby, Scala, Swift

## 构建命令

```bash
make build          # 构建全部
make test           # 运行全部测试
make test-engine    # 仅引擎测试
make build-server   # 构建 Rust 服务器
make bench-check    # 快速基准
make bench-full     # 全量基准
```

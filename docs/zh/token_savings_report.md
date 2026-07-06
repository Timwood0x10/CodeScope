# Token 节省报告

> CodeScope 对比直接读取源文件 —— 以及对比 codebase-memory-mcp —— 基于真实分析场景的实测数据。
> 硬件：Apple M3 Max, 36 GB RAM。系统：macOS。

## 为什么 Token 节省很重要

当 LLM 需要理解代码时，有两个选择：
1. **读取原始源文件**——数千行代码，大部分是无关上下文
2. **查询 CodeScope**——精确获取需要的结构化知识（符号、图、指标）

CodeScope 相对于原始源码平均减少 **~98.9%** 的 token 消耗，相对于 codebase-memory-mcp 减少 **~85%**。

## 各场景实测节省

### 对比原始源码

| 场景 | CodeScope (tokens) | 原始源码 (tokens) | 节省 |
|------|:------------------:|:-----------------:|:----:|
| 查找函数定义 | ~21 | ~2,265 | **99.1%** |
| 追踪函数调用者 | ~18 | ~2,000 | **99.1%** |
| 项目模块树 (`get_module_tree`) | ~4 | ~1,875 | **99.8%** |
| 项目总览 (`project_overview`) | ~71 | ~1,875 | **96.2%** |
| 函数复杂度分析 | ~43 | ~4,733 | **99.1%** |
| 符号名称搜索 | ~23 | ~958 | **97.6%** |
| USB 驱动子系统概览 | ~250 | ~24,000 | **99.0%** |
| Linux 内核调度器分析 | ~180 | ~15,000 | **98.8%** |
| **平均** | **~76** | **~6,744** | **98.9%** |

### 对比 codebase-memory-mcp

| 查询 | CodeScope (tokens) | codebase-memory-mcp (tokens) | 节省 |
|------|:------------------:|:---------------------------:|:----:|
| 图统计 | **~18** | ~5,012 (get_architecture) | **99.6%** |
| 热点 Top 10 | **~488** | ~5,012（含在架构报告） | **90.3%** |
| 搜索 "handler" | **~372** | ~830 (search_graph) + ~828 (search_code) | **55.2%** |
| 入口点 | **~5** | ~5,012（含在架构报告） | **99.9%** |
| 模块树 | **~4** | ❌ 不支持 | — |
| 社区检测（默认） | **~1K-50K** | ❌ 不支持 | — |
| 调用者查询 | **~7** | — | — |
| **典型分析组合** | **~910** | **~6,048** | **85.0%** |

## 真实基准测试

### 项目索引（最终优化版）

| 项目 | 文件数 | 索引时间 | 节点 | 边 | 数据库大小 |
|------|:-----:|:--------:|:----:|:--:|:---------:|
| ARES Agent (Go) | 95 | **0.31s** | 24,924 | 23,184 | ~250 MB |
| memscope-rs (Rust+C) | 238 | **2.36s** | 123,270 | 108,905 | ~1.2 GB |
| GoAgent (1,167 Go) | 1,167 | **2.89s** | 261,743 | 244,078 | ~2.8 GB |
| Linux 内核 6.14.7（部分） | 6,173 | **~300s**（超时） | — | — | 12 GB |

### 查询 Token 消耗（逐工具参考）

| 工具 | Token 消耗 | 何时使用 |
|------|:----------:|---------|
| `get_graph_stats` | **~18** | 快速了解项目规模 |
| `get_module_tree` | **~4** | 项目模块结构概览 |
| `get_entry_points` | **~5** | 查找入口点 |
| `find_callers` / `find_callees` | **~10-50** | 调用图追踪 |
| `get_project_info` | **~44** | 项目元信息 |
| `project_overview` | **~71** | 新项目第一步 |
| `find_definition` / `find_references` | **~20-30** | 符号定位 |
| `get_complexity` | **~20** | 复杂度指标 |
| `locate_code` | **~50-300** | 源码上下文 |
| `codescope_capabilities` | **~309** | 功能就绪状态 |
| `search` | **~300-1000** | 代码搜索 |
| `codescope_trace` | **~50-200** | 调用路径追踪 |
| `detect_changes` | **~100-500** | 变更影响分析 |
| `get_hotspots` | **~500** | 热点函数 |
| `codescope_build_context` | **~200-1000** | AI 上下文构建 |
| `get_communities`（默认） | **1K-200K** ⚠️ | 模块边界检测 |

### 典型分析流程

```
分析管道: project_overview + entry_points + hotspots + search + modules
CodeScope:          71  +     5      +    500   +  1000 +   4   =  ~1,580 tokens
codebase-memory:  get_architecture(5012) + search_graph(830) + query_graph(206) =  ~6,048 tokens

完整分析：CodeScope 比 codebase-memory-mcp 节省 ~74% tokens
          CodeScope 比直接读源码节省 ~98.9% tokens
```

## 原理

CodeScope 将代码库预索引为结构化知识库（SQLite，15+ 张表）。当 LLM 提问时，CodeScope 只返回**相关事实**——而不是整个源文件。

```
源代码 → CodeScope（事实 + 索引 + 图 + 指标）→ LLM 只获取相关上下文
                                                            ↓
                         对比直接读源码节省 ~98.9% tokens
                         对比 codebase-memory 节省 ~85% tokens
```

## 方法论

采用 DeepSeek 公式估算：`tokens = ASCII字符数 × 0.3 + 非ASCII字符数 × 0.6`。CodeScope 输出从实际 JSON 响应中计数。原始源码按 4 chars/token 估算。codebase-memory-mcp 输出通过 fast 模式索引的实际 JSON 响应计数。

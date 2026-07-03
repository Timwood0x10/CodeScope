# CodeScope

**CodeScope** 是一个基于 MCP（Model Context Protocol）协议的代码理解服务。它通过解析源代码生成统一 AST IR，构建多维代码图（调用图 + 符号引用图），持久化到 SQLite，并通过 14 个 MCP 工具暴露强大的查询能力——让 AI 通过图遍历理解代码结构和行为，无需读取原始源文件。

## 架构

```
AI 客户端 (Claude Desktop, Cursor 等)
    │
    │ MCP stdio (JSON-RPC 2.0)
    ▼
┌─────────────────────────────┐
│  Rust MCP Server             │
│  - MCP 协议处理              │
│  - 工具定义 + 分发            │
│  - C FFI 桥接到引擎          │
└──────────────┬──────────────┘
               │ C FFI
               ▼
┌─────────────────────────────┐
│  C++ 核心引擎                │
│  - tree-sitter 解析器（8 语言）│
│  - 统一 AST IR               │
│  - 图构建器                  │
│  - SQLite 存储               │
│  - 查询引擎                  │
└─────────────────────────────┘
```

**数据流：**
```
源代码 → tree-sitter CST → 统一 AST IR → 代码图 → SQLite 存储 → 查询引擎 → MCP 工具
```

## 功能特性

### 14 个 MCP 工具

| 分类 | 工具 | 说明 |
|------|------|------|
| **核心** (11) | `find_definition` | 查找符号定义位置 |
| | `find_references` | 查找符号的所有引用 |
| | `get_callers` | 获取调用指定函数的函数 |
| | `get_callees` | 获取指定函数调用的函数 |
| | `get_neighbors` | 获取图中节点的邻居 |
| | `find_shortest_path` | 查找两个节点间的最短路径 |
| | `get_subgraph` | 获取以某节点为中心的子图 |
| | `locate_code` | 定位代码实体在源文件中的位置 |
| | `index_project` | 索引整个项目目录 |
| | `index_file` | 索引单个源文件 |
| | `get_graph_stats` | 获取代码图统计信息 |
| **搜索** | `search_code` | FTS5 全文搜索（前缀匹配） |
| **分析** | `get_complexity` | 圈复杂度 + 嵌套深度 |
| | `graph_query` | Cypher-like DSL：`MATCH (函数)-[调用]->(函数)` |
| | `detect_changes` | 变更影响分析（修改文件的调用者/被调用者） |
| | `get_communities` | 标签传播社区检测 |

### 支持的语言（8 种）

| 语言 | 解析器 | IR 转换器 | 已验证 |
|------|--------|-----------|--------|
| Python | ✅ | ✅ | ✅ |
| Go | ✅ | ✅ | ✅ |
| C | ✅ | ✅ | ✅ |
| C++ | ✅ | ✅ | ✅ |
| Rust | ✅ | ✅ | ✅ |
| JavaScript | ✅ | ✅ | ✅ |
| TypeScript | ✅ | ✅ | ✅ |
| Java | ✅ | ✅ | ✅ |

### 图能力

- **6 种边类型**：`References`、`Calls`、`Defines`、`Contains`、`Imports`、`Inherits`
- **8 种节点类型**：`Function`、`Method`、`Class`、`Struct`、`Interface`、`Variable`、`Module`、`File`
- **SQLite 持久化**：零外部依赖，单文件数据库，可移植
- **FTS5 全文搜索**：符号名和文件路径前缀匹配
- **社区检测**：标签传播算法实现架构概览
- **变更影响分析**：通过调用图追踪影响传播

## 快速开始

### 前置依赖
- Rust 2024 Edition + 1.85+（`cargo`）
- CMake 3.30+，C++23 编译器（Clang 17+）
- SQLite3（开发包）
- tree-sitter 核心库
- Node.js（构建语法 .so 文件）

### 构建与运行

```bash
# 1. 安装 tree-sitter 语法（一次性）
npm install -g tree-sitter-python tree-sitter-c tree-sitter-cpp \
  tree-sitter-rust tree-sitter-javascript tree-sitter-typescript \
  tree-sitter-go tree-sitter-java

# 2. 构建语法 .so 文件
cd grammars && bash build.sh && cd ..

# 3. 构建全部
make build

# 4. 运行所有测试
make test

# 5. 启动 MCP 服务
cargo run --bin ast-graph-mcp
```

### 作为 Claude Desktop MCP 服务器

添加到 `claude_desktop_config.json`：
```json
{
  "mcpServers": {
    "codescope": {
      "command": "/path/to/CodeScope/target/release/ast-graph-mcp",
      "args": [],
      "env": {
        "ASTGRAPH_DB_PATH": "/tmp/astgraph.db",
        "GRAMMARS_DIR": "/path/to/CodeScope/grammars"
      }
    }
  }
}
```

### 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `ASTGRAPH_DB_PATH` | `/tmp/astgraph.db` | SQLite 数据库路径 |
| `GRAMMARS_DIR` | `grammars/` | 语法 .so 文件目录 |
| `CODESCOPE_LSP` | 未设置 | LSP 服务器命令（如 `pylsp`），用于类型增强 |

## Token 节省

使用代码图代替原始源文件，5 个常见查询场景平均节省 **~98.8%** 的 token：

| 场景 | 图 (tokens) | 源码 (tokens) | 节省 |
|------|------------|-------------|------|
| 函数定义查找 | ~21 | ~2,265 | **99.1%** |
| 调用者追踪 | ~18 | ~2,000 | **99.1%** |
| 架构概览 | ~32 | ~1,875 | **98.3%** |
| 函数分析 | ~43 | ~4,733 | **99.1%** |
| 符号搜索 | ~23 | ~958 | **97.6%** |

## 对比 codebase-memory-mcp

| 维度 | CodeScope | codebase-memory-mcp |
|------|-----------|---------------------|
| **后端** | SQLite（嵌入式） | Neo4j（外部服务） |
| **部署** | 单个二进制 | Neo4j + 配置文件 |
| **搜索** | FTS5 前缀匹配 | BM25 + 向量语义搜索 |
| **图查询** | 最小 DSL | 完整 Cypher |
| **类型信息** | 可选 LSP 增强 | LSP 感知 |
| **复杂度** | 圈复杂度 + 嵌套深度 | 圈复杂度 + 认知复杂度 + 热路径 |
| **社区检测** | 标签传播 | Leiden 算法 |
| **跨仓库** | ❌ | ✅ |
| **ADR 管理** | ❌ | ✅ |
| **外部依赖** | 零 | Neo4j |

**CodeScope 优势**：零依赖部署、统一 IR 层、可移植。
**codebase-memory-mcp 优势**：更丰富的查询、语义搜索、类型感知解析。

## 路线图

参见 [plan/roadmap.md](plan/roadmap.md)。

## 开发

### 项目结构

```
server/         Rust MCP 服务器（协议、工具、FFI）
engine/         C++ 核心引擎
  src/parser/   tree-sitter 解析器封装
  src/ir/       统一 AST IR + 翻译器
  src/graph/    代码图构建器
  src/store/    SQLite 持久化
  src/query/    查询引擎 + DSL
  src/lsp/      LSP 客户端（类型增强）
grammars/       tree-sitter 语法 .so 文件
plan/           设计文档 + 路线图
tests/          集成测试
```

### 编码规范

- 文件上限：1000 行
- 注释：仅英文
- Rust：rustfmt + clippy
- C++：Google C++ Style Guide，C++23
- 内存管理：RAII，禁止裸 new/delete
- FFI：每个函数必须有所有权文档注释

## License

MIT

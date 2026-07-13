# CodeScope 架构拆解（十三）：解析器与 GraphBuilder

> tree-sitter 是这个项目最正确的技术选型之一。不是因为它快，而是因为它**不会报错**——无论源码有多畸形，tree-sitter 总能产出一棵 AST。这在索引百万行级别的项目时至关重要：一个文件解析失败不应该阻塞整个索引。

---

## Parser：tree-sitter 封装层

解析器位于 `engine/src/parser/parser.cpp`，是对 tree-sitter C API 的 C++ 封装。

### 职责

1. 根据文件扩展名选择语言解析器
2. 调用 tree-sitter 生成 CST
3. 将 CST 传给对应的 IR Translator
4. 处理解析错误（主要是内存限制）

```cpp
// engine/src/parser/parser.cpp 核心流程
TranslationUnit *parseFile(const char *path, const char *source,
                           size_t source_len, const char *language) {
    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, getLanguage(language));
    TSTree *tree = ts_parser_parse_string(parser, nullptr, source, source_len);
    // 将 TSTree 交给 IR Translator
    Translator *t = createTranslator(language);
    return t->translate(tree, source, path);
}
```

### tree-sitter 的选择理由

| 对比项 | tree-sitter | 传统 LALR/LR 解析器 |
|--------|-------------|-------------------|
| 容错性 | ✅ 永远产出 AST（含 error 节点） | ❌ 遇到语法错误就挂 |
| 增量解析 | ✅ 支持 | ❌ 需要重新解析全部 |
| 多语言 | ✅ 原生支持 100+ 语言 | ❌ 每种语言一套工具链 |
| 速度 | ✅ C 实现，足够快 | ✅ 更快，但不够鲁棒 |

### 代价

tree-sitter 的 CST 包含**所有语法细节**——括号、分号、关键字。IR Translator 的工作就是从中提取语义结构，丢弃语法噪音。

---

## Grammar 管理

```cpp
// engine/src/codescope_grammars.h
// 管理 8 种语言的 tree-sitter grammar 编译产物（.so 文件）
```

Grammars 在构建时通过 CMake FetchContent 自动下载并编译。每种语言对应一个 `.so` 文件，运行时按需加载。当前支持的 8 种语言和其 grammar 状态：

| 语言 | Grammar 源 | AST 节点类型数 |
|------|-----------|:-------------:|
| C | tree-sitter-c | ~50 |
| C++ | tree-sitter-cpp | ~100 |
| Go | tree-sitter-go | ~60 |
| Python | tree-sitter-python | ~40 |
| Rust | tree-sitter-rust | ~80 |
| JavaScript | tree-sitter-javascript | ~70 |
| TypeScript | tree-sitter-typescript | ~80 |
| Java | tree-sitter-java | ~70 |

---

## GraphBuilder：从 IR 到图

`GraphBuilder` 位于 `engine/src/graph/graph_builder.h`，是 IR 翻译和 Resolver 之间的桥梁。

### 两套 API

```
旧 API: TranslationUnit + Node* 树
  ├── 用于: C, C++, Go, Python, Rust, Java
  └── Visitor 模式递归遍历 IR 树
      对每个 NodeKind → 生成 GraphNode
      对每个 Relation → 生成 GraphEdge

新 API: SemanticUnit + flat records
  ├── 用于: JavaScript, TypeScript
  └── 直接接受 flat records（不需要遍历树）
      因为 JS/TS visitor 输出已经是扁平格式
```

### 图类型定义

```cpp
// engine/src/graph/graph_types.h
enum class NodeType : uint8_t {
    Function, Method, Class, Struct, Interface,
    Variable, Macro, Module, File
};

enum class EdgeType : uint8_t {
    References, Calls, Defines, Contains, Imports, Inherits
};
```

### Parent Chain Cache

GraphBuilder 的关键优化：在插入子节点时，通过 `parent_chain_cache` 缓存父节点 ID，避免反复查询 SQLite。对于深层嵌套的 AST（如复杂表达式），这个缓存减少了 **O(depth)** 的数据库查询。

### 写入流程

```
GraphBuilder::build(translation_unit)
    │
    ├── 遍历 IR 节点树
    │   ├── FunctionDecl → NodeType::Function
    │   ├── ClassDecl    → NodeType::Class
    │   └── VariableDecl → NodeType::Variable
    │
    ├── 遍历 Relation 列表
    │   ├── CallTarget   → EdgeType::Calls
    │   ├── BaseClass    → EdgeType::Inherits
    │   └── SymbolRef    → EdgeType::References
    │
    └── 批量写入 SQLite
        ├── graph_nodes (逐条 insert)
        └── graph_edges (批量 insert)
```

---

## 性能

从实际基准测试看：

| 阶段 | 小项目（200 文件） | Linux 内核（6.4 万文件） |
|------|:-----------------:|:----------------------:|
| tree-sitter 解析 | ~秒级 | ~2 分钟 |
| IR 翻译 | ~毫秒级 | ~30 秒 |
| GraphBuilder 写入 | ~毫秒级 | ~20 秒 |
| ResolverPipeline | ~毫秒级 | ~1 分钟（最耗时） |

**ResolverPipeline 是唯一跟项目规模超线性相关的阶段**——因为每个引用都需要在所有 entity 中查找候选。其他阶段基本是 O(N) 线性扩展。

---

## 系列导航

| # | 文章 | 主题 |
|---|------|------|
| (一) | 开篇 | 56KB vs 629 bytes，CodeScope 要解决什么问题 |
| (二) | 渐进式就绪 | 毫秒级让 AI 开始理解你的代码 |
| (三) | Worker 隔离 | 为什么索引不会拖垮 MCP Server |
| (四) | 零冗余响应 | 精简响应，按需返回 |
| (五) | C++ 引擎拆解 | 从源码到多维代码图的管线 |
| (六) | MCP 协议层 | 工具的设计哲学 |
| (七) | 语言翻译器 | 10 种语言 → 统一 IR |
| (八) | 存储层 | SQLite WAL + FTS5 + vec0 |
| (九) | 自适应查询 | Fallback 机制与就绪检测 |
| (十) | 性能真相 | 从 200 到 60,000 文件的实测 |
| (十一) | 验证层 | 让 AI 对自己的话负责 |
| (十二) | Model Engine | 从事实到理解 |
| **(十三)** | **Parser + GraphBuilder** | **解析与建图 ← 本文** |

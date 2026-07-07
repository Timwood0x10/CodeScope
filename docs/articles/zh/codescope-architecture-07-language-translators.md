# CodeScope 架构拆解（七）：语言翻译器——10 种语言 → 统一 IR

> 我一开始以为，"支持 10 种语言"是这个项目最难的工程挑战。一个语言一个解析器、一个 AST、一套规则——写到第 3 种的时候代码就开始失控了。到第 8 种的时候，每个新增语言的代码量不是"再加一个 if"，而是"再加一个 600 行的 monolithic 类"。
>
> 后来我意识到：问题不在于语言数量，而在于抽象层的设计。如果每个语言的 visitor 都从同一个基类继承，共享调度逻辑和 scope 管理，那么新增一种语言的边际成本就可以降到 200 行。事实证明了——从 GoTranslator（600 行）到 GoVisitor（~200 行），4 倍的代码量缩减。

---

## 三个问题

1. **tree-sitter 为每种语言生成完全不同的 CST 节点名**——JavaScript 叫 `function_declaration`，C 叫 `function_definition`，Rust 叫 `function_item`。怎么统一？
2. **不同语言的语义粒度不同**——Go 的 `short_var_decl` 没有类型信息，Java 的 `variable_declaration` 有。翻译成 IR 时信息损失多少是可接受的？
3. **全量 AST 太大**——一个 1000 行的文件生成 200-400KB 的 AST。但分析代码关系只需要其中 1/5 的节点。有没有必要保留全部？

---

## 两条管线

CodeScope 内部不是一条翻译管线，而是**两条独立的管线共存**。这不是一开始规划好的——它反映了一个工程项目的演进过程。

```
                    createTranslator()                     createJsVisitor()
                           |                                       |
                    +------+--------+                    +--------+--------+
                    |               |                    |                 |
            [Old Pipeline]   nullptr                [New Pipeline]  ScannerVisitor
            TranslationUnit                        SemanticUnit
            (Node 树)                              (扁平 Record)
            ~60 bytes/节点                          ~50-200 bytes/记录
            children 向量                           parent_id 链接
            SemanticEdge 指针                       无指针
            
            全量 AST                               语义子集
            39 种 NodeKind                         16 种 RecordKind
            控制流完整                              控制流舍弃
```

### 旧管线：完整 AST 树

旧管线的核心是 `Translator` 接口：

```cpp
// engine/src/ir/ir_translator.h
class Translator {
public:
    virtual ~Translator() = default;
    virtual TranslationUnit *translate(TSTree *tree, const char *source,
                                       const char *file_path) = 0;
    virtual const char *language() const = 0;
};
```

每个语言有一个独立的 `*Translator` 类，全部定义在各自的 `.cpp` 文件中：

| 文件 | 类名 | 行数 |
|---|---|---|
| `translators/c_translator.cpp` | `CTranslator` | ~600 |
| `translators/javascript_translator.cpp` | `JavascriptTranslator` | ~400 |
| `translators/go_translator.cpp` | `GoTranslator` | ~600 |

它们共享一个模式：
- `translate()` 创建 `TranslationUnit`，设置根节点，递归调用 `translateChildren()`
- 每个 tree-sitter 节点类型对应一个 `if/else` 分支
- 节点通过 `parent->children.push_back(child)` 构建树结构
- 语义边直接在 `Node*` 指针间创建

输出是 `TranslationUnit` —— 一个完整的 AST 树：

```cpp
// engine/src/ir/ir.h
struct Node {
    uint64_t id;
    NodeKind kind;              // 39 种
    std::string name;
    std::string qualified_name;
    SourceLocation loc;
    std::vector<Node*> children;
    std::vector<SemanticEdge> semantic_edges;
    std::string language;
    std::string file_path;
    bool has_error;
};

struct TranslationUnit {
    Node *root;
    std::vector<Node*> all_nodes;  // 持有所有权
    std::string source_content;
};
```

39 种 NodeKind 覆盖了所有编程结构：声明（FunctionDecl、ClassDecl、VariableDecl）、语句（IfStmt、ForStmt、WhileStmt、SwitchStmt）、表达式（CallExpr、BinaryExpr、MemberExpr）、导入导出（ImportDecl、ExportDecl）、注释。

**优点**：完整。GraphBuilder 需要什么都能从树上取到。

**缺点**：太大。每个节点 ~60 字节 + 子向量 + 字符串堆分配。1000 行文件产生 200-400KB。而且大多数下游查询根本不关心控制流。

### 新管线：扁平语义记录

新管线的核心是 `JsVisitor` 基类和 `SemanticUnit` 输出：

```cpp
// engine/src/ir/translators/js_visitor.h
class JsVisitor {
public:
    virtual SemanticUnit *visit(TSTree *tree, const char *source,
                                const char *file_path);
    virtual void reset();
    // scope 管理、handler 方法、Aho-Corasick 调度
};
```

输出是扁平向量：

```cpp
// engine/src/ir/semantic_unit.h
struct Record {
    uint64_t id;
    RecordKind kind;                // 16 种（语义子集）
    std::string name;
    std::string qualified_name;
    uint64_t parent_id;             // 0 = 顶层（无指针！）
    SourceRange loc;
    std::string file_path;
    std::string language;
};

class SemanticUnit {
    std::vector<Record> records_;   // 扁平、连续内存
    uint64_t next_id_ = 1;
};
```

关键差异：
- **无指针**：父子关系通过 `parent_id`（uint64_t）表达
- **无向量**：每个 Record 没有自己的 `children` 向量
- **连续内存**：`std::vector<Record>` 在堆上连续排列，cache-friendly
- **大小减半**：~50-200 bytes/record，1000 行文件 ~50KB

---

## Aho-Corasick 调度

新管线的核心设计是 **Aho-Corasick 自动机** 替代了旧管线中的 `if/else strcmp` 链。

```cpp
// engine/src/ir/translators/js_visitor.cpp
void JsVisitor::visitNode(TSNode node, uint64_t parent_id) {
    const char *type = ts_node_type(node);
    int id = getJsAC().match(type);   // O(n) 模式匹配
    switch (id) {
    case 100: return visitFunctionDecl(node, parent_id);
    case 101: return visitArrowFunction(node, parent_id);
    case 102: return visitClassDecl(node, parent_id);
    case 104: return visitCallExpr(node, parent_id);
    // ...
    case 200: emitter_->emitLiteral(nodeText(node), loc, parent_id); return;
    case 300: visitChildren(node, parent_id); return;  // 透传
    default:  visitChildren(node, parent_id); return;
    }
}
```

调度表分为三个级别：

```cpp
// engine/src/ir/translators/js_visitor.cpp (AC 构建)
// 100-109: Handlers — 产生语义记录
ac.addPattern("function_declaration", 100);
ac.addPattern("arrow_function", 101);
ac.addPattern("class_declaration", 102);
ac.addPattern("call_expression", 104);
ac.addPattern("variable_declaration", 106);
ac.addPattern("member_expression", 109);

// 200: Literals — 产生 Literal 记录
ac.addPattern("number", 200);
ac.addPattern("string", 200);
ac.addPattern("comment", 201);

// 300: 复合语句 — 透传（不产生记录，仅递归子节点）
ac.addPattern("if_statement", 300);
ac.addPattern("for_statement", 300);
ac.addPattern("while_statement", 300);
ac.addPattern("binary_expression", 300);
ac.addPattern("ternary_expression", 300);
```

这意味着：
- **只有语义上有意义的节点产生记录**（声明、调用、成员访问、导入导出）
- **控制流结构被舍弃**（if/for/while/switch/try——它们的子节点被"透传"到最近的函数/类作用域）
- **操作符被舍弃**（二元/一元/赋值表达式——递归访问操作数）
- **类型注解被舍弃**（只在 TypeScript visitor 中特殊处理了 TypeAlias）

**故意的信息损失**。这个设计的前提是：代码分析的下游任务（调用图、符号搜索、模块依赖）只关心"有什么符号"和"谁调了谁"，不关心"这个 if 的条件是什么"。

---

## Visitor 继承体系

所有新管线 visitor 共享 `JsVisitor` 作为基类——包括 C/C++ visitor：

```
JsVisitor          ── 基类: Aho-Corasick 调度、scope 跟踪、helper 方法
  │
  +-- CVisitor     ── 覆写 visitNode(), 增加 C handler（func/struct/enum/call/include/typedef/preproc）
  │     │
  │     +-- CppVisitor  ── 增加 class_specifier/namespace_definition/template_declaration
  │
  +-- GoVisitor    ── func/method/type/call/import/var/short_var
  +-- PythonVisitor  ── func/class/call/import/assignment
  +-- JavaVisitor  ── method/class/interface/enum/invocation/variable/import
  +-- RustVisitor  ── function/struct/enum/trait/impl/call/let/use
  +-- SwiftVisitor ── func/class/struct/enum/protocol/call/var/import
  │
  +-- TsVisitor    ── 增加 interface_declaration/type_alias_declaration/enum_declaration
       │
       +-- TsxVisitor  ── JSX 元素跳过（无语义价值）, JSX 表达式递归
```

### CVisitor：最复杂的实现

C 的 visitor 是最厚的一个，因为它要处理 GNU C 扩展带来的 tree-sitter 解析问题：

```cpp
// engine/src/ir/translators/c_visitor.cpp (简化)
void CVisitor::visitFunctionDecl(TSNode node, uint64_t parent_id) {
    // GNU C: __attribute__((...)) 包裹函数声明
    TSNode actual = skipAttributedDeclarator(node);
    // GNU C: ERROR 节点可能包含函数定义
    if (actual_is_error) {
        // tree-sitter 对 GNU C 扩展产生错误节点
        // 递归到 ERROR 子节点中找函数定义
        // ...
    }
    emitFunction(actual, parent_id);
}
```

这个 visitor 处理的边界情况：
- **`__attribute__((xxx))`**：跳过，找到实际的函数声明
- **ERROR 节点**：tree-sitter 对 GNU C 扩展（如 `__sched`）会产生 ERROR 节点。C visitor 递归到 ERROR 的子节点中找到函数定义
- **宏定义**：`#define`（简单和函数式）作为 Variable 记录
- **预处理条件编译**：`#if`/`#ifdef` 内的声明仍然递归索引

### JS/TS：最小的增量差异

TypeScript 的 visitor 只比 JS 多了三个 handler：

```cpp
// engine/src/ir/translators/ts_visitor.h
class TsVisitor : public JsVisitor {
protected:
    void visitInterfaceDecl(TSNode node, uint64_t parent_id);
    void visitTypeAliasDecl(TSNode node, uint64_t parent_id);
    void visitEnumDecl(TSNode node, uint64_t parent_id);
    // 覆写：class 名在 TS 中叫 type_identifier 而非 identifier
    void visitClassDecl(TSNode node, uint64_t parent_id) override;
};
```

TsxVisitor 更简单——只处理 JSX：

```cpp
class TsxVisitor : public TsVisitor {
protected:
    void visitJsxElement(TSNode node, uint64_t parent_id);
    // JSX 元素本身跳过（无语义价值）
    // JSX 表达式 { ... } 递归进去
};
```

---

## 语言差异的处理

不同语言的 visitor 处理相同的语义结构时，语法差异被吸收在 visitor 内部：

| 语义 | JavaScript | C | Rust | Go |
|---|---|---|---|---|
| 函数 | `function_declaration` | `function_definition` | `function_item` | `function_declaration` |
| 类 | `class_declaration` | `struct_specifier` | `struct_item` | `type_spec` |
| 调用 | `call_expression` | `call_expression` | `call_expression` | `call_expression` |
| 赋值 | `variable_declaration` | `declaration` | `let_declaration` | `short_var_decl` |

每个 visitor 的职责是把这些不同的语法映射到同一个 `RecordKind`（Function、Class、CallExpr、Variable），抹除语言差异。

---

## ScannerVisitor：10x 更快的备选

除了完整解析，还有一个轻量级的 `ScannerVisitor`：

```
完整 tree-sitter 解析:  ~30-50ms/文件 (1000 行)
ScannerVisitor:         ~3-5ms/文件  (10x 更快)
```

它的原理极其简单：

```cpp
// engine/src/ir/translators/scanner_visitor.cpp (概念)
void ScannerVisitor::scanFile(const char *source, size_t len) {
    for (each line in source) {
        if (line matches "<type_keyword> <name>(")  // function
            emit(Function, name, location);
        else if (line matches "class <name>")       // class
            emit(Class, name, location);
        else if (line matches "struct <name>")      // struct
            emit(Class, name, location);
        // ... 更多语言特定的关键词规则
    }
}
```

它不调用 tree-sitter，不解析 AST，不做 scope 跟踪。只按行匹配关键词模式。这对应的是整个管线中的 Phase A 快速扫描。

关键实现细节：
- **语言特定的关键词表**：每种语言有自己的检测模式
- **C/C++ 的 false-positive 过滤**：`if (condition)` 会被误识别为函数，需要检查前面是不是 if/while/for/switch/catch/return 关键词
- **只提取顶层声明**：不解析函数体
- **输出兼容性**：结果写入相同的 `SemanticUnit` 格式

---

## 数据对比

| 维度 | 旧管线（Translator） | 新管线（JsVisitor） | ScannerVisitor |
|---|---|---|---|
| 输出 | TranslationUnit（Node 树） | SemanticUnit（扁平记录） | SemanticUnit |
| NodeKind/RecordKind | 39 种 | 16 种 | 16 种 |
| 单文件内存（1000 行） | ~200-400 KB | ~50 KB | ~10 KB |
| 控制流 | 完整保留 | 舍弃 | 舍弃 |
| 类型信息 | 部分保留（通过节点类型） | 极少 | 极少 |
| 跨文件解析 | Linker 后续处理 | GraphBuilder 后续处理 | N/A |
| 调度方式 | if/else strcmp 链 | Aho-Corasick 自动机 | 行匹配 |
| 新语言边际成本 | 400-600 行 | 150-250 行 | 30-50 行（关键词表） |
| 典型耗时（1000 行） | 30-50ms | 30-50ms | 3-5ms |

---

## 坦诚反思

**1. 两条管线并存是历史遗留问题**

旧管线是先写的，发现太臃肿才设计了新管线。但旧管线没有被淘汰——因为 GraphBuilder 的某些路径还依赖 `TranslationUnit` 的 Node 树结构。新管线只用于 JS/TS 系列的 visitor。

理想情况下应该只有一条管线。但从实用角度看，维护两套代码的边际成本（通过共享的 IR 类型系统）低于一次性迁移所有 visitor 的风险。

**2. RecordKind 是 NodeKind 的子集——但这是过度优化吗？**

新管线舍弃了控制流信息。理论上，GraphBuilder 构建调用图不需要知道 if/for/while 语句。但有一些场景（如路径追踪——"这个调用是否在错误处理路径中"）需要控制流上下文。当这些场景出现时，就需要回退到旧管线或重新设计 RecordKind。

**3. ScannerVisitor 的局限性**

扫描器模式匹配会漏掉一些声明——比如多行声明的续行、嵌套在表达式内部的函数定义、模板特化等。Phase A 的设计本就是"有损但足够快"。问题在于"足够"的标准——如果 AI 依赖 `find_symbol` 的结果做出决策，被漏掉的声明可能导致错误结论。

---

## 系列导航

| 文章 | 主题 |
|---|---|
| (一) 开篇 | 56KB vs 629 bytes，CodeScope 要解决什么问题 |
| (二) 渐进式就绪 | 367ms 让 AI 开始理解你的代码 |
| (三) Worker 隔离 | 为什么索引不会拖垮 MCP Server |
| (四) 零冗余响应 | 1 Token 干 35 个 Token 的活 |
| (五) C++ 引擎拆解 | 从源码到多维代码图的管线 |
| (六) MCP 协议层 | 35+ 工具的设计哲学 |
| **(七) 语言翻译器** | **10 种语言 → 统一 IR ← 本文** |
| (八) 存储层 | SQLite WAL + FTS5 + vec0 |
| (九) 自适应查询 | Fallback 机制与就绪检测 |
| (十) 性能真相 | 从 200 到 60,000 文件的实测 |

---

下一篇我们拆解 **存储层**——SQLite 如何同时承担关系存储（图节点/边）、全文搜索（FTS5）和向量检索（vec0）三种角色。一张 `call_edges` 表如何支持 300ms 的调用链查询。
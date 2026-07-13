# CodeScope 架构拆解（五）：C++ 引擎拆解——从源码到多维代码图的管线

> 有一次我 debug 一个跨文件调用链，发现调用图里少了一条边。那个函数明明被调用了，但图里就是没有。翻了一下午代码，最后发现原因很简单：**调用发生在宏展开里，而我们的 visitor 不展开宏。** 这不是 bug，是 tradeoff。但这件事让我意识到，从源码到代码图，每一个环节都在做取舍。

---

## 三个问题

任何代码理解工具的核心，都是把"文本"变成"结构"。这个过程可以拆解成三个问题：

1. **如何在不解析 AST 的情况下，极速获取代码结构？**（毫秒级 返回结果的关键）
2. **如何把 10 种语言的 AST 统一成一种中间表示？**（跨语言分析的基础）
3. **如何从统一的 IR 构建出可供 AI 查询的多维代码图？**（最终交付物）

CodeScope 的 C++ 引擎，就是围绕这三个问题设计的。

---

## 整体管线

```
┌─────────────────────────────────────────────────────────────────┐
│                     C++ Engine Pipeline                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Phase A (Fast Scan)        Phase B/C (Full Index)              │
│  ┌───────────────┐         ┌─────────────────────────┐          │
│  │   Line-by-line │         │  File Read + Language   │          │
│  │   Regex Scan   │         │  Detection              │          │
│  │   (no AST!)    │         │         │               │          │
│  └───────┬───────┘         │         ▼               │          │
│          │                 │  ┌──────────────┐        │          │
│          ▼                 │  │ tree-sitter   │        │          │
│  ┌───────────────┐         │  │ Parse         │        │          │
│  │   Modules     │         │  └──────┬───────┘        │          │
│  │   Symbols     │         │         ▼               │          │
│  │   EntryPoints │         │  ┌──────────────┐        │          │
│  │   Stubs       │         │  │ IR Translator │        │          │
│  └───────────────┘         │  │ (CST → IR)   │        │          │
│          │                 │  └──────┬───────┘        │          │
│          │                 │         ▼               │          │
│          ▼                 │  ┌──────────────┐        │          │
│   ┌────────────┐          │  │ Linker Passes │        │          │
│   │  JSON      │          │  │ ┌────────────┐│        │          │
│   │  Response  │          │  │ │ BuildSymbol││        │          │
│   └────────────┘          │  │ │ IndexPass  ││        │          │
│          │                 │  │ ├────────────┤│        │          │
│          │                 │  │ │ ResolveCall││        │          │
│          │                 │  │ │ Pass       ││        │          │
│          │                 │  │ ├────────────┤│        │          │
│          │                 │  │ │ EmitGraph  ││        │          │
│          │                 │  │ │ Pass       ││        │          │
│          │                 │  │ └────────────┘│        │          │
│          │                 │  └──────┬───────┘        │          │
│          │                 │         ▼               │          │
│          │                 │  ┌──────────────┐        │          │
│          │                 │  │ Complexity   │        │          │
│          │                 │  │ Analyzer     │        │          │
│          │                 │  └──────┬───────┘        │          │
│          │                 │         ▼               │          │
│          │                 │  ┌──────────────┐        │          │
│          ├─────────────────►│  SQLite Store  │        │          │
│          │                 │  (FTS5 + vec0) │        │          │
│          │                 │  + Graph Nodes │        │          │
│          │                 │  + Graph Edges │        │          │
│          │                 └───────────────┘        │          │
│          │                          │               │          │
│          ▼                          ▼               │          │
│   ┌────────────────────────────────────────┐        │          │
│   │      Query Engine (adaptive)           │        │          │
│   │   Phase A ready → fast scan results    │        │          │
│   │   Phase B ready → + symbol search      │        │          │
│   │   Phase C ready → + call graph / FTS   │        │          │
│   └────────────────────────────────────────┘        │          │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Phase A：不建 AST，怎么理解代码？

Phase A 的目标是：**在 毫秒级 内，让 AI 知道这个项目里有什么。** 解析 AST 是不可能的——tree-sitter 解析 24K 行代码需要秒级，而且还不是瓶颈，后续的 IR 翻译和图构建才是大头。

CodeScope 的 Phase A 用了另一种思路：**逐行正则扫描**。

实际的扫描逻辑位于 `engine/src/engine_scanner.cpp`（1194 行），核心流程是：

```cpp
// engine/src/engine_scanner.cpp:786
char *engine_scan_project(uint64_t project_id, const char *dir_path,
                          const char *language_filter)
```

它做的事情：

1. **遍历目录树**，跳过 `.gitignore` 匹配的文件
2. **逐行读取文件**，跳过注释和空行
3. 对每一行，调用 `trimLeft()` 去掉前导空格，然后用 `detectDecl()` 判断声明类型
4. 匹配到的声明用 `extractName()` 提取符号名
5. 插入 SQLite，构建模块树

### detectDecl 的"黑魔法"

`detectDecl()` 的实现很有意思——它没有用正则表达式，而是用了一系列前缀匹配和启发式规则，针对每种语言做了特化：

```cpp
// engine/src/engine_scanner.cpp: 内部函数
// 检测 C 函数声明：如 "int foo(" 或 "void *bar("
bool looksLikeCFunction(std::string_view sv) {
    // 32 种 C 类型关键字：int, void, char, size_t, gfp_t, ...
    // 匹配 "type name(" 模式
}
```

对于 C/C++，它检查行首是不是类型关键字 + 标识符 + 左括号。对于 Rust，它检查 `fn` 关键字。对于 Python，它检查 `def` 和 `class`。

这种做法的好处是**极快**——没有正则编译开销，没有 AST 构建，就是纯粹的字符串操作。代价是**不精确**——会漏掉一些声明，也会误判一些非声明。但 Phase A 的定位是"快速预览"，不要求 100% 召回。

### Stub 检测

一个有趣的细节是 `stub` 检测。Phase A 只能在单行内判断一个函数是不是 stub（空实现）：

```cpp
// engine/src/engine_scanner.cpp: 约 1080 行
if ((kind == "function" || kind == "method") &&
    line.find("{}") != std::string::npos) {
    // 检查 {} 之间是否只有空白字符
    if (!has_content) {
        g_store->setSymbolStub(sym_id, true);
    }
}
```

多行 stub（左大括号在下一行）Phase A 检测不到，需要 Phase B 的 AST 解析才能覆盖。

### 增量扫描

Phase A 还支持 Git 增量扫描：

```cpp
// engine/src/engine_scanner.cpp
std::unordered_set<std::string> git_changed = getGitChangedFiles(dir);
```

通过 `git diff --name-only` 获取变更文件列表，只扫描有变化的文件。同时记录文件的 `mtime` 和 `size`，未变化的文件跳过。

---

## CST 到 IR：翻译器的设计

Phase B 的核心是 tree-sitter 解析 + IR 翻译。tree-sitter 输出的是 **CST（Concrete Syntax Tree）**——保留了所有语法细节，包括括号、分号、关键字。

CodeScope 的 IR 定义在 `engine/src/ir/ir.h`：

```cpp
// engine/src/ir/ir.h
enum class NodeKind : uint16_t {
    TranslationUnit, Module,          // 编译单元
    FunctionDecl, MethodDecl,         // 函数/方法
    ClassDecl, VariableDecl, FieldDecl, ParameterDecl,  // 声明
    EnumDecl, EnumMemberDecl, TypeAliasDecl,            // 枚举/类型
    MacroDecl, TemplateDecl, NamespaceDecl,             // 宏/模板/命名空间
    BlockStmt, IfStmt, ForStmt, WhileStmt,             // 语句
    CallExpr, BinaryExpr, MemberExpr, IdentifierExpr,  // 表达式
    // ... 共约 40 种
};

enum class Relation : uint8_t {
    Parent, Child, TypeRef, SymbolRef, CallTarget, Receiver, BaseClass
};
```

翻译器接口定义在 `engine/src/ir/ir_translator.h`：

```cpp
// engine/src/ir/ir_translator.h
class Translator {
public:
    virtual ~Translator() = default;
    virtual TranslationUnit *translate(TSTree *tree, const char *source,
                                       const char *file_path) = 0;
    virtual const char *language() const = 0;
};

// 工厂方法
Translator *createTranslator(const char *language);
```

`createTranslator()` 支持 10 种语言：Python, C++, C, Rust, JavaScript, TypeScript, Go, Java, Swift, TSX。

每种语言的翻译器实现在 `engine/src/ir/translators/` 目录下。它们通过 **tree-sitter visitor 模式**遍历 CST 节点，转换成统一的 IR。

比如 `c_visitor.h` 中的 C 语言翻译器，会把：

```c
int foo(int x) {
    return x + 1;
}
```

翻译成：

```
TranslationUnit
  └─ FunctionDecl "foo"
       ├─ ParameterDecl "x" (type: int)
       └─ BlockStmt
            └─ ReturnStmt
                 └─ BinaryExpr "+"
                      ├─ IdentifierExpr "x"
                      └─ LiteralExpr "1"
```

这个转换过程丢掉了很多 CST 细节——分号、括号、逗号——保留了语义结构。代价是**丢失了精确的源码位置信息**（比如分号的位置），但这对代码理解来说不是必须的。

---

## ResolverPipeline：约束链驱动的跨文件解析

IR 翻译是逐文件进行的。文件之间的调用关系怎么处理？这就是 Resolver（解析器）的工作。

最初版本有一个 `engine/src/linker/linker.h`，采用三趟 Pass 模式。经过重构，现已被 **`engine/src/resolver/pipeline.h`** 替代——一个基于约束链（constraint chain）的多因子评分管线。

```
旧 Linker（已删除）         新 ResolverPipeline（当前）
──────────────────────    ──────────────────────────
3 趟 Pass                 单趟 run() + applyConstraints()
整数加分                  9 因子加权评分（0.0-1.0）
无模糊匹配                FuzzyResolver 兜底
无可见性检查              语言级可见性规则（Go/Python/Java）
无 call kind 区分          CallKind 感知（Direct/Method/Interface/Constructor）
```

### 架构：两步走

`ResolverPipeline::run()` 的核心逻辑只有两步：

```
Step 0: 预加载 entity 表 → HashMap<name, vector<Candidate>>
        避免逐条 SQL 查询（之前的性能瓶颈）

Step 1: 遍历 reference 表 → 对每个引用:
  a. 从 HashMap 按名查找候选
  b. 零候选？ → FuzzyResolver 兜底（大小写/前缀/后缀）
  c. applyConstraints() → 9 因子加权评分
  d. 语言可见性硬过滤（如 Go 小写名不可跨包调用）
  e. 选最高分，阈值 0.4 以上写入 relation + graph_edges
```

### 九因子评分系统

```cpp
// engine/src/resolver/factors.h
constexpr double kWeightModuleMatch     = 0.15;  // 同模块
constexpr double kWeightImportMatch     = 0.80;  // 跨模块主导因子
constexpr double kWeightNamespaceMatch  = 0.10;  // 同命名空间
constexpr double kWeightSignatureMatch  = 0.10;  // 参数数量匹配
constexpr double kWeightDistanceMatch   = 0.05;  // 文件路径距离
constexpr double kWeightConstructorMatch = 0.10; // 构造函数匹配
constexpr double kWeightReceiverMatch   = 0.15;  // Go/Python receiver
constexpr double kWeightCommonNamePenalty = 0.10; // 常见名降权
constexpr double kWeightCallKindMatch   = 0.15;  // 调用类型感知
```

注意 `ImportMatch` 权重 **0.80**——这是跨模块解析的主导因子。如果目标名称在当前文件有 import 语句引用，基本可以确定。其他因子只在同模块或模糊匹配时起作用。

### CallKind 感知

从 `CallKind` 枚举看调用类型对解析的影响：

| 类型 | 值 | 行为 |
|------|----|------|
| `Direct` | 0 | 不应用 CallKind 因子 |
| `Method` | 1 | 略降分：方法调用通常在同模块 |
| `Interface` | 2 | 降分：接口派发更难解析 |
| `Constructor` | 3 | 加分：构造函数跨模块调用是合理的 |

### FuzzyResolver 兜底

当精确名称查找零候选时，`FuzzyResolver` 尝试三种模糊匹配：

- **大小写不敏感**（`getUser` ⇢ `getuser`）
- **前缀匹配**（`parseJSON` ⇢ `parse`）
- **后缀匹配**（`initDB` ⇢ `DB`）

每种匹配都有独立的评分衰减。找到候选后再进入 `applyConstraints()` 走完整评分流程。

### 语言可见性硬过滤

这是**硬规则**，不是加权因子——加权因子可以被其他因子覆盖，但语言规则必须绝对遵守：

- **Go**：小写字母开头的名称不可跨包调用
- **Python**：`_` 开头的名称视为私有
- **Java**：无 `public` 修饰符的为包内可见

这个设计参考了 codebase-memory-mcp 的 `cbm_is_exported()`，但实现为绝对拒绝而非打分。

### 写双表：relation + graph_edges

解析结果同时写入两张表：

- `relation` — 关系表（`source_id`, `target_id`, `type`, `confidence`）
- `graph_edges` — 兼容 CSR 格式的图边表

写双表是为了不同查询路径都能走索引，同时通过 `INSERT OR IGNORE` 保证幂等。

---

## GraphBuilder：从 IR 到图

GraphBuilder 定义在 `engine/src/graph/graph_builder.h`，核心是两套 API：

- 旧 API（`TranslationUnit`/`Node*` 树）：用于非 JS 语言
- 新 API（`SemanticUnit`/flat records）：用于 JS/TS（因为 JS/TS 的 visitor 输出已经是 flat records）

图节点的类型定义在 `engine/src/graph/graph_types.h`：

```cpp
// engine/src/graph/graph_types.h
enum class NodeType : uint8_t {
    Function, Method, Class, Struct, Interface, Variable, Macro, Module, File
};

enum class EdgeType : uint8_t {
    References, Calls, Defines, Contains, Imports, Inherits
};

struct GraphNode {
    uint64_t id, ir_node_id;
    NodeType type;
    std::string name, qualified_name, module_path, file_path, language, signature;
    uint32_t start_row, start_col, end_row, end_col;
    int complexity;          // 圈复杂度
    bool is_entry_point;
};

struct GraphEdge {
    uint64_t id, source_id, target_id;
    EdgeType type;
    std::string graph_type;  // "call_graph" | "symbol_reference"
    std::string call_site_file;
    int call_site_line;
    std::string label;       // "async" / "virtual" / "override"
};
```

GraphBuilder 使用 Visitor 模式遍历 IR 树，对每种 NodeKind 产生不同的图节点。一个关键优化是 **parent chain cache**——避免在插入子节点时反复查找父节点的 ID。

---

## ComplexityAnalyzer：不只是圈复杂度

复杂度分析实现在 `engine/src/ir/ir_complexity.cpp`：

```cpp
// engine/src/ir/ir_complexity.h
struct ComplexityResult {
    uint64_t cyclomatic = 0;      // McCabe 圈复杂度
    uint64_t cognitive = 0;       // 认知复杂度 (v2)
    uint64_t nesting_depth = 0;   // 最大嵌套深度
    uint64_t decision_points = 0; // 决策点计数
};
```

计算规则：

- **圈复杂度** = 1 + 决策点数量（`if/for/while/do-while/switch-case/catch/ternary`）
- **认知复杂度** = 决策点数量 + 嵌套深度加权
- **嵌套深度** = 控制流嵌套的最大层数

一个已知限制：当前版本**不计算 `&&` 和 `||` 运算符**作为决策点，因为需要检查运算符类型时需要查看源码文本，而纯 IR 遍历做不到。这在代码注释中有标注，是未来的改进方向。

---

## 查询层：自适应引擎

全部分析完成后，数据落入 SQLite。查询引擎定义在 `engine/src/engine_queries.cpp`（1052 行），提供：

- `engine_get_module_tree()` — 获取模块树
- `engine_find_symbol()` — 符号搜索（带智能提示）
- `engine_find_definition()` — 定义查找
- `engine_find_callers()` / `engine_find_callees()` — 调用图查询
- `engine_get_entry_points()` — 入口点
- `engine_get_complexity()` — 复杂度
- `engine_get_graph_stats()` — 图统计
- `engine_get_index_progress()` — 索引进度

查询引擎具备**自适应能力**：根据当前索引完成度，决定返回的信息量。Phase A 完成后只能返回符号列表，Phase B 完成后可以返回定义和引用，Phase C 完成后才能返回完整的调用图和 FTS 搜索。

---

## 坦诚反思

这个引擎设计有一些明显的取舍，值得坦白说：

**1. Phase A 的精度问题**

逐行扫描一定会漏掉声明。比如 C 语言中返回复杂类型的函数声明跨越多行：

```c
const struct very_long_type_name *
foo(int x, int y)
```

Phase A 无法识别这种声明——`detectDecl()` 看到 `const` 只是类型修饰符，下一行 `foo(` 没有类型关键字前缀，所以跳过了。Phase B 的 AST 解析能覆盖，但 Phase A 返回的数据就不完整。

**2. 跨文件调用的存根模式**

`ResolveCallPass` 创建的存根（stub）IR 节点，只包含函数名和文件路径，没有参数签名。这意味着如果两个同名的函数在不同文件中，排名启发式可能选错。在我们的实测中，同名函数在大型 C 项目（如 Linux 内核）中很常见，存根的准确率在 70% 左右。

**3. 复杂度分析器的局限性**

不计算 `&&`/`||` 运算符，意味着某些条件表达式的复杂度被低估了。比如：

```c
if (a && b && c && d && e)  // 实际有 5 个决策点，但只算 1 个
```

这对认知复杂度的计算影响更大——因为 IR 中无法区分 `&&` 和 `||`，也就无法应用"每个条件运算符 +1"的规则。

**4. ResolverPipeline 的执行效率**

当前的 `run()` 会预加载整个 entity 表到 HashMap 中。对于大型项目（百万级 entity），这个 HashMap 可能占用数百 MB 内存。虽然有 SQL 索引和批量写入优化，但全量扫描 reference 表 + 逐条 `applyConstraints()` 仍然是索引阶段最耗时的部分。实测在 Linux 内核规模下，解析耗时约占全索引时间的 40%。

`INSERT OR IGNORE INTO relation` 中的 `WHERE NOT EXISTS` 子查询也增加了写开销——这是为了过滤 test/bench 文件的调用边，避免被测试代码污染调用图。

---

## 系列导航

| 文章 | 主题 |
|---|---|
| (一) 开篇 | 56KB vs 629 bytes，CodeScope 要解决什么问题 |
| (二) 渐进式就绪 | 毫秒级让 AI 开始理解你的代码 |
| (三) Worker 隔离 | 为什么索引不会拖垮 MCP Server |
| (四) 零冗余响应 | 精简响应，按需返回 |
| **(五) C++ 引擎拆解** | **从源码到多维代码图的管线 ← 本文** |
| (六) MCP 协议层 | 35+ 工具的设计哲学 |
| (七) 语言翻译器 | 10 种语言 → 统一 IR |
| (八) 存储层 | SQLite WAL + FTS5 + vec0 |
| (九) 自适应查询 | Fallback 机制与就绪检测 |
| (十) 性能真相 | 从 200 到 60,000 文件的实测 |
| (十一) 验证层 | 让 AI 对自己的话负责 |
| (十二) Model Engine | 从事实到理解 |
| (十三) Parser + GraphBuilder | 解析与建图 |

---

下一篇我们将拆解 **MCP 协议层**——看看 35+ 个工具是怎么组织、路由、和自描述的。如果一个工具返回错误，AI 怎么知道下一步该调用哪个？
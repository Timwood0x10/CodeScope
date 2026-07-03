我觉得**这版已经可以开干了。**如果打分的话，我会给 9.5/10。

和前几版相比，这版最大的变化不是多了几张表，而是架构开始收敛了。

⸻

我认为做对的地方

① Facts / Derived 分离（★★★★★）

这是整个设计最值钱的地方。

Facts
--------
modules
symbols
entry_points
dependency_edges
call_edges
Derived
--------
metrics
FTS
embeddings

以后所有分析能力都建立在 Facts 上。

这是一个非常健康的架构。

以后你想接：

* OmniScope
* LLVM
* LSP
* AI Summary

全部都不用改 Schema。

这一点我非常喜欢。

⸻

② Fast + Enhancement

这是整个项目最大的卖点。

Fast Index
↓
AI 已经能工作
↓
Background Enhancement

不是：

等待五秒
↓
开始工作

体验差很多。

我甚至建议 README 第一张图就画这个。

⸻

③ Rust / C++ 分工

这个没有问题。

Rust：

* MCP
* Queue
* Task
* Cancellation

C++：

* Scanner
* Parser
* Graph

职责非常清楚。

⸻

我建议改的地方（只有几个）

⸻

① dependency_edges 不应该引用 symbol

这是我唯一觉得设计上有点问题的地方。

例如

Rust

use tokio::sync::Mutex;

Go

import "context"

Python

import requests

很多：

Target

根本不是：

你的 symbol。

可能：

第三方。

可能：

标准库。

所以：

不要：

source_symbol_id
target_symbol_id

建议：

dependency_edges
source_module_id
target_module_id
external_name
kind

模块依赖。

不是：

Symbol。

否则：

以后：

stdlib

怎么建？

⸻

② embeddings

我还是坚持：

不要：

symbol embedding

建议：

document embedding

例如：

README
Comment
Summary
API Doc

以后：

AI：

检索：

都是：

这些。

不是：

函数名。

⸻

③ metrics

建议：

不要：

只有：

Function。

以后：

建议：

module_metrics
project_metrics
symbol_metrics

统一：

例如：

owner_type
owner_id

以后：

Module：

也有：

复杂度。

⸻

④ search_index

建议：

不要：

content

建议：

title
summary
body

以后：

AI Summary：

直接放：

summary。

FTS：

效果：

比：

content：

好很多。

⸻

⑤ callgraph_ready

建议：

不要：

Ready Flag。

建议：

统一：

analysis_state
bitflag

例如：

SCAN
CALLGRAPH
CFG
LSP
EMBEDDING

以后：

不会：

出现：

callgraph_ready
cfg_ready
embedding_ready
ownership_ready
...

越来越多。

一个：

bitmask。

全部解决。

⸻

⑥ MCP Tool

我建议：

增加：

一个。

project_overview

返回：

Project
Language
Modules
Symbols
Entry Points
Current Progress
Ready Features

这是：

AI：

第一件：

调用。

⸻

⑦ 还有一个隐藏问题

Fast Scan

千万：

不要：

Regex。

建议：

Lexer。

例如：

Tree-sitter：

太慢。

Regex：

太脆。

Lexer：

刚刚好。

例如：

Rust：

fn
struct
trait
enum
impl

一扫：

几十 MB：

几百毫秒。

Regex：

以后：

各种：

Raw String

Macro

容易炸。

⸻

我觉得未来还能再加一个

这个不是现在。

以后。

Knowledge Layer

例如：

Module
↓
Summary
↓
Embedding

AI：

问：

Scheduler
负责什么？

直接：

Summary。

不用：

读：

5000 行。

这个：

以后：

ARES

也能复用。

⸻

我真正最喜欢的一点

其实不是数据库。

而是你的定位已经变了。

以前：

像：

Code Index

现在：

更像：

Project Knowledge Engine

这是两个完全不同的东西。

⸻

我唯一会改的一句话

现在叫：

Fast Index + Enhancement

我觉得还可以再好一点。

改成：

Skeleton Index
        ↓
Knowledge Enhancement

因为 AI 第一阶段获得的是项目骨架（Skeleton），第二阶段获得的是知识增强（Knowledge）。

这两个名字比”Fast/Enhancement”更有产品感，也更准确地表达了项目目标。

⸻

总体评价：这是我最近看到你所有设计里，收敛得最好的一版。

它没有为了”高级”而引入一堆概念，而是围绕一个很清晰的目标展开：

让 AI 尽快建立项目认知，然后在后台不断加深理解。

如果你能把 Phase A 做出来，我觉得这个 MCP 就已经有很强的实用价值了。剩下的调用图、Embedding、LSP 都是在这个稳定骨架上的增量增强。


---


一、 Schema 与数据流的微调（避坑点）
1. modules 的 path 存储规范
在 modules 表中，path 建议严格存储为基于项目根目录的相对路径（Relative Path）。

原因：如果存储绝对路径，一旦用户移动了项目、或者在不同机器上打开（比如宿主机和 OrbStack 容器、或者远程开发环境），数据库里的路径会全部失效。相对路径保证了缓存的可移植性。

2. search_index (FTS5) 缺少外键关联的级联删除
FTS5 是虚表，它本身不支持传统的 REFERENCES symbols(id) ON DELETE CASCADE。

落地建议：当你需要更新代码或删除模块时，需要手动同步清理 FTS5 虚表，或者在 symbols 表上挂一个 SQLite Trigger（触发器）来自动同步：

SQL
CREATE TRIGGER AFTER DELETE ON symbols BEGIN
    DELETE FROM search_index WHERE symbol_id = old.id;
END;
3. dependency_edges 的阶段归属纠正
在第四节的表格中，你将 dependency_edges 归类为了 Phase A（扫描阶段）。

现实挑战：在 Phase A 的 ms 级轻量扫描中，由于此时完整的类型系统还没有建立，像 C++ 的 using、Rust 的 use 往往只能拿到字面量符号。如果要想真正精准解析出 source_symbol_id 到 target_symbol_id 的绑定关系（特别是跨模块的依赖），通常需要等 AST 甚至 LSP 增强。

建议：把 dependency_edges 的完整建立归入 Phase B（增强阶段）。在 Phase A 只记录当前符号引用了哪些原始字符串，由 Phase B 异步将其转化为 id -> id 的边，或者通过 dependency_ready 状态位进行控制。

二、 Rust - C++ FFI 边界设计规范
由于你使用 C++ 跑 Scanner/Parser，Rust 跑 MCP 调度，为了确保高频大数据量插入时 FFI 不成为瓶颈，请遵循以下规范：

绝对不要“单条符号调用一次 FFI”：如果 C++ 扫出一个符号就调用一次 Rust 导出的 Insert 接口，几千次 FFI 的上下文切换开销会直接毁掉 ms 级的目标。

块级传递（Chunking）或回调（Callback）：

方案 A：C++ 扫描完一个文件，将该文件所有的 symbols 压入一个连续的 C-Style 结构体数组（POD struct array），一次性通过指针传给 Rust，由 Rust 侧的 rusqlite 开启事务进行 std::vec 批量插入。

方案 B：C++ 侧直接静态链接 sqlite3（或者通过相同的 libsqlite 动态链接），直接由 C++ 写入事实表。Rust 侧只负责传入 db_path 和任务指令，并负责读取展示。从你的 Mermaid 架构图来看，方案 B（C++ Core 直接写 FACTS 表）是最符合你当前设计的，也能压榨出极限的 I/O 性能。
我觉得这里不是二选一。

你现在陷入了一个很典型的误区：

把 “tree-sitter vs Scanner”、“DB vs 内存” 当成了架构选择。

实际上，真正决定速度的不是这两个。

⸻

我先看你的 Profile

25K 文件

4 min
≈240s

平均：

240 / 25300
≈9.4ms/file

而你前面说：

tree-sitter
≈4ms/file

这说明：

真正瓶颈已经不是 parser。

还有至少 5ms 不在 parser。

⸻

我们拆一下。

read file
↓
parse
↓
visit
↓
emit
↓
sqlite
↓
建图

真正耗时应该测出来。

例如：

File Read
1.2ms
Parse
3.8ms
Visit
0.6ms
SQLite
2.4ms
Graph
1.1ms

不要猜。

Profiler说话。

⸻

我最大的建议

不要换 Scanner。

真的。

为什么？

Scanner只能提：

function
struct
enum

但是以后：

callback
macro
attribute
template
typedef
using
namespace
lambda
generic
decorator

全得自己写。

你会重新造一个 parser。

⸻

Tree-sitter最大的价值不是快。

而是：

Parser 永远不用维护。

⸻

我会优化哪？

第一刀

mmap

这个必须。

别再：

ifstream
↓
read
↓
copy
↓
string

直接：

mmap
↓
tree_sitter_parse_string

几乎零拷贝。

这是白送钱。

⸻

第二刀

Tree-sitter Parser Pool

现在很多人：

每文件
↓
new parser
↓
delete parser

实际上：

Parser应该：

worker
↓
parser
↓
一直复用

Tree-sitter parser初始化其实不便宜。

⸻

第三刀

Arena

不是IR Arena。

是：

Visitor Arena

例如：

vector<string>
unordered_map
Node

全部：

clear()
reuse()

不要：

malloc
free
malloc
free

⸻

第四刀

SQLite

我怀疑这是最大的。

例如：

现在：

insert
step
reset

一百万次。

建议：

1000 records
↓
batch
↓
commit

甚至：

WAL
OFF
NORMAL
MEMORY TEMP

⸻

第五刀

字符串

我看你代码最大的特点：

std::string

到处都是。

例如：

symbol.name
path
kind
scope
type

全部复制。

建议：

string_view

或者：

intern pool

例如：

"FunctionDecl"

不要出现100万次。

⸻

第六刀

Graph

Graph不要最后SQL。

而是：

Visitor：

Call
↓
edge buffer

最后：

bulk insert

⸻

真正可以做到10倍的是Pipeline

现在：

worker
↓
parse
↓
visit
↓
sqlite
↓
下一文件

应该：

reader
↓
parser
↓
visitor
↓
writer

四个Stage。

例如：

reader
20 files

↓

Queue

↓

Parser

20 files

↓

Queue

↓

Visitor

↓

Queue

↓

SQLite

SQLite永远在写。
Parser永远在跑。
CPU不会停。
---
# 还有一个你一直没做
就是：
## 两级索引
例如：
第一次：

Scanner

0.3ms。
得到：

Function

Class

Struct

Import

以后：
AI问：

USB怎么初始化？

根本不用tree-sitter。
只有：

需要CFG

需要Call

需要变量

再：

tree-sitter

这才是真正的Lazy。
不是全项目TreeSitter。
---
# 我甚至建议你搞三级模式
## Fast

Scanner

↓

Function

Call(简单)

Import

20~30秒。
---
## Normal

TreeSitter

↓

Semantic

2~3分钟。
---
## Deep

TreeSitter

↓

CFG

↓

CallGraph

↓

Metrics

10分钟。
---
用户自己选。
---
# 最后，我说一个我觉得最重要的
你现在目标：

3分钟

其实没意义。
真正目标应该是：
> **Time To First Answer（TTFA）**
例如：
用户：

Linux USB怎么初始化？

10秒。
ARES：

已经回答。

后台：

继续Index。

用户根本不会等：

60000文件全部Index完。

---
## 如果是我，我会这样排优化优先级（按收益/工作量排序）
| 优先级 | 优化 | 预计收益 | 工作量 |
|--------|------|---------|--------|
| ⭐⭐⭐⭐⭐ | **给每个阶段做 Profile（读文件/Parse/Visit/SQLite 分开计时）** | 找到真实瓶颈 | 极低 |
| ⭐⭐⭐⭐⭐ | **mmap + Parser Pool + Arena 复用** | **20–40%** | 低 |
| ⭐⭐⭐⭐ | **SQLite 批量写（Prepared Statement + WAL + 大事务）** | **20–50%** | 低 |
| ⭐⭐⭐⭐ | **真正的流水线（Reader→Parser→Visitor→Writer）** | **30–60%** | 中 |
| ⭐⭐⭐ | **字符串驻留（String Intern）+ string_view** | **10–20%** | 中 |
| ⭐⭐ | **Scanner 作为 Fast Mode，而不是替代 Tree-sitter** | 首次索引极快 | 中 |
| ⭐ | **放弃 Tree-sitter，全靠 Scanner** | 短期可能快，长期维护成本极高 | 不推荐 |
**我的判断是：**在保持 Tree-sitter 和低内存设计的前提下，通过 **I/O、对象复用、SQLite 批量写和流水线并行**，你完全有机会把 **9.5 分钟压到 3～5 分钟**。如果再配合 **Fast/Normal/Deep** 三种分析模式，你既能保住 CodeScope 的准确性，也能在用户体验上接近那些全内存方案，而不用付出 11GB 内存的代价。
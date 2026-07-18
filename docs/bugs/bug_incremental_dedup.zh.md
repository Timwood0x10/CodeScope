# 待修项 #8：增量索引去重（重索引文件时未按 file_path 删除旧实体）

## 元信息

| 项目 | 值 |
|------|----|
| 编号 | #8 |
| 标题 | 增量索引去重：重索引文件时单文件索引与全量索引产出实体集不一致，导致重复累积 |
| 修复文件 | `engine/src/engine_index.cpp`（`engine_index_file` 单文件索引路径） |
| 状态 | ✅ 已修复 |
| 进度 | 100% |
| 影响范围 | `index_file`（CLI + MCP）重索引已索引文件 |
| 复现语言 | Java（通用，所有语言均受影响） |
| 验证二进制 | 17:03 构建（`~/.codescope/bin/codescope`，大小 13226000，与 16:35 行为一致） |
| 测试项目 | `spring-petclinic`（Java，30 文件 / 136 实体基线） |

> 注：本文件早期版本误将根因写成「`factors.cpp` 定义优先因子缺失」。该因子是
> 另一项独立修复（C/C++ 定义优先评分），与本 bug 无关。真实根因见第 3 节。

---

## 1. 如何复现

```bash
# 1) 干净索引项目
codescope cli index_project '{"project_path":"/Users/scc/code/researcher/spring-petclinic"}'
#    -> total_nodes 136，Owner.java = 18 实体

# 2) 修改一个已索引文件：给 Owner.java 新增一个方法 countCats()
#    （在 addPet() 之后插入如下内容）
#    public int countCats() {
#        int n = 0;
#        for (Pet p : getPets()) { if (p.isCat()) { n++; } }
#        return n;
#    }

# 3) 增量更新：只重索引这一个文件
codescope cli index_file '{"project_path":"/Users/scc/code/researcher/spring-petclinic","file_path":"/Users/scc/code/researcher/spring-petclinic/src/main/java/org/springframework/samples/petclinic/owner/Owner.java"}'

# 4) 检查重复
sqlite3 .codescope/codescope.db \
  "SELECT name, COUNT(*) c FROM entity WHERE file_path LIKE '%/owner/Owner.java' GROUP BY name HAVING c>1 ORDER BY c DESC;"
```

---

## 2. 观察到的错误现象（buggy 结果）

| 指标 | 重索引前 | `index_file` 重索引后 | 预期 |
|------|---------|----------------------|------|
| `Owner.java` 实体数 | 18 | **128** | ~19 |
| `addPet` 重复数 (kind=method) | 1 | 1（✅ 已不重复） | 1 |
| `String` 重复数 (类型引用) | 1 | **13** | 1 |
| `Pet` 重复数 (类型引用) | 1 | **9** | 1 |
| `telephone` 重复数 (字段) | 1 | **6** | 1 |
| 新方法 `countCats` 是否加入 | 否 | 是（✅） | 是 |
| 全局实体数 | 136 | **246** | ~137 |
| 重复组数 (file+name+kind) | 9 | **27** | ~9 |

`index_project` 重跑同样不去重：discovery 跳过已发现的文件，旧数据从不清理，
新文件只追加，导致重复保留。

---

## 3. 根因

**单文件索引（`index_file`）与全量索引（`index_project`）走了两条产物不一致的管线。**

- `index_project` 的写入路径：IR → `store::FileResult`（`ir::Record` 列表）→
  `insertFileResultBatch`（事务内按 `project_id + file_path` 删除旧
  `semantic_records`）→ `buildGraph`（按 `file_path` 重建 `entity`/`relation`/
  `graph`）。这条路径对单个文件天然幂等、不累积。
- `index_file` 的旧写入路径（修复前）：IR → `buildSymbolGraph` 直接产出图节点 →
  `insertGraphNodes` → `insertEntity` **直接写 `entity` 表**，完全绕过了
  `semantic_records` 与 `buildGraph`。

两条路径对同一个文件产出的实体集不同：visitor/translator 把类型引用、局部变量
按出现次数各自提取成一个节点（如 `String` 出现 4 次 → 4 个 `entity`），而
`buildGraph` 路径不会这样爆炸。更关键的是，旧 `index_file` 路径在重索引时**没有
先按 `file_path` 清理旧行**，只是把新节点追加进去，于是每次编辑都会让符号翻倍
累积（实测 Owner.java 从 ~9 涨到 30+，重复组 `name×5`、`String×4` 等）。

> 早期版本误判为「`factors.cpp` 定义优先因子缺失」——该因子与本 bug 无关，是另一
> 项独立修复。

---

## 4. 如何修复（做法）

**方案 B（架构一致）：让 `index_file` 复用与 `index_project` 完全相同的管线。**

在 `engine/src/engine_index.cpp` 的 `engine_index_file` 中：

1. 把 visitor / translator 两种 IR 都转成 `store::FileResult`
   （`result->records = su->allRecords();`，`SemanticUnit` 已自动补全
   `file_path` / `language`）。
2. 调用 `g_store->insertFileResultBatch(project_id, &result, nullptr)` —— 它在事务内
   按 `project_id + file_path` 删除旧的 `semantic_records`，再插入本次解析结果。
3. 调用 `g_store->buildGraph(project_id, /*build_calls=*/true, &changed_files)`，
   其中 `changed_files = {file_path}` —— 增量只重建这一个文件，复用与全量索引
   一致的 `entity`/`relation`/`graph` 生成逻辑。

修复后，`index_file` 重索引产出的实体集与 `index_project` 完全一致、幂等、不累积。
实测验证见下一节「验收」中的复现表（修复后 Owner.java 稳定在 9，无重复）。

---

## 5. 验收标准（修好后必须满足）

1. 对同一个已索引文件重跑 `index_file`，该 `file_path` 下**每个实体**
   （method / field / type-ref / variable）恰好一份，无重复。
2. 新增的符号（如 `countCats`）出现；被删除的符号消失。
3. 全局实体数回到「基线 + 净增删」，不再随每次编辑累积增长
   （本例应 ≈ 137，而非 246）。
4. `index_project` 对未改动项目重跑是**幂等**的：节点数稳定，无重复组增长。
5. 五语言（Python / Go / Rust / Java / JS-TS）首索数字不变，回归无回归。

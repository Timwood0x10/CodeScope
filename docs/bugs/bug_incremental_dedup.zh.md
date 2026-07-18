# 待修项 #8：增量索引去重（重索引文件时未按 file_path 删除旧实体）

## 元信息

| 项目 | 值 |
|------|----|
| 编号 | #8 |
| 标题 | 增量索引去重：重索引文件时未按 `file_path` 删除旧实体再整体插入 |
| 因子文件 | `factors.cpp`（定义优先因子缺失） |
| 状态 | ❌ 没修 |
| 进度 | 0% —— 仍无该因子 |
| 影响范围 | `index_file`（CLI + MCP）重索引已索引文件；`index_project` 重跑 |
| 复现语言 | Java（通用，所有语言均受影响） |
| 验证二进制 | 17:03 构建（`~/.codescope/bin/codescope`，大小 13226000，与 16:35 行为一致） |
| 测试项目 | `spring-petclinic`（Java，30 文件 / 136 实体基线） |

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

`factors.cpp` 中的「定义优先因子」缺失。重索引时引擎只做**插入**，没有先按
`file_path` 删除该文件的旧实体/关系。

- 部分缓解已落地：仅对 `kind = method` 做了 upsert（`addPet` 不再重复）。
- 但**字段 (field)、类型引用 (type-ref)、局部变量 (variable)** 仍被无条件追加，
  每次编辑都会让这些符号翻倍累积。

正确行为应是：重索引某文件时，先 `DELETE` 该 `file_path` 下的所有 `entity` +
`relation`（及依赖表）行，**再**整体插入本次解析结果 —— 即以 `file_path` 为键的
真正 upsert。

---

## 4. 如何修复（做法）

在 `index_file` / `index_project` 的「写入单文件」路径里，插入前先做按 `file_path`
的清理：

1. 在事务内，对目标 `file_path` 执行：
   - `DELETE FROM relation WHERE source_id IN (SELECT id FROM entity WHERE file_path = ?) OR target_id IN (SELECT id FROM entity WHERE file_path = ?);`
   - `DELETE FROM entity WHERE file_path = ?;`
   - （如有 `module_state` / `module_path` / FTS 映射等依赖表，同样按 file_path 清理）
2. 再插入本次解析出的实体与关系。
3. `index_project` 重跑时复用同一「按 file_path 清理再插入」逻辑，使重跑幂等。

注意：`factors.cpp` 的定义优先因子应统一承载「重索引 = 先删后插」的判定，而非
只在 method 分支特殊处理。

---

## 5. 验收标准（修好后必须满足）

1. 对同一个已索引文件重跑 `index_file`，该 `file_path` 下**每个实体**
   （method / field / type-ref / variable）恰好一份，无重复。
2. 新增的符号（如 `countCats`）出现；被删除的符号消失。
3. 全局实体数回到「基线 + 净增删」，不再随每次编辑累积增长
   （本例应 ≈ 137，而非 246）。
4. `index_project` 对未改动项目重跑是**幂等**的：节点数稳定，无重复组增长。
5. 五语言（Python / Go / Rust / Java / JS-TS）首索数字不变，回归无回归。

<!--
CodeScope 代码审查模板 —— 对应 CODE_REVIEW_STANDARD.md
作者开 PR 时逐域自检并勾选；reviewer 按 §3 定级。
-->

## 变更说明
**为什么改（动机）：**

**关联 issue：** (如 #123)

**自测说明（怎么验证、看到什么）：**

---

## 代码审查自检（作者勾选，对应 CODE_REVIEW_STANDARD.md §2）

- [ ] **§2.1 内存/资源**：裸指针已判空、`std::string::c_str()` 未越生命周期、句柄全路径关闭、所有权清晰（值类型不可拷贝已 `= delete` 或注释）
- [ ] **§2.2 FFI**：`unsafe` 入参已判空、返回的 `*mut c_char` 释放权唯一、FFI 层无裸拼 SQL（走 `GraphStore` 方法）
- [ ] **§2.3 SQLite**：单句柄已串行化（`SQLITE_CONFIG_SERIALIZED`/mutex/连接池）、已设 `busy_timeout`、事务边界清晰
- [ ] **§2.4 JSON**：对外输出经转义/结构化构造、键唯一、解析外部输入坏数据不杀服务
- [ ] **§2.5 跨平台**：新增 POSIX API 有 Windows 分支或 `#error`、路径用 `std::filesystem`、shell 调用无 shell 或严格校验元字符
- [ ] **§2.6 性能**：性能改动已附 before/after benchmark、无未测量声明；无新增全表扫描/N+1 且无规模上限说明
- [ ] **§2.7 测试**：核心逻辑（调用图/语义搜索/JSON/FFI 边界）改动已配单测或回归测试

---

## 审查要求（reviewer 参考 CODE_REVIEW_STANDARD.md §4.5）
- 核心模块（FFI / store / parser / graph builder / lsp）：需 **2 approval**（含 1 名模块负责人，见 CODEOWNERS）
- 安全相关：需 **2 approval**（含安全复核）
- 性能相关：**1 approval + benchmark 数据**

## 严重程度速查
🔴 Blocker（合入前必修）· 🟠 Major（应修，可建 issue 跟踪）· 🟡 Minor（可选）

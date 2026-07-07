# Grammars 编译方案

## 问题背景

CodeScope 使用 tree-sitter 解析多种编程语言。`grammars/*.so` 文件是编译后的语法库，运行时通过 `dlopen` 加载。

### 原有问题

1. **依赖 npm 包名不正确** - `npm install -g tree-sitter-python` 可能失败
2. **缺少编译步骤** - 仅仅安装 npm 包无法生成 `.so` 文件
3. **CI 失败风险高** - 使用 `2>/dev/null || true` 隐藏错误

---

## 解决方案

### 方案 1：使用 tree-sitter CLI（推荐）

**CI 配置：**
```bash
# 安装官方 CLI
npm install -g tree-sitter-cli

# 编译语法
cd grammars
bash build_ci.sh
```

**优点：**
- ✅ 官方推荐方式
- ✅ 可靠性高
- ✅ 支持所有语言

**缺点：**
- ⚠️ 编译耗时（每个语法 ~30秒）
- ⚠️ 需要网络下载源码

---

### 方案 2：预编译 + Git Submodules

**结构：**
```
grammars/
├── prebuilt/
│   ├── linux-x86_64/
│   │   ├── tree-sitter-python.so
│   │   ├── tree-sitter-c.so
│   │   └── ...
│   ├── macos-arm64/
│   │   ├── tree-sitter-python.so
│   │   └── ...
│   └── macos-x86_64/
│       └── ...
├── tree-sitter-python/  # Git submodule
├── tree-sitter-c/       # Git submodule
└── build.sh
```

**CI 配置：**
```yaml
- name: Checkout submodules
  run: git submodule update --init --recursive

- name: Build grammars
  run: |
    cd grammars
    bash build.sh --platform ${{ matrix.target }}
```

**优点：**
- ✅ 无需网络下载
- ✅ 编译速度快（有缓存）
- ✅ 版本控制精确

**缺点：**
- ⚠️ 仓库体积增大
- ⚠️ 需要维护多平台预编译文件

---

### 方案 3：容器化编译（最可靠）

**Dockerfile：**
```dockerfile
FROM ubuntu:22.04

RUN apt-get update && \
    apt-get install -y cmake gcc git nodejs npm && \
    npm install -g tree-sitter-cli

WORKDIR /grammars
COPY build_ci.sh .

RUN bash build_ci.sh

VOLUME /output
CMD cp *.so /output/
```

**CI 配置：**
```yaml
- name: Build grammars (Docker)
  run: |
    docker build -t codescope-grammars grammars/
    docker run --rm -v $PWD/grammars:/output codescope-grammars
```

**优点：**
- ✅ 环境完全一致
- ✅ 无需处理平台差异
- ✅ 可复用编译结果

**缺点：**
- ⚠️ Docker 构建开销

---

## CI 支持的平台

### 自动编译（CI）

| 平台 | 编译脚本 | 输出文件 | CI 状态 |
|------|---------|----------|---------|
| **Linux (ubuntu-22.04)** | build_ci.sh | *.so | ✅ 自动编译 |
| **macOS (macos-14)** | build_ci.sh | *.so | ✅ 自动编译 |

### 手动编译（Windows）

| 平台 | 编译脚本 | 输出文件 | 状态 |
|------|---------|----------|------|
| **Windows** | build_win_guide.sh → build_ci.ps1 | *.dll | ⚠️ 用户自行编译 |

---

## Windows 手动编译指南

### 1. 准备环境

运行环境准备脚本：
```bash
# Git Bash 环境下运行
cd grammars
bash build_win_guide.sh
```

脚本会检查并提示缺失的依赖：
- Git
- GCC (MinGW-w64)
- Node.js
- tree-sitter CLI

### 2. 安装依赖（如缺失）

**PowerShell (Chocolatey):**
```powershell
choco install -y git mingw nodejs
npm install -g tree-sitter-cli
```

**手动下载:**
1. Git: https://git-scm.com/download/win
2. MinGW-w64: https://www.mingw-w64.org/
3. Node.js: https://nodejs.org/
4. tree-sitter: `npm install -g tree-sitter-cli`

### 3. 编译 grammars

**PowerShell (推荐):**
```powershell
cd grammars
.\build_ci.ps1              # 完整编译
.\build_ci.ps1 -Languages python,c  # 指定语言
```

**Git Bash:**
```bash
cd grammars
bash build_ci.sh            # 会自动检测 Windows 环境
```

### 4. 验证编译结果

```powershell
dir *.dll                   # PowerShell
```

```bash
ls -lh *.dll                # Git Bash
```

**预期输出文件：**
- tree-sitter-python.dll
- tree-sitter-c.dll
- tree-sitter-cpp.dll
- tree-sitter-rust.dll
- tree-sitter-javascript.dll
- tree-sitter-typescript.dll
- tree-sitter-tsx.dll
- tree-sitter-go.dll
- tree-sitter-java.dll

---

## CI 工作流程

### Build & Test (build.yml)
```yaml
支持平台: Linux (ubuntu-22.04) + macOS (macos-14)

步骤：
1. Install dependencies (apt/brew + tree-sitter CLI)
2. Build grammars → bash build_ci.sh → *.so
3. Build engine → cmake + ninja
4. Build server → cargo build --release
5. Run tests → cargo test
6. Upload artifacts → codescope + grammars/*.so
```

### Release (release.yml)
```yaml
支持平台: Linux + macOS

步骤：
1. Build per platform → *.so
2. Package with grammars → tar.gz
3. Generate checksums
4. Create GitHub Release

注意: Windows 用户需要手动编译 .dll 文件
```

---

## Windows 用户注意事项

### 为什么 Windows 不在 CI 中自动编译？

1. **编译环境复杂** - Windows 需要额外的 MinGW-w64 设置
2. **CI 时间过长** - Windows 编译耗时增加 CI 运行时间
3. **失败风险高** - Windows 环境差异导致编译失败概率较高
4. **用户可控** - Windows 用户可以根据自己的环境灵活调整

### Windows 用户的责任

- ✅ 自行准备编译环境（Git + MinGW + Node.js）
- ✅ 运行 `build_win_guide.sh` 检查环境
- ✅ 使用 `build_ci.ps1` 或 `build_ci.sh` 编译
- ✅ 确保 `.dll` 文件与 `codescope.exe` 在同一目录

---

## 常见问题

### Q1: 编译失败怎么办？

**检查项：**
1. tree-sitter CLI 是否安装成功
2. Git 是否可以克隆语法仓库
3. GCC/Clang 是否可用
4. 检查具体错误日志

**解决方法：**
```bash
# 手动测试
cd grammars
bash build_ci.sh 2>&1 | tee build.log

# 检查单个语法
tree-sitter generate  # 在语法目录内
gcc -fPIC -shared src/parser.c -I src -o test.so
```

### Q2: 如何加速编译？

**优化策略：**
1. **缓存** - 使用 `actions/cache` 缓存编译结果
2. **并发** - 并行编译多个语法
3. **预构建** - 在本地预编译并提交

**缓存配置：**
```yaml
- name: Cache grammars
  uses: actions/cache@v3
  with:
    path: grammars/tree-sitter-*
    key: grammars-${{ hashFiles('grammars/build_ci.sh') }}
```

### Q3: 不同平台的 .so 兼容性？

**关键点：**
- Linux: `.so` 文件（ELF 格式）
- macOS: `.so` 文件（Mach-O 格式，实际是 `.dylib`）
- Windows: `.dll` 文件（PE 格式）

**解决方案：**
```bash
# Linux/macOS (build_ci.sh)
gcc -fPIC -shared src/parser.c -I src -o tree-sitter-python.so

# macOS 需要特殊处理
gcc -dynamiclib -o tree-sitter-python.dylib src/parser.c -I src

# Windows PowerShell (build_ci.ps1)
gcc -shared -o tree-sitter-python.dll src\parser.c -Isrc

# Windows Batch (build_ci.bat)
gcc -shared -o tree-sitter-python.dll -Isrc src\parser.c
```

### Q4: Windows 编译的注意事项？

**依赖安装：**
```powershell
# Chocolatey 方式
choco install -y cmake ninja nodejs git mingw

# 手动安装
1. Git: https://git-scm.com/download/win
2. MinGW-w64: https://www.mingw-w64.org/
3. Node.js: https://nodejs.org/
4. tree-sitter CLI: npm install -g tree-sitter-cli
```

**编译器选择：**
- GCC（MinGW-w64）- 推荐，兼容性好
- Clang（LLVM）- 可选，现代编译器

**常见错误：**
1. `gcc not found` - 安装 MinGW-w64 并添加到 PATH
2. `tree-sitter not found` - 运行 `npm install -g tree-sitter-cli`
3. `Permission denied` - 以管理员权限运行 PowerShell
4. `.dll vs .so` - Windows 必须使用 `.dll` 扩展名

---

## 下一步优化

1. ✅ 使用 tree-sitter CLI（已完成）
2. ✅ 支持 Windows 平台（已完成）
3. ⏭️ 添加缓存机制
4. ⏭️ 并行编译加速

---

## 本地测试指南

### Linux/macOS
```bash
cd grammars
bash build_ci.sh           # 完整编译
bash build_ci.sh python c  # 仅编译指定语言
ls -lh *.so                # 查看结果
```

### Windows (PowerShell)
```powershell
cd grammars
.\build_ci.ps1              # 完整编译
.\build_ci.ps1 -Languages python,c  # 仅编译指定语言
dir *.dll                   # 查看结果
```

### Windows (Batch - 备用)
```cmd
cd grammars
build_ci.bat                # 完整编译
dir *.dll                   # 查看结果
```

---

## 参考资料

- [tree-sitter CLI 文档](https://tree-sitter.github.io/tree-sitter/creating-parsers)
- [官方语法列表](https://github.com/tree-sitter/tree-sitter/wiki/List-of-parsers)
- [CI 最佳实践](https://docs.github.com/en/actions/using-workflows/workflow-syntax-for-github-actions)
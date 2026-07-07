# CodeScope Windows Installer
# PowerShell script — run as Administrator
#
# Usage:
#   Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
#   .\install.ps1

Write-Host "=== CodeScope Install (Windows) ===" -ForegroundColor Cyan

$CODESCOPE_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $CODESCOPE_DIR

# 1. Check prerequisites
Write-Host ""
Write-Host "[1/4] Checking prerequisites..." -ForegroundColor Yellow

$missing = @()
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $missing += "cmake"
}
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    $missing += "ninja"
}
if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
    $missing += "nodejs"
}
if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
    $missing += "rust (cargo)"
}
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    $missing += "git"
}

if ($missing.Count -gt 0) {
    Write-Host "  Missing: $($missing -join ', ')" -ForegroundColor Red
    Write-Host "  Install via: choco install cmake ninja nodejs rust git" -ForegroundColor Yellow
    Write-Host "  Or manually from:" -ForegroundColor Yellow
    Write-Host "    cmake: https://cmake.org/download/"
    Write-Host "    rust:  https://rustup.rs/"
    Write-Host "    node:  https://nodejs.org/"
    exit 1
}
Write-Host "  All prerequisites found." -ForegroundColor Green

# 2. Install tree-sitter grammars
Write-Host ""
Write-Host "[2/4] Installing tree-sitter grammars..." -ForegroundColor Yellow
$grammars = @(
    "tree-sitter-python", "tree-sitter-c", "tree-sitter-cpp",
    "tree-sitter-rust", "tree-sitter-javascript", "tree-sitter-typescript",
    "tree-sitter-go", "tree-sitter-java", "tree-sitter-swift"
)
foreach ($g in $grammars) {
    npm install -g $g 2>$null | Out-Null
}
Write-Host "  Grammars installed." -ForegroundColor Green

# 3. Build grammar .dll files
Write-Host ""
Write-Host "[3/4] Building grammar shared libraries..." -ForegroundColor Yellow
Set-Location "$CODESCOPE_DIR\grammars"
# On Windows, we use MSVC or clang to build .dll files
$nodeModules = npm root -g 2>$null
if (-not $nodeModules) {
    $nodeModules = "$env:APPDATA\npm\node_modules"
}

function Build-Grammar($lang) {
    $pkgDir = "$nodeModules\tree-sitter-$lang"
    $srcDir = "$pkgDir\src"
    if (-not (Test-Path "$srcDir\parser.c")) {
        Write-Host "  Skipping $lang (grammar not found)" -ForegroundColor Yellow
        return
    }
    $files = @("$srcDir\parser.c")
    if (Test-Path "$srcDir\scanner.c") {
        $files += "$srcDir\scanner.c"
    }
    $flags = "-I$srcDir"
    if ($lang -eq "typescript") {
        $subDir = "$pkgDir\typescript\src"
        if (Test-Path "$subDir\parser.c") {
            gcc -fPIC -shared $flags -I$subDir "$subDir\parser.c" "$subDir\scanner.c" -o "$CODESCOPE_DIR\grammars\tree-sitter-typescript.dll" 2>$null
            Write-Host "  Built: tree-sitter-typescript.dll"
        }
        $tsxDir = "$pkgDir\tsx\src"
        if (Test-Path "$tsxDir\parser.c") {
            gcc -fPIC -shared $flags -I$tsxDir -I"$pkgDir\common" "$tsxDir\parser.c" "$tsxDir\scanner.c" -o "$CODESCOPE_DIR\grammars\tree-sitter-tsx.dll" 2>$null
            Write-Host "  Built: tree-sitter-tsx.dll"
        }
        return
    }
    $srcFiles = $files -join ' '
    gcc -fPIC -shared $flags $srcFiles -o "$CODESCOPE_DIR\grammars\tree-sitter-$lang.dll" 2>$null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  Built: tree-sitter-$lang.dll"
    } else {
        Write-Host "  Failed: tree-sitter-$lang (gcc not available? Install MSYS2)" -ForegroundColor Red
    }
}

# Try to build with gcc (from MSYS2/MinGW) — fallback to skipping
try {
    Get-Command gcc -ErrorAction Stop | Out-Null
    foreach ($g in $grammars) {
        $lang = $g -replace "^tree-sitter-", ""
        Build-Grammar $lang
    }
} catch {
    Write-Host "  gcc not found. Build grammars manually or use WSL." -ForegroundColor Yellow
    Write-Host "  See: https://github.com/tree-sitter/tree-sitter/blob/master/docs/installation.md" -ForegroundColor Yellow
}

# 4. Build CodeScope
Write-Host ""
Write-Host "[4/4] Building CodeScope engine + server..." -ForegroundColor Yellow
Set-Location $CODESCOPE_DIR

# Build C++ engine
cmake -B engine/build -G Ninja -DCMAKE_BUILD_TYPE=Release engine/
if ($LASTEXITCODE -eq 0) {
    cmake --build engine/build --target astgraph_engine
} else {
    Write-Host "  CMake configuration failed. Check that cmake and ninja are installed." -ForegroundColor Red
    exit 1
}

# Build Rust server
cargo build --release --bin codescope
if ($LASTEXITCODE -eq 0) {
    Write-Host "  Rust server built." -ForegroundColor Green
} else {
    Write-Host "  Rust build failed." -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "=== CodeScope Ready ===" -ForegroundColor Green
Write-Host ""
Write-Host "Quick start:"
Write-Host "  cd $CODESCOPE_DIR"
Write-Host "  .\target\release\codescope"
Write-Host ""
Write-Host "Index a project:"
Write-Host "  .\target\release\codescope worker codescope.db C:\path\to\project lang project-name 1"
Write-Host ""
Write-Host "Start MCP server:"
Write-Host "  .\target\release\codescope"

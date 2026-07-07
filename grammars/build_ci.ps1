# Build tree-sitter grammars on Windows using PowerShell
# Requires: Git, GCC/Clang, Node.js, tree-sitter-cli

param(
    [string[]]$Languages = @("python", "c", "cpp", "rust", "javascript", "typescript", "tsx", "go", "java", "swift")
)

$ErrorActionPreference = "Stop"
$GrammarsDir = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "Building tree-sitter grammars on Windows..." -ForegroundColor Cyan

# Check prerequisites
function CheckPrerequisites {
    $errors = @()
    
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        $errors += "Git not found. Install from: https://git-scm.com/download/win"
    }
    
    if (-not (Get-Command gcc -ErrorAction SilentlyContinue) -and -not (Get-Command clang -ErrorAction SilentlyContinue)) {
        $errors += "GCC or Clang not found. Install MinGW-w64 or LLVM"
    }
    
    if (-not (Get-Command tree-sitter -ErrorAction SilentlyContinue)) {
        $errors += "tree-sitter CLI not found. Install with: npm install -g tree-sitter-cli"
    }
    
    if ($errors.Count -gt 0) {
        Write-Host "Missing prerequisites:" -ForegroundColor Red
        $errors | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
        exit 1
    }
}

CheckPrerequisites

# Determine compiler
$Compiler = "gcc"
if (Get-Command clang -ErrorAction SilentlyContinue) {
    $Compiler = "clang"
}
Write-Host "Using compiler: $Compiler" -ForegroundColor Green

# Build each grammar
foreach ($lang in $Languages) {
    Write-Host "Building grammar: $lang" -ForegroundColor Yellow
    
    $grammarRepo = "https://github.com/tree-sitter/tree-sitter-$lang"
    $grammarDir = Join-Path $GrammarsDir "tree-sitter-$lang"
    
    # Clone if not exists
    if (-not (Test-Path $grammarDir)) {
        Write-Host "  Cloning $grammarRepo..."
        git clone $grammarRepo $grammarDir --depth 1
    }
    
    Push-Location $grammarDir
    
    # Generate parser if needed
    if (-not (Test-Path "src\parser.c")) {
        Write-Host "  Generating parser.c..."
        tree-sitter generate
    }
    
    # Determine output file (Windows uses .dll)
    $outputFile = Join-Path $GrammarsDir "tree-sitter-$lang.dll"
    
    # Compile to DLL
    Write-Host "  Compiling to DLL..."
    $srcFiles = @("src\parser.c")
    if (Test-Path "src\scanner.c") {
        $srcFiles += "src\scanner.c"
    }
    
    $compilerArgs = @(
        "-shared",
        "-o", $outputFile,
        "-Isrc"
    )
    $compilerArgs += $srcFiles
    
    # Use different flags for GCC vs Clang
    if ($Compiler -eq "gcc") {
        # MinGW-w64 specific flags
        $compilerArgs = @("-shared", "-o", $outputFile, "-Isrc") + $srcFiles
    } else {
        # Clang specific flags
        $compilerArgs = @("-shared", "-o", $outputFile, "-Isrc") + $srcFiles
    }
    
    & $Compiler $compilerArgs
    
    Write-Host "  -> $outputFile" -ForegroundColor Green
    Pop-Location
}

# Special handling for TypeScript
$typescriptDir = Join-Path $GrammarsDir "tree-sitter-typescript"
if (Test-Path $typescriptDir) {
    Write-Host "Building TypeScript sub-grammars..." -ForegroundColor Yellow
    
    Push-Location $typescriptDir
    
    # Build typescript grammar
    Write-Host "  Compiling TypeScript..."
    & $Compiler -shared -o (Join-Path $GrammarsDir "tree-sitter-typescript.dll") `
        -Itypescript\src typescript\src\parser.c
    
    # Build TSX grammar
    Write-Host "  Compiling TSX..."
    $tsxFiles = @("tsx\src\parser.c")
    if (Test-Path "tsx\src\scanner.c") {
        $tsxFiles += "tsx\src\scanner.c"
    }
    & $Compiler -shared -o (Join-Path $GrammarsDir "tree-sitter-tsx.dll") `
        -Itsx\src -Icommon $tsxFiles
    
    Pop-Location
}

# List built grammars
Write-Host "Done. Built grammars:" -ForegroundColor Cyan
Get-ChildItem -Path $GrammarsDir -Filter "tree-sitter-*.dll" | ForEach-Object {
    Write-Host "  $($_.Name)" -ForegroundColor Green
}

Write-Host "`nNote: Windows uses .dll files instead of .so" -ForegroundColor Yellow
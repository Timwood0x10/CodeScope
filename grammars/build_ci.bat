@echo off
REM Build tree-sitter grammars on Windows using batch script
REM Alternative for environments without PowerShell

setlocal enabledelayedexpansion

echo Building tree-sitter grammars on Windows...

REM Check prerequisites
where git >nul 2>&1
if errorlevel 1 (
    echo Error: Git not found. Install from https://git-scm.com/download/win
    exit /b 1
)

where gcc >nul 2>&1
if errorlevel 1 (
    where clang >nul 2>&1
    if errorlevel 1 (
        echo Error: GCC or Clang not found. Install MinGW-w64 or LLVM
        exit /b 1
    )
    set COMPILER=clang
) else (
    set COMPILER=gcc
)

where tree-sitter >nul 2>&1
if errorlevel 1 (
    echo Error: tree-sitter CLI not found. Install with: npm install -g tree-sitter-cli
    exit /b 1
)

echo Using compiler: !COMPILER!

REM Languages to build
set LANGS=python c cpp rust javascript typescript tsx go java swift

REM Build each grammar
for %%L in (!LANGS!) do (
    echo Building grammar: %%L
    
    set GRAMMAR_REPO=https://github.com/tree-sitter/tree-sitter-%%L
    set GRAMMAR_DIR=%~dp0tree-sitter-%%L
    
    REM Clone if not exists
    if not exist "!GRAMMAR_DIR!" (
        echo   Cloning !GRAMMAR_REPO!
        git clone !GRAMMAR_REPO! "!GRAMMAR_DIR!" --depth 1
    )
    
    pushd "!GRAMMAR_DIR!"
    
    REM Generate parser if needed
    if not exist "src\parser.c" (
        echo   Generating parser.c
        tree-sitter generate
    )
    
    REM Compile to DLL
    echo   Compiling to DLL...
    set OUTPUT_FILE=%~dp0tree-sitter-%%L.dll
    
    if exist "src\scanner.c" (
        !COMPILER! -shared -o "!OUTPUT_FILE!" -Isrc src\parser.c src\scanner.c
    ) else (
        !COMPILER! -shared -o "!OUTPUT_FILE!" -Isrc src\parser.c
    )
    
    echo   -^> !OUTPUT_FILE!
    popd
)

REM Special handling for TypeScript
if exist "%~dp0tree-sitter-typescript" (
    echo Building TypeScript sub-grammars...
    
    pushd "%~dp0tree-sitter-typescript"
    
    echo   Compiling TypeScript
    !COMPILER! -shared -o "%~dp0tree-sitter-typescript.dll" -Itypescript\src typescript\src\parser.c
    
    echo   Compiling TSX
    if exist "tsx\src\scanner.c" (
        !COMPILER! -shared -o "%~dp0tree-sitter-tsx.dll" -Itsx\src -Icommon tsx\src\parser.c tsx\src\scanner.c
    ) else (
        !COMPILER! -shared -o "%~dp0tree-sitter-tsx.dll" -Itsx\src -Icommon tsx\src\parser.c
    )
    
    popd
)

echo Done. Built grammars:
dir /b "%~dp0tree-sitter-*.dll"

echo.
echo Note: Windows uses .dll files instead of .so
endlocal
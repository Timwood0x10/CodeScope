# Building CodeScope on Windows

## Prerequisites

- **Windows 10 1803+** (build 17063+, for built-in `tar.exe`)
- **Git** (`git` in PATH)
- **CMake** (`choco install cmake`)
- **Ninja** (`choco install ninja`)
- **MinGW-w64** (gcc/g++) — recommended: [w64devkit](https://github.com/skeeto/w64devkit) or `choco install mingw`
- **Rust** (`rustup`) with the `windows-gnu` target

## Step-by-step

### 1. Install Rust GNU target

```powershell
rustup target add x86_64-pc-windows-gnu
```

> CodeScope requires the GNU ABI because the C++ engine is compiled with MinGW gcc. The default `windows-msvc` target is not supported (the `build.rs` will panic with a clear error message if you try).

### 2. Install system dependencies

```powershell
choco install -y cmake ninja mingw git
```

Or install [w64devkit](https://github.com/skeeto/w64devkit) which bundles MinGW, then manually install CMake + Ninja.

### 3. Clone and build

```powershell
git clone https://github.com/Timwood0x10/CodeScope
cd CodeScope/server
cargo build --target x86_64-pc-windows-gnu --release
```

The binary will be at `target\x86_64-pc-windows-gnu\release\codescope.exe`.

### 4. (Optional) Build vec0.dll for vector search

```powershell
cd grammars
git clone https://github.com/asg017/sqlite-vec --depth 1
gcc -shared -O2 -Isqlite-vec -o vec0.dll sqlite-vec/sqlite-vec.c -DSQLITE_VEC_OMIT_LITTLEENDIAN=0
```

Place `vec0.dll` next to `codescope.exe` or set `GRAMMARS_DIR` to the directory containing it.

### 5. Set up PATH

```powershell
set PATH=%USERPROFILE%\.codescope\bin;%PATH%
codescope cli index_project {"project_path":"C:\path\to\project"}
```

## Troubleshooting

### `build.rs` panics with "requires the GNU ABI"

You're using the default MSVC target. Run:
```powershell
rustup target add x86_64-pc-windows-gnu
cargo build --target x86_64-pc-windows-gnu --release
```

### `vec0.dll` not found / vector search unavailable

Vector search requires the sqlite-vec extension DLL. See step 4 above. Without it, indexing and all other features work — only semantic vector search is degraded.

### LSP features not working

LSP (go-to-definition, references) is fully supported on Windows via `CreateProcessW` + pipes. Ensure your LSP server (e.g., `clangd`, `pylsp`) is installed and in PATH.

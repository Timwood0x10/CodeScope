use std::env;
use std::process::Command;

fn main() {
    let manifest_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
    let engine_dir = format!("{}/../engine", manifest_dir);

    println!("cargo:rerun-if-changed=../engine/src");
    println!("cargo:rerun-if-changed=../engine/CMakeLists.txt");
    println!("cargo:rerun-if-env-changed=CC");
    println!("cargo:rerun-if-env-changed=CXX");

    // Build the C++ engine in a separate directory from the Makefile's
    // Debug+Tests build (engine/build). This avoids cmake cache
    // invalidation when switching between Release (cargo) and Debug (make test).
    let build_dir = format!("{}/build-release", engine_dir);
    let _ = std::fs::create_dir_all(&build_dir);

    // ── Detect platform ────────────────────────────────────────────
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();

    // ── Windows: enforce GNU ABI ────────────────────────────────────
    // CodeScope's C++ engine (CMake + MinGW gcc) produces a static
    // library with the GCC ABI (SJLJ/DWARF exception model, libgcc
    // runtime). The Rust side must use the matching `windows-gnu`
    // target to avoid ABI mismatch (exception unwinding, malloc/free
    // across CRT boundaries). MSVC (`windows-msvc`) is not supported.
    if target_os == "windows" {
        let target_env = env::var("CARGO_CFG_TARGET_ENV").unwrap_or_default();
        if target_env != "gnu" {
            panic!(
                "\n\nCodeScope on Windows requires the GNU ABI.\n\
                 Run:\n  rustup target add x86_64-pc-windows-gnu\n\
                 Then:\n  cargo build --target x86_64-pc-windows-gnu --release\n"
            );
        }
        eprintln!("build.rs: Windows GNU ABI confirmed");
    }

    // ── Detect compiler ────────────────────────────────────────────
    // Priority: CC/CXX env vars > platform default > fallback
    let (cc_path, cxx_path) = if let (Ok(cc), Ok(cxx)) = (env::var("CC"), env::var("CXX")) {
        eprintln!(
            "build.rs [{}]: using CC={}, CXX={} from env",
            target_os, cc, cxx
        );
        (cc, cxx)
    } else {
        platform_default_compiler(&target_os)
    };

    eprintln!("build.rs [{}]: CC={}, CXX={}", target_os, cc_path, cxx_path);

    // ── macOS: get SDK path for sysroot ────────────────────────────
    let sdk_arg = if target_os == "macos" {
        match Command::new("xcrun").args(["--show-sdk-path"]).output() {
            Ok(output) if output.status.success() => {
                let path = String::from_utf8_lossy(&output.stdout).trim().to_string();
                if !path.is_empty() {
                    format!("-DCMAKE_OSX_SYSROOT={}", path)
                } else {
                    String::new()
                }
            }
            _ => String::new(),
        }
    } else {
        String::new()
    };

    // ── CMake configure ────────────────────────────────────────────
    // Sources are vendored under engine/third_party/ — zero network
    // at configure time. Use Ninja if available for faster builds.
    let mut cmake_args = vec![
        "-S".to_string(),
        engine_dir.clone(),
        "-B".to_string(),
        build_dir.clone(),
        "-DCMAKE_BUILD_TYPE=Release".to_string(),
        "-DBUILD_TESTS=OFF".to_string(),
        format!("-DCMAKE_C_COMPILER={}", cc_path),
        format!("-DCMAKE_CXX_COMPILER={}", cxx_path),
    ];
    // Cross-compilation: when targeting Windows, tell CMake the target
    // system so it doesn't add host-specific flags (e.g. macOS -arch arm64).
    if target_os == "windows" {
        cmake_args.push("-DCMAKE_SYSTEM_NAME=Windows".to_string());
        // MinGW cross-compiler needs the RC compiler for Windows resources
        cmake_args.push("-DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres".to_string());
        // Disable tests (they use POSIX APIs not available in cross-compile)
        cmake_args.push("-DBUILD_TESTS=OFF".to_string());
        // Skip compiler test (cross-compile toolchain may not pass detection)
        cmake_args.push("-DCMAKE_C_COMPILER_WORKS=TRUE".to_string());
        cmake_args.push("-DCMAKE_CXX_COMPILER_WORKS=TRUE".to_string());
    }
    // Use Ninja generator if available (faster parallel builds)
    if std::process::Command::new("ninja")
        .arg("--version")
        .output()
        .is_ok()
    {
        cmake_args.push("-G".to_string());
        cmake_args.push("Ninja".to_string());
    }
    if !sdk_arg.is_empty() {
        cmake_args.push(sdk_arg);
    }

    let cmake_status = Command::new("cmake")
        .args(&cmake_args)
        .status()
        .expect("Failed to run cmake configure");

    if !cmake_status.success() {
        panic!("CMake configure failed");
    }

    // ── CMake build ────────────────────────────────────────────────
    let build_status = Command::new("cmake")
        .args(["--build", &build_dir, "--config", "Release"])
        .status()
        .expect("Failed to run cmake build");

    if !build_status.success() {
        panic!("CMake build failed");
    }

    // ── Linker flags ───────────────────────────────────────────────
    // All deps (tree-sitter core lib.c, sqlite3 amalgamation, sqlite-vec,
    // grammar sources) are compiled directly into astgraph_engine.a.
    // No separate tree-sitter library is needed — the previous
    // -ltree-sitter directive was accidentally linking Homebrew's
    // libtree-sitter.a, which could cause ABI mismatches.
    println!("cargo:rustc-link-search=native={}", build_dir);
    println!("cargo:rustc-link-lib=static=astgraph_engine");

    // LadybugDB (optional, for embedded graph storage via Cypher).
    // Read the CMake cache to determine whether CMake's find_library()
    // succeeded — this is the single source of truth, ensuring build.rs
    // and CMakeLists.txt agree on whether HAS_LADYBUG is defined. If
    // CMake found the library, build.rs links it too; otherwise neither
    // side references lbug symbols and the C++ engine uses SQLite only.
    let cmake_cache = format!("{}/CMakeCache.txt", build_dir);
    let lbug_lib = std::fs::read_to_string(&cmake_cache)
        .ok()
        .and_then(|content| {
            for line in content.lines() {
                if line.starts_with("LADYBUG_LIBRARY:FILEPATH=") {
                    let val = line.trim_start_matches("LADYBUG_LIBRARY:FILEPATH=");
                    if !val.is_empty() && val != "LADYBUG_LIBRARY-NOTFOUND" && val != "NOTFOUND" {
                        return Some(val.to_string());
                    }
                    return None;
                }
            }
            None
        });

    if let Some(lib_path) = &lbug_lib {
        // CMake found liblbug. Derive the directory and link mode from the path.
        let is_windows = target_os == "windows";
        let (lib_dir, lib_name, link_mode, is_static) = if is_windows {
            // Windows cross-compile: use vendored Windows library directly.
            // CMake cache stores the macOS path (from the host build), so
            // we ignore it and use the Windows-specific path.
            let win_lib_dir = format!("{}/third_party/ladybug/lib/windows", engine_dir);
            (
                win_lib_dir,
                "lbug_shared".to_string(),
                "dylib".to_string(),
                false,
            )
        } else {
            let dir = std::path::Path::new(lib_path)
                .parent()
                .map(|p| p.to_string_lossy().to_string())
                .unwrap_or_else(|| ".".to_string());
            let is_static = lib_path.ends_with(".a");
            let mode = if is_static {
                "static".to_string()
            } else {
                "dylib".to_string()
            };
            (dir, "lbug".to_string(), mode, is_static)
        };
        println!("cargo:rustc-link-search=native={}", lib_dir);
        println!("cargo:rustc-link-lib={}={}", link_mode, lib_name);
        // Embed the library directory in the binary's rpath so the
        // dynamic linker can find liblbug at runtime without requiring
        // DYLD_LIBRARY_PATH (macOS), ldconfig (Linux), or PATH (Windows).
        if !is_static && !is_windows {
            println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib_dir);
        }
        // Windows: copy lbug_shared.dll next to the executable
        if is_windows {
            let dll_name = format!("{}/lbug_shared.dll", lib_dir);
            let out_dir = env::var("OUT_DIR").unwrap();
            let out_path = std::path::Path::new(&out_dir);
            // cargo's OUT_DIR is in the build tree; copy to target/release/
            let target_dir = out_path.ancestors().nth(3).unwrap();
            let dll_dest = format!("{}/lbug_shared.dll", target_dir.display());
            if std::path::Path::new(&dll_name).exists() {
                let _ = std::fs::copy(&dll_name, &dll_dest);
                eprintln!("build.rs: Windows DLL copied to {}", dll_dest);
            }
        }
        eprintln!(
            "build.rs: LadybugDB {} lib found via CMake cache at {}",
            if is_static { "static" } else { "dynamic" },
            lib_path
        );
    } else if target_os == "macos" || target_os == "linux" || target_os == "windows" {
        // CMake did not find LadybugDB — consistent with HAS_LADYBUG not
        // being defined. Emit a clear warning so users know Cypher queries
        // will be unavailable.
        eprintln!("WARNING: LadybugDB not found by CMake. Graph storage will use SQLite only.");
        eprintln!("  Install LadybugDB: https://ladybugdb.com/docs/getting-started");
    }

    // C++ standard library: libc++ on macOS, libstdc++ on Linux/Windows
    match target_os.as_str() {
        "macos" => println!("cargo:rustc-link-lib=dylib=c++"),
        _ => println!("cargo:rustc-link-lib=dylib=stdc++"),
    }
}

/// Platform-default compiler detection.
fn platform_default_compiler(target_os: &str) -> (String, String) {
    match target_os {
        "macos" => {
            // macOS: prefer Homebrew LLVM@21 (C++23), fall back to Xcode CLT clang.
            // Apple Silicon installs Homebrew under /opt/homebrew; Intel Macs
            // use /usr/local. Probe both so the build is portable across archs.
            let homebrew_prefixes = ["/opt/homebrew", "/usr/local"];
            let mut found = None;
            for prefix in homebrew_prefixes {
                let homebrew_cc = format!("{}/opt/llvm@21/bin/clang", prefix);
                if std::path::Path::new(&homebrew_cc).exists() {
                    found = Some(prefix);
                    break;
                }
            }
            if let Some(prefix) = found {
                eprintln!("build.rs: macOS → Homebrew LLVM@21 at {}", prefix);
                (
                    format!("{}/opt/llvm@21/bin/clang", prefix),
                    format!("{}/opt/llvm@21/bin/clang++", prefix),
                )
            } else {
                eprintln!("build.rs: macOS → system clang (Xcode CLT)");
                ("clang".to_string(), "clang++".to_string())
            }
        }
        "linux" => {
            // Linux: gcc/g++ is the default (installed via apt)
            eprintln!("build.rs: Linux → gcc/g++");
            ("gcc".to_string(), "g++".to_string())
        }
        "windows" => {
            // Windows: MinGW gcc/g++ (installed via choco)
            eprintln!("build.rs: Windows → gcc/g++ (MinGW)");
            ("gcc".to_string(), "g++".to_string())
        }
        _ => {
            eprintln!("build.rs: unknown OS {} → clang fallback", target_os);
            ("clang".to_string(), "clang++".to_string())
        }
    }
}

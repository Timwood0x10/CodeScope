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
    // Cross-compilation: when targeting Windows FROM macOS/Linux, tell CMake
    // the target system so it doesn't add host-specific flags (e.g. -arch arm64).
    // On native Windows, CMake detects the system correctly and should NOT be
    // overridden — setting CMAKE_SYSTEM_NAME on Windows would break detection.
    if target_os == "windows" {
        // NOTE: CARGO_CFG_TARGET_OS gives the CROSS target, not the host.
        // Use std::env::consts::OS to get the actual build host:
        //   "macos" on macOS, "linux" on Linux, "windows" on Windows.
        // Previously this line read CARGO_CFG_TARGET_OS again, which during a
        // cross-compile returns "windows" and made is_cross always false.
        let build_host = std::env::consts::OS;
        let is_cross = build_host != "windows";
        if is_cross {
            cmake_args.push("-DCMAKE_SYSTEM_NAME=Windows".to_string());
            // MinGW cross-compiler needs the RC compiler for Windows resources
            cmake_args.push("-DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres".to_string());
            // Skip compiler test (cross-compile toolchain may not pass detection)
            cmake_args.push("-DCMAKE_C_COMPILER_WORKS=TRUE".to_string());
            cmake_args.push("-DCMAKE_CXX_COMPILER_WORKS=TRUE".to_string());
        }
        // Disable tests (they use POSIX APIs not available on Windows)
        cmake_args.push("-DBUILD_TESTS=OFF".to_string());
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

    // ── LadybugDB (optional, for embedded graph storage via Cypher) ──
    // Read the CMake cache to determine whether CMake's find_library()
    // succeeded — this is the single source of truth, ensuring build.rs
    // and CMakeLists.txt agree on whether HAS_LADYBUG is defined. If
    // CMake found the library, build.rs links it too; otherwise neither
    // side references lbug symbols and the C++ engine uses SQLite only.
    //
    // Windows: always None. engine/CMakeLists.txt unconditionally skips
    // LadybugDB on Windows (stale entries from a previous native cmake
    // run in the same build-release/ directory are proactively purged by
    // unset(LADYBUG_LIBRARY CACHE) in the Windows branch, but the defense
    // also lives here — no cache-reading on Windows at all).
    let lbug_lib: Option<String> = if target_os == "windows" {
        None
    } else {
        let cmake_cache = format!("{}/CMakeCache.txt", build_dir);
        std::fs::read_to_string(&cmake_cache)
            .ok()
            .and_then(|content| {
                for line in content.lines() {
                    if line.starts_with("LADYBUG_LIBRARY:FILEPATH=") {
                        let val = line.trim_start_matches("LADYBUG_LIBRARY:FILEPATH=");
                        if !val.is_empty() && val != "LADYBUG_LIBRARY-NOTFOUND" && val != "NOTFOUND"
                        {
                            return Some(val.to_string());
                        }
                        return None;
                    }
                }
                None
            })
    };

    if let Some(lib_path) = &lbug_lib {
        // CMake found liblbug. Derive the directory and link mode from the path.
        //
        // NOTE: Windows never reaches this branch. engine/CMakeLists.txt skips
        // the LadybugDB find_library() on Windows (the vendored lbug_shared.lib
        // is a static archive of unverified MinGW ABI, and no lbug_shared.dll
        // exists), so `lbug_lib` stays None on Windows and the `else if` warning
        // branch below handles it instead. Graph storage uses SQLite only on
        // Windows — this is by design, not a regression.
        let lib_dir = std::path::Path::new(lib_path)
            .parent()
            .map(|p| p.to_string_lossy().to_string())
            .unwrap_or_else(|| ".".to_string());
        let is_static = lib_path.ends_with(".a");
        let link_mode = if is_static {
            "static".to_string()
        } else {
            "dylib".to_string()
        };
        println!("cargo:rustc-link-search=native={}", lib_dir);
        println!("cargo:rustc-link-lib={}=lbug", link_mode);
        // Embed RELATIVE rpaths so the binary stays portable: a hardcoded
        // build-machine absolute path (the previous behavior) made the
        // binary fail on any other machine — dyld reported
        // "Library not loaded: @rpath/liblbug.0.dylib" when the embedded
        // /Users/.../engine/third_party/ladybug/lib/macos path did not
        // exist. @executable_path (macOS) / $ORIGIN (Linux) resolve
        // relative to the binary's own location, so moving the whole
        // repository (or re-cloning it elsewhere) keeps the library
        // findable. Two candidates cover the `target/release/codescope`
        // and `bin/codescope` layouts; dyld tries each in order. Windows
        // never reaches here (see NOTE above), so no PATH-copy is needed.
        if !is_static {
            // Windows never reaches this branch (lbug_lib is always None
            // there — LadybugDB is disabled on Windows, SQLite only), so
            // no Windows RPATH is needed.
            if target_os == "macos" {
                println!(
                    "cargo:rustc-link-arg=-Wl,-rpath,@executable_path/../../engine/third_party/ladybug/lib/macos"
                );
                println!(
                    "cargo:rustc-link-arg=-Wl,-rpath,@executable_path/../engine/third_party/ladybug/lib/macos"
                );
            } else if target_os == "linux" {
                // Linux has per-arch vendored dirs: x86_64 -> "linux",
                // aarch64 -> "linux-aarch64". Pick by target arch so the
                // relative RPATH resolves on both.
                let target_arch = env::var("CARGO_CFG_TARGET_ARCH").unwrap_or_default();
                let linux_lib_dir = if target_arch == "aarch64" {
                    "linux-aarch64"
                } else {
                    "linux"
                };
                println!(
                    "cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN/../../engine/third_party/ladybug/lib/{}",
                    linux_lib_dir
                );
                println!(
                    "cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN/../engine/third_party/ladybug/lib/{}",
                    linux_lib_dir
                );
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

    // C++ standard library: libc++ on macOS, libstdc++ on Linux/Windows.
    // On Windows (MinGW GNU ABI) link libstdc++ STATICALLY. This is intentionally
    // redundant with the `-static` rustflag in .cargo/config.toml
    // [target.x86_64-pc-windows-gnu] — the explicit `static=` mode here provides
    // defense in depth (works even if config.toml is absent). Together they bake
    // the C++ runtime into codescope.exe so it no longer depends on
    // libstdc++-6.dll / libgcc_s_seh-1.dll / libwinpthread-1.dll at runtime,
    // eliminating the MinGW 14.0.0 vs 16.1.0 runtime-DLL mismatch that caused
    // access-violation crashes after SQLite init and PowerShell DLL pollution.
    match target_os.as_str() {
        "macos" => println!("cargo:rustc-link-lib=dylib=c++"),
        "windows" => println!("cargo:rustc-link-lib=static=stdc++"),
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
            // Native Windows: MinGW gcc/g++ (installed via choco).
            // Cross-compilation (macOS/Linux → Windows): use the MinGW
            // cross-compiler (x86_64-w64-mingw32-gcc) from mingw-w64 Homebrew
            // formula, which is on PATH when installed.
            let build_host = std::env::consts::OS;
            if build_host == "windows" {
                eprintln!("build.rs: Windows native → gcc/g++ (MinGW)");
                ("gcc".to_string(), "g++".to_string())
            } else {
                eprintln!(
                    "build.rs: cross-compile {}→windows → x86_64-w64-mingw32-gcc/g++",
                    build_host
                );
                (
                    "x86_64-w64-mingw32-gcc".to_string(),
                    "x86_64-w64-mingw32-g++".to_string(),
                )
            }
        }
        _ => {
            eprintln!("build.rs: unknown OS {} → clang fallback", target_os);
            ("clang".to_string(), "clang++".to_string())
        }
    }
}

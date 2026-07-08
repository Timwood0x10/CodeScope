use std::env;
use std::process::Command;

fn main() {
    let manifest_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
    let engine_dir = format!("{}/../engine", manifest_dir);

    println!("cargo:rerun-if-changed=../engine/src");
    println!("cargo:rerun-if-changed=../engine/CMakeLists.txt");
    println!("cargo:rerun-if-env-changed=CC");
    println!("cargo:rerun-if-env-changed=CXX");

    // Build the C++ engine
    let build_dir = format!("{}/build", engine_dir);
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
    // FetchContent mode: all deps (tree-sitter core, sqlite3 amalgamation,
    // sqlite-vec, grammar sources) are compiled into astgraph_engine.a.
    // Only tree-sitter core is a separate static lib.
    println!("cargo:rustc-link-search=native={}", build_dir);
    println!("cargo:rustc-link-lib=static=astgraph_engine");

    let ts_build_lib = format!("{}/_deps/ts_repo-build/lib", build_dir);
    let ts_alt = format!("{}/_deps/ts_repo-build", build_dir);
    if std::path::Path::new(&ts_build_lib).exists() {
        println!("cargo:rustc-link-search=native={}", ts_build_lib);
    }
    if std::path::Path::new(&ts_alt).exists() {
        println!("cargo:rustc-link-search=native={}", ts_alt);
    }
    println!("cargo:rustc-link-lib=static=tree-sitter");

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
            // macOS: prefer Homebrew LLVM@21 (C++23), fall back to Xcode CLT clang
            let homebrew_cc = "/opt/homebrew/opt/llvm@21/bin/clang";
            if std::path::Path::new(homebrew_cc).exists() {
                eprintln!("build.rs: macOS → Homebrew LLVM@21");
                (
                    homebrew_cc.to_string(),
                    "/opt/homebrew/opt/llvm@21/bin/clang++".to_string(),
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

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
    println!("cargo:rustc-link-search=native={}", build_dir);
    println!("cargo:rustc-link-lib=static=astgraph_engine");

    // C++ standard library: libc++ on macOS, libstdc++ on Linux/Windows
    match target_os.as_str() {
        "macos" => println!("cargo:rustc-link-lib=dylib=c++"),
        "linux" => println!("cargo:rustc-link-lib=dylib=stdc++"),
        _ => println!("cargo:rustc-link-lib=dylib=stdc++"),
    }
    println!("cargo:rustc-link-lib=tree-sitter");

    // SQLite + tree-sitter: platform-dependent library paths
    match target_os.as_str() {
        "macos" => {
            // Homebrew lib path for tree-sitter and sqlite3
            let homebrew_lib = "/opt/homebrew/lib";
            let cellars = [
                "/opt/homebrew/Cellar/sqlite/3.53.3/lib",
                "/opt/homebrew/Cellar/sqlite/3.48.0/lib",
                "/opt/homebrew/opt/sqlite/lib",
            ];
            if std::path::Path::new(homebrew_lib).exists() {
                println!("cargo:rustc-link-search=native={}", homebrew_lib);
            }
            let mut found_sqlite = false;
            for dir in &cellars {
                if std::path::Path::new(dir).exists() {
                    println!("cargo:rustc-link-search=native={}", dir);
                    found_sqlite = true;
                    break;
                }
            }
            if !found_sqlite {
                eprintln!("build.rs: Homebrew sqlite3 not found, relying on default linker search");
            }
            println!("cargo:rustc-link-lib=dylib=tree-sitter");
            println!("cargo:rustc-link-lib=dylib=sqlite3");
        }
        "linux" => {
            // System packages provide libtree-sitter.so and libsqlite3.so
            println!("cargo:rustc-link-lib=tree-sitter");
            println!("cargo:rustc-link-lib=sqlite3");
        }
        "windows" => {
            // MinGW and msys2 provide libraries
            println!("cargo:rustc-link-lib=tree-sitter");
            println!("cargo:rustc-link-lib=sqlite3");
        }
        _ => {
            println!("cargo:rustc-link-lib=tree-sitter");
            println!("cargo:rustc-link-lib=sqlite3");
        }
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

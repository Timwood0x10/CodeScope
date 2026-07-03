use std::env;
use std::process::Command;

fn main() {
    let manifest_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
    let engine_dir = format!("{}/../engine", manifest_dir);

    println!("cargo:rerun-if-changed=../engine/src");
    println!("cargo:rerun-if-changed=../engine/CMakeLists.txt");

    // Build the C++ engine
    let build_dir = format!("{}/build", engine_dir);

    // Create build directory
    let _ = std::fs::create_dir_all(&build_dir);

    // Get SDK path
    let sdk_path = Command::new("xcrun")
        .args(["--show-sdk-path"])
        .output()
        .expect("Failed to get SDK path");
    let sdk_path = String::from_utf8_lossy(&sdk_path.stdout).trim().to_string();

    // Run CMake configure with homebrew llvm clang
    let cmake_status = Command::new("cmake")
        .args([
            "-S",
            &engine_dir,
            "-B",
            &build_dir,
            "-DCMAKE_BUILD_TYPE=Release",
            "-DBUILD_TESTS=OFF",
            "-DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm@21/bin/clang",
            "-DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm@21/bin/clang++",
            &format!("-DCMAKE_OSX_SYSROOT={}", sdk_path),
        ])
        .status()
        .expect("Failed to run cmake configure");

    if !cmake_status.success() {
        panic!("CMake configure failed");
    }

    // Run CMake build
    let build_status = Command::new("cmake")
        .args(["--build", &build_dir, "--config", "Release"])
        .status()
        .expect("Failed to run cmake build");

    if !build_status.success() {
        panic!("CMake build failed");
    }

    // Tell cargo where to find the library
    println!("cargo:rustc-link-search=native={}", build_dir);
    println!("cargo:rustc-link-lib=static=astgraph_engine");
    println!("cargo:rustc-link-lib=dylib=c++");
    println!("cargo:rustc-link-lib=tree-sitter");
    // Use homebrew sqlite3 (has extension loading support)
    println!("cargo:rustc-link-search=native=/opt/homebrew/Cellar/sqlite/3.53.3/lib");
    println!("cargo:rustc-link-lib=dylib=sqlite3");
}

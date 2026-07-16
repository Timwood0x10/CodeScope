cmake_minimum_required(VERSION 3.30)
project(astgraph_engine VERSION 0.1.0 LANGUAGES C CXX)

# ── Build Optimization ───────────────────────────────────────────
if(NOT CMAKE_GENERATOR)
    find_program(NINJA ninja)
    if(NINJA)
        set(CMAKE_GENERATOR "Ninja" CACHE STRING "Build generator")
    endif()
endif()
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    # Cache BOTH C and C++ — the heavy translation units (sqlite3.c,
    # tree-sitter parser.c/scanner.c) are C sources and were previously
    # NOT cached, so every clean/rebuild recompiled them from scratch.
    set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Build Type Default ───────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ── Build Speed Options ──────────────────────────────────────────
# Unity build: merges translation units to reduce redundant header
# parsing — can cut build time 20-40%. Off by default; changes error
# locations (may point at unity blob instead of original source).
option(ENABLE_UNITY_BUILD "Enable unity build for faster compilation" OFF)

# Fast dev build: -O0, no LTO. Trades runtime speed for the fastest
# compile/link cycle. Overrides Release optimization flags.
option(BUILD_FAST "Fast dev build: -O0, no LTO, relaxed warnings" OFF)

# ── Linux linker detection (mold / lld) ─────────────────────────
# macOS uses its own linker (ld64); detection is Linux-only.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(MOLD_LINKER mold)
    find_program(LLD_LINKER lld)
    if(MOLD_LINKER)
        set(CMAKE_LINKER_TYPE "MOLD")
        message(STATUS "Linker: mold detected at ${MOLD_LINKER}")
    elseif(LLD_LINKER)
        set(CMAKE_LINKER_TYPE "LLD")
        message(STATUS "Linker: lld detected at ${LLD_LINKER}")
    else()
        message(STATUS "Linker: neither mold nor lld found, using system default")
    endif()
endif()

# ── Windows toolchain detection ─────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    if(MSVC)
        # MSVC: enable Unicode source encoding and raise warning level
        add_compile_options(/utf-8 /W4)
        # Disable MSVC-specific noisy warnings from third-party headers
        add_compile_options(/wd4996)  # 'strcpy' was deprecated (tree-sitter API)
        message(STATUS "Windows/MSVC: /utf-8 /W4 enabled")
    else()
        # MinGW / Clang on Windows
        add_compile_options(-Wall -Wextra)
        message(STATUS "Windows/MinGW: GCC-compatible flags")
    endif()
    # Common Windows definitions
    add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX)
endif()

# ── Split debug info on Linux (Debug only) ───────────────────────
# Keeps .dwo out of the executable for faster linking.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-gsplit-dwarf)
    message(STATUS "Debug: enabling -gsplit-dwarf for faster linking")
endif()

# ── BUILD_FAST ──────────────────────────────────────────────────
if(BUILD_FAST)
    message(STATUS "BUILD_FAST: ON — fast dev build (-O0 -g -UNDEBUG, no LTO)")
    add_compile_options(-O0 -g -UNDEBUG)
endif()
# NOTE: ThinLTO (-flto=thin) was evaluated and removed. When the C++
# engine is compiled with Homebrew LLVM 21 ThinLTO bitcode, the Rust
# server's linker (Apple ld64, LLVM 17) cannot parse the bitcode and
# fails with "Unknown attribute kind (102)". Cross-toolchain LTO is
# not supported on macOS. Use BUILD_FAST for dev and plain Release
# (no LTO) for production — the PCH + ccache already cut build time.

# ── Warning Flags ────────────────────────────────────────────────
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# ── Dependency version pins ──────────────────────────────────────
include(cmake/deps_versions.cmake)

# ── Vendored third-party sources (zero network at configure time) ──
# All generated C sources (tree-sitter grammars, tree-sitter core,
# sqlite-vec, sqlite amalgamation) are committed under engine/third_party/.
# This removes the ~286s serial git-clone / HTTP-download that used to
# happen at cmake configure. Versions are pinned in deps_versions.cmake;
# to refresh, run: scripts/vendor-deps.sh
set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(GRAMMAR_DIR "${THIRD_PARTY_DIR}/grammars")

# tree-sitter core runtime. lib.c amalgamates every file in lib/src/*.c
# (alloc.c, language.c, parser.c, wasm_store.c, ...). We compile lib.c
# directly into our static lib — no external tree-sitter to link against
# (this avoids the Homebrew libtree-sitter SIGBUS struct-layout mismatch).
set(TREE_SITTER_LIB "")
set(TREE_SITTER_SOURCES
    "${THIRD_PARTY_DIR}/tree-sitter/lib/src/lib.c")
set(TREE_SITTER_INCDIR "${THIRD_PARTY_DIR}/tree-sitter/lib/include")
# Grammar parser.c must resolve "tree_sitter/parser.h" to the CORE's
# internal header FIRST, before the grammar's own bundled copy, otherwise
# a struct-layout mismatch → SIGBUS. Keep this include dir before
# TREE_SITTER_INCLUDE_DIRS in target_include_directories (below).
set(TREE_SITTER_SRCDIR "${THIRD_PARTY_DIR}/tree-sitter/lib/src")

# ── Grammar parser.c / scanner.c (vendored generated sources) ──
# Swift omitted: parser.c incompatible with tree-sitter core v0.26.11.
set(GRAMMAR_C_PARSER "${GRAMMAR_DIR}/c/src/parser.c")
set(GRAMMAR_SOURCES
    ${GRAMMAR_DIR}/c/src/parser.c
    ${GRAMMAR_DIR}/cpp/src/parser.c
    ${GRAMMAR_DIR}/cpp/src/scanner.c
    ${GRAMMAR_DIR}/go/src/parser.c
    ${GRAMMAR_DIR}/java/src/parser.c
    ${GRAMMAR_DIR}/javascript/src/parser.c
    ${GRAMMAR_DIR}/javascript/src/scanner.c
    ${GRAMMAR_DIR}/python/src/parser.c
    ${GRAMMAR_DIR}/python/src/scanner.c
    ${GRAMMAR_DIR}/rust/src/parser.c
    ${GRAMMAR_DIR}/rust/src/scanner.c
    ${GRAMMAR_DIR}/typescript/typescript/src/parser.c
    ${GRAMMAR_DIR}/typescript/typescript/src/scanner.c
    ${GRAMMAR_DIR}/typescript/tsx/src/parser.c
    ${GRAMMAR_DIR}/typescript/tsx/src/scanner.c
)

# Include dirs: each grammar's src/ (for "./alloc.h" and bundled
# tree_sitter/*.h) plus the typescript common/ dir (scanner.h).
set(TREE_SITTER_INCLUDE_DIRS
    ${GRAMMAR_DIR}/c/src
    ${GRAMMAR_DIR}/cpp/src
    ${GRAMMAR_DIR}/go/src
    ${GRAMMAR_DIR}/java/src
    ${GRAMMAR_DIR}/javascript/src
    ${GRAMMAR_DIR}/python/src
    ${GRAMMAR_DIR}/rust/src
    ${GRAMMAR_DIR}/typescript/typescript/src
    ${GRAMMAR_DIR}/typescript/tsx/src
    ${GRAMMAR_DIR}/typescript/common
)

# ── sqlite3 amalgamation (vendored) ───────────────────────────────
set(SQLITE3_DIR "${THIRD_PARTY_DIR}/sqlite")
set(SQLITE3_INCLUDE_DIRS "${SQLITE3_DIR}")
set(SQLITE3_AMAL_SRC "${SQLITE3_DIR}/sqlite3.c")
set(SQLITE3_LIBRARIES "")
# Linux needs -ldl for dlopen(); macOS has it in libc; Windows doesn't use dl
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_DL_LIBS dl)
else()
    set(CMAKE_DL_LIBS "")
endif()

# ── sqlite-vec (vendored source + pre-generated header) ──────────
set(SQLITE_VEC_DIR "${THIRD_PARTY_DIR}/sqlite-vec")
set(SQLITE_VEC_SRC "${SQLITE_VEC_DIR}/sqlite-vec.c")
message(STATUS "sqlite-vec: source at ${SQLITE_VEC_SRC}")

# ── Engine sources ───────────────────────────────────────────────
set(ENGINE_SOURCES
    ${GRAMMAR_SOURCES}
    src/engine.cpp
    src/engine_helpers.cpp
    src/engine_lifecycle.cpp
    src/engine_index.cpp
        src/engine_index_project.cpp
        src/engine_index_metrics.cpp
    src/engine_scanner.cpp
    src/engine_queries.cpp
    src/engine_ffi.cpp
    src/engine_verify_ffi.cpp
    src/engine_verify_drift_ffi.cpp
    src/parser/parser.cpp
    src/ir/ir.cpp
    src/ir/ir_visitor.cpp
    src/ir/ir_translator.cpp
    src/ir/semantic_unit.cpp
    src/ir/semantic_emitter.cpp
    src/ir/translators/python_translator.cpp
    src/ir/translators/go_translator.cpp
    src/ir/translators/c_translator.cpp
    src/ir/translators/cpp_translator.cpp
    src/ir/translators/rust_translator.cpp
    src/ir/translators/javascript_translator.cpp
    src/ir/translators/typescript_translator.cpp
    src/ir/translators/js_visitor.cpp
    src/ir/translators/ts_visitor.cpp
    src/ir/translators/tsx_visitor.cpp
    src/ir/translators/c_visitor.cpp
    src/ir/translators/cpp_visitor.cpp
    src/ir/translators/go_visitor.cpp
    src/ir/translators/python_visitor.cpp
    src/ir/translators/rust_visitor.cpp
    src/ir/translators/java_visitor.cpp
    src/ir/translators/java_translator.cpp
    src/ir/translators/scanner_visitor.cpp
    src/ir/translators/tsx_translator.cpp
    src/graph/graph_types.cpp
    src/graph/graph_builder.cpp
    src/store/store_core.cpp
    src/store/store_schema.cpp
    src/store/store.cpp
    src/store/store_insert.cpp
    src/store/store_search.cpp
    src/store/store_batch.cpp
    src/store/store_type.cpp
    src/store/store_graph.cpp
    src/store/store_query.cpp
    src/store/store_project.cpp
    src/store/store_knowledge.cpp
        src/store/store_ladybug.cpp
    src/query/query_engine.cpp
    src/query/query_analysis.cpp
    src/query/graph_query.cpp
    src/query/impact_analysis.cpp
         src/lsp/lsp_client.cpp
    src/resolver/resolver.cpp
    src/resolver/pipeline.cpp
    src/resolver/fuzzy_resolver.cpp
        src/resolver/factors.cpp
        src/model/engine.cpp
        src/model/plugins/workflow.cpp
        src/model/plugins/capability.cpp
        src/model/plugins/architecture.cpp
        src/model/plugins/contract.cpp
        src/model/state_builder.cpp
    src/resolver/project_index.cpp
        src/resolver/project_resolver.cpp
        src/verify/capability_verifier.cpp
        src/verify/contract_verifier.cpp
        src/verify/architecture_verifier.cpp
        src/verify/dead_code_inspector.cpp
        src/verify/claim_parser.cpp
        src/verify/claim.cpp
        src/verify/registry.cpp
        src/verify/documentation_drift.cpp
        src/verify/capability_drift.cpp
        src/verify/architecture_drift.cpp
        src/async_knowledge.cpp
        src/resolver/resolve_cache.cpp
        src/filter_policy.cpp
        src/filter_policy_ignore.cpp
        src/platform_win.cpp
        ${SQLITE3_AMAL_SRC}
        ${TREE_SITTER_SOURCES}
)

# Add sqlite-vec.c if available
if(EXISTS "${SQLITE_VEC_SRC}")
    list(APPEND ENGINE_SOURCES ${SQLITE_VEC_SRC})
    message(STATUS "sqlite-vec: source found, compiling into engine")
endif()

add_library(astgraph_engine STATIC ${ENGINE_SOURCES})

# ── Unity Build ──────────────────────────────────────────────────
# Batches translation units together to reduce redundant header
# parsing. Batch size 8 keeps memory bounded while still cutting
# compile time on this ~100-file target.
if(ENABLE_UNITY_BUILD)
    set_target_properties(astgraph_engine PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE 8)
    message(STATUS "Unity build: ON (batch size 8) for astgraph_engine")
endif()

# ── Precompiled Header ───────────────────────────────────────────
# PCH cuts build time 30-50% by pre-parsing heavy headers once.
target_precompile_headers(astgraph_engine PRIVATE
    <cstdio>
    <cstring>
    <string>
    <vector>
    <unordered_map>
    <unordered_set>
    <memory>
    <mutex>
    <sqlite3.h>
    <tree_sitter/api.h>
)

# Compile sqlite3.c and sqlite-vec.c as C, not C++
# Also skip PCH for all C sources (PCH contains C++-only headers like <unordered_map>)
set_source_files_properties(${SQLITE3_AMAL_SRC} ${GRAMMAR_SOURCES} PROPERTIES LANGUAGE C)
if(EXISTS "${SQLITE_VEC_SRC}")
    set_source_files_properties(${SQLITE_VEC_SRC} PROPERTIES LANGUAGE C)
endif()
set_source_files_properties(${SQLITE3_AMAL_SRC} ${GRAMMAR_SOURCES}
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
# tree-sitter C grammar: compile with -O0 to work around SIGILL on Apple Silicon
# (v0.24.2 generates code that triggers illegal instruction on arm64 at -O1+)
set_source_files_properties(${GRAMMAR_C_PARSER}
    PROPERTIES COMPILE_OPTIONS "-O0")
set_source_files_properties(${TREE_SITTER_SOURCES}
    PROPERTIES LANGUAGE C SKIP_PRECOMPILE_HEADERS ON)

# ── sqlite3.c: avoid -O0 bottleneck in BUILD_FAST mode ──────────
# sqlite3.c is a ~250k-line amalgamation. Under -O0 it dominates
# the build timeline. Bump to -O1 specifically for this file so
# the dev cycle isn't blocked on a single translation unit.
# (Applied only when BUILD_FAST is ON; Release already uses -O2/-O3.)
if(BUILD_FAST)
    set_source_files_properties(${SQLITE3_AMAL_SRC}
        PROPERTIES COMPILE_OPTIONS "-O1")
    message(STATUS "BUILD_FAST: sqlite3.c compiled with -O1 (avoid -O0 bottleneck)")
endif()

if(EXISTS "${SQLITE_VEC_SRC}")
    set_source_files_properties(${SQLITE_VEC_SRC} PROPERTIES SKIP_PRECOMPILE_HEADERS ON LANGUAGE C)
    # Header is pre-generated and committed at third_party/sqlite-vec/sqlite-vec.h.
    target_include_directories(astgraph_engine PRIVATE "${SQLITE_VEC_DIR}")
endif()

target_compile_definitions(astgraph_engine PUBLIC
    SQLITE_ENABLE_LOAD_EXTENSION
    SQLITE_ENABLE_FTS5
    SQLITE_THREADSAFE=1)

target_include_directories(astgraph_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SQLITE3_INCLUDE_DIRS}
    ${TREE_SITTER_INCDIR}
    ${TREE_SITTER_SRCDIR}
    ${TREE_SITTER_INCLUDE_DIRS}
)
# sqlite-vec include is added via COMPILE_FLAGS on the C source file only
# (see the sqlite-vec section below) — not here, to avoid the C++ <version> conflict.
# Add sqlite-vec source dir to C-only compiler flags via generator expression.
# This lets sqlite-vec.c find sqlite-vec.h without polluting C++ include paths
# (C++20's <version> would shadow against the repo's "version" file).
if(EXISTS "${SQLITE_VEC_SRC}")
    target_compile_options(astgraph_engine PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-I${SQLITE_VEC_DIR}>)
endif()

target_link_libraries(astgraph_engine PUBLIC
    ${TREE_SITTER_LIB}
    ${SQLITE3_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

# ── LadybugDB (embedded graph database for graph storage) ──────
# Optional dependency. The engine builds and runs without it (SQLite
# remains the source of truth for graph storage). When LadybugDB is
# present, Cypher queries and native graph traversal are enabled via
# the HAS_LADYBUG compile flag.
#
# Detection order:
#   1. Vendored in third_party/ladybug/ (preferred, zero network)
#   2. System-wide install → find_library picks it up
#   3. macOS Homebrew → /opt/homebrew/lib, /usr/local/lib
#
# Set -DLADYBUG_REQUIRED=ON to make the build fail when LadybugDB
# is not found (useful for production builds that require Cypher).
option(LADYBUG_REQUIRED "Fail build if LadybugDB is not found" OFF)
set(LADYBUG_VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ladybug")
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(LADYBUG_VENDOR_LIB_DIR "${LADYBUG_VENDOR_DIR}/lib/macos")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Select the architecture-matched vendored library directory:
    #   lib/linux       → x86-64
    #   lib/linux-aarch64 → aarch64
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(LADYBUG_VENDOR_LIB_DIR "${LADYBUG_VENDOR_DIR}/lib/linux-aarch64")
    else()
        set(LADYBUG_VENDOR_LIB_DIR "${LADYBUG_VENDOR_DIR}/lib/linux")
    endif()
else()
    set(LADYBUG_VENDOR_LIB_DIR "${LADYBUG_VENDOR_DIR}/lib")
endif()
# Force re-detection: clear stale cache entries from previous runs
unset(LADYBUG_LIBRARY CACHE)
unset(LADYBUG_INCLUDE_DIR CACHE)
find_library(LADYBUG_LIBRARY lbug
    PATHS ${LADYBUG_VENDOR_LIB_DIR} /opt/homebrew/lib /usr/local/lib /usr/lib
    NO_DEFAULT_PATH)
if(NOT LADYBUG_LIBRARY)
    find_library(LADYBUG_LIBRARY lbug)
endif()

# ── Architecture compatibility check ───────────────────────────
# find_library only checks the filename, not the binary architecture.
# On aarch64 Linux the vendored liblbug.so is x86-64; without this
# check CMake would define HAS_LADYBUG and the link step would fail
# with "skipping incompatible liblbug.so".
if(LADYBUG_LIBRARY)
    execute_process(
        COMMAND file -b "${LADYBUG_LIBRARY}"
        OUTPUT_VARIABLE _LBUG_ARCH_INFO
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    set(_LBUG_ARCH_OK FALSE)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" AND _LBUG_ARCH_INFO MATCHES "x86-64")
            set(_LBUG_ARCH_OK TRUE)
        elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" AND _LBUG_ARCH_INFO MATCHES "aarch64")
            set(_LBUG_ARCH_OK TRUE)
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" AND _LBUG_ARCH_INFO MATCHES "arm64")
            set(_LBUG_ARCH_OK TRUE)
        elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" AND _LBUG_ARCH_INFO MATCHES "x86_64")
            set(_LBUG_ARCH_OK TRUE)
        endif()
    else()
        # Unknown platform — assume compatible (let linker decide)
        set(_LBUG_ARCH_OK TRUE)
    endif()
    if(NOT _LBUG_ARCH_OK)
        message(STATUS "LadybugDB: ${LADYBUG_LIBRARY} is [${_LBUG_ARCH_INFO}], "
            "does not match ${CMAKE_SYSTEM_PROCESSOR} — skipping")
        unset(LADYBUG_LIBRARY CACHE)
    endif()
endif()

find_path(LADYBUG_INCLUDE_DIR lbug.h
    PATHS ${LADYBUG_VENDOR_DIR}/include /opt/homebrew/include /usr/local/include /usr/include
    NO_DEFAULT_PATH)
if(NOT LADYBUG_INCLUDE_DIR)
    find_path(LADYBUG_INCLUDE_DIR lbug.h)
endif()

# ── Fallback: search for versioned library names ───────────────
# If the dev symlink (liblbug.so / liblbug.dylib) is missing but the
# versioned shared object exists (e.g., liblbug.so.0.18.2), use it
# directly and create the dev symlink so the linker can find it.
# Also re-check architecture on the versioned library.
if(NOT LADYBUG_LIBRARY AND EXISTS "${LADYBUG_VENDOR_LIB_DIR}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        file(GLOB _LADYBUG_VERIFIED
            "${LADYBUG_VENDOR_LIB_DIR}/liblbug.[0-9]*.dylib")
    else()
        file(GLOB _LADYBUG_VERIFIED
            "${LADYBUG_VENDOR_LIB_DIR}/liblbug.so.[0-9]*")
    endif()
    if(_LADYBUG_VERIFIED)
        list(GET _LADYBUG_VERIFIED 0 _LBUG_CANDIDATE)
        # Verify architecture of the versioned library too
        set(_LBUG_VER_OK FALSE)
        execute_process(
            COMMAND file -b "${_LBUG_CANDIDATE}"
            OUTPUT_VARIABLE _LBUG_VER_INFO
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" AND _LBUG_VER_INFO MATCHES "x86-64")
                set(_LBUG_VER_OK TRUE)
            elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" AND _LBUG_VER_INFO MATCHES "aarch64")
                set(_LBUG_VER_OK TRUE)
            endif()
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" AND _LBUG_VER_INFO MATCHES "arm64")
                set(_LBUG_VER_OK TRUE)
            elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" AND _LBUG_VER_INFO MATCHES "x86_64")
                set(_LBUG_VER_OK TRUE)
            endif()
        else()
            set(_LBUG_VER_OK TRUE)
        endif()
        if(_LBUG_VER_OK)
            set(LADYBUG_LIBRARY "${_LBUG_CANDIDATE}")
            # Create the dev symlink so the linker's -llbug resolves.
            if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
                set(_LADYBUG_DEV_LINK "${LADYBUG_VENDOR_LIB_DIR}/liblbug.dylib")
            else()
                set(_LADYBUG_DEV_LINK "${LADYBUG_VENDOR_LIB_DIR}/liblbug.so")
            endif()
            if(NOT EXISTS "${_LADYBUG_DEV_LINK}")
                execute_process(
                    COMMAND ${CMAKE_COMMAND} -E create_symlink
                        "${LADYBUG_LIBRARY}" "${_LADYBUG_DEV_LINK}"
                    RESULT_VARIABLE _SYMLINK_RESULT)
            endif()
            message(STATUS "LadybugDB: using versioned library ${LADYBUG_LIBRARY}")
        else()
            message(STATUS "LadybugDB: versioned library ${_LBUG_CANDIDATE} is "
                "[${_LBUG_VER_INFO}], does not match ${CMAKE_SYSTEM_PROCESSOR} — skipping")
        endif()
    endif()
endif()

if(LADYBUG_LIBRARY AND LADYBUG_INCLUDE_DIR)
    target_compile_definitions(astgraph_engine PRIVATE HAS_LADYBUG=1)
    target_compile_definitions(astgraph_engine PRIVATE LBUG_STATIC_DEFINE)
    target_include_directories(astgraph_engine PUBLIC ${LADYBUG_INCLUDE_DIR})
    target_link_libraries(astgraph_engine PUBLIC ${LADYBUG_LIBRARY})
    message(STATUS "LadybugDB: found at ${LADYBUG_LIBRARY}")
elseif(LADYBUG_REQUIRED)
    message(FATAL_ERROR
        "LadybugDB is REQUIRED (LADYBUG_REQUIRED=ON) but was not found.\n"
        "Please install manually:\n"
        "  macOS:  brew install ladybug\n"
        "  Linux:  curl -fsSL https://install.ladybugdb.com | sh\n"
        "  Docs:   https://docs.ladybugdb.com/installation/")
else()
    message(WARNING
        "LadybugDB not found — building without Cypher/graph query support.\n"
        "The engine will use SQLite for all graph storage.\n"
        "To enable: install LadybugDB or set -DLADYBUG_REQUIRED=ON to enforce.")
endif()

# ── Tests ────────────────────────────────────────────────────────
# Automated tests live in tests/. Each must run with no command-line
# arguments and is exercised by `make test-engine` and CI.
#
# Decision (Option A): manual debugging tools that require external
# args (e.g. <grammars_dir> <kernel_dir> or a large source tree to
# scan) were moved to manual/ so the tests/ glob only picks up
# automated tests. This prevents arg-requiring tools from being built
# and run by default, which previously broke `make test-engine` / CI
# (they exit 1 on missing args). Manual tools are still buildable on
# demand with -DBUILD_MANUAL=ON.
option(BUILD_TESTS "Build automated tests" ON)
if(BUILD_TESTS)
    file(GLOB TEST_SOURCES tests/*.cpp)
    foreach(test_src ${TEST_SOURCES})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        target_link_libraries(${test_name} astgraph_engine)
    endforeach()
endif()

# ── Manual debug tools (not built by default) ────────────────────
# These need command-line args and are not automated tests. Build
# explicitly with: cmake -S engine -B build -DBUILD_MANUAL=ON
option(BUILD_MANUAL "Build manual debug tools (require external args)" OFF)
if(BUILD_MANUAL)
    file(GLOB MANUAL_SOURCES manual/*.cpp)
    foreach(tool_src ${MANUAL_SOURCES})
        get_filename_component(tool_name ${tool_src} NAME_WE)
        add_executable(${tool_name} ${tool_src})
        target_link_libraries(${tool_name} astgraph_engine)
    endforeach()
endif()

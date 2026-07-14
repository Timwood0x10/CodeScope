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

# ── FetchContent: tree-sitter core + sqlite amalgamation + grammars + sqlite-vec ──
include(FetchContent)
include(cmake/fetch_grammar.cmake)

# ── tree-sitter core (build from source) ─────────────────────────
message(STATUS "tree-sitter: fetching core v${TS_CORE_VERSION}...")
FetchContent_Declare(ts_repo
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
    GIT_TAG ${TS_CORE_VERSION}
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR lib)
set(BUILD_SHARED_LIBS OFF)
set(TREE_SITTER_ENABLE_TESTING OFF)
set(TREE_SITTER_ENABLE_EXAMPLES OFF)
FetchContent_MakeAvailable(ts_repo)
# IMPORTANT: Force using FetchContent-built tree-sitter, NOT the
# system/Homebrew version. cmake's pkg-config may find libtree-sitter
# from Homebrew first, which skips building the FetchContent source
# entirely. This causes the grammar to compile against FetchContent
# headers but link against Homebrew's library → SIGBUS from struct
# layout mismatch. Fix: compile tree-sitter source directly into our
# binary (like CBM's ts_runtime.c approach), bypassing the library
# dependency entirely so there is NO external tree-sitter to resolve.
set(TREE_SITTER_LIB "")
set(TREE_SITTER_SOURCES
    "${ts_repo_SOURCE_DIR}/lib/src/lib.c")
set(TREE_SITTER_INCDIR "${ts_repo_SOURCE_DIR}/lib/include" "${ts_repo_BINARY_DIR}/lib/include")
# Grammar compilation needs the core's internal parser.h (in lib/src/),
# not just the public API (lib/include/). Adding lib/src/ here ensures
# #include "tree_sitter/parser.h" resolves to the core's version FIRST,
# before the grammar's bundled copy. Without this, grammars compile
# against their OWN bundled parser.h which may have a different struct
# layout than what the v0.26.10 core expects → SIGBUS.
set(TREE_SITTER_SRCDIR "${ts_repo_SOURCE_DIR}/lib/src")

# ── sqlite3 amalgamation (download once, compile as C) ──────────
set(SQLITE3_DEPS_DIR "${CMAKE_BINARY_DIR}/_deps")
set(SQLITE3_AMAL_SRC "${SQLITE3_DEPS_DIR}/sqlite3.c")
if(NOT EXISTS "${SQLITE3_AMAL_SRC}")
    message(STATUS "sqlite3: downloading amalgamation ${SQLITE_VERSION}...")
    file(DOWNLOAD "https://www.sqlite.org/2025/sqlite-amalgamation-${SQLITE_VERSION}.zip"
        "${SQLITE3_DEPS_DIR}/sqlite-amalgamation.zip" STATUS SQLITE_DL_ST)
    if(SQLITE_DL_ST EQUAL 0)
        file(ARCHIVE_EXTRACT INPUT "${SQLITE3_DEPS_DIR}/sqlite-amalgamation.zip"
            DESTINATION "${SQLITE3_DEPS_DIR}/")
        file(GLOB SQLITE_EXTRACTED LIST_DIRECTORIES true "${SQLITE3_DEPS_DIR}/sqlite-amalgamation-*")
        if(SQLITE_EXTRACTED)
            file(GLOB SQLITE_FILES "${SQLITE_EXTRACTED}/*")
            file(COPY ${SQLITE_FILES} DESTINATION "${SQLITE3_DEPS_DIR}/")
        endif()
    else()
        message(FATAL_ERROR "sqlite3: amalgamation download failed (status=${SQLITE_DL_ST})")
    endif()
endif()
set(SQLITE3_INCLUDE_DIRS "${SQLITE3_DEPS_DIR}")
set(SQLITE3_AMAL_SRC "${SQLITE3_DEPS_DIR}/sqlite3.c")
set(SQLITE3_LIBRARIES "")
set(CMAKE_DL_LIBS "")
if(APPLE OR (UNIX AND NOT APPLE))
    set(CMAKE_DL_LIBS dl)
endif()

# ── sqlite-vec (build from source, compiled directly into binary) ──
# Uses FetchContent to clone the repo (like tree-sitter grammars).
# Compiled as C — no runtime load_extension needed.
include(FetchContent)
cmake_policy(SET CMP0169 OLD)
FetchContent_Declare(sqlite_vec
    GIT_REPOSITORY https://github.com/asg017/sqlite-vec.git
    GIT_TAG v0.1.10-alpha.4
    GIT_SHALLOW TRUE)
FetchContent_GetProperties(sqlite_vec)
if(NOT sqlite_vec_POPULATED)
    FetchContent_Populate(sqlite_vec)
endif()
set(SQLITE_VEC_SRC "${sqlite_vec_SOURCE_DIR}/sqlite-vec.c")
message(STATUS "sqlite-vec: source at ${SQLITE_VEC_SRC}")

# ── Grammar parser.c (FetchContent per grammar) ──────────────────
# Swift omitted: parser.c incompatible with tree-sitter core v0.24.7.
set(GRAMMAR_SOURCES
    ${GRAMMAR_SOURCE_DIR_c}/parser.c
    ${GRAMMAR_SOURCE_DIR_cpp}/parser.c
    ${GRAMMAR_SOURCE_DIR_cpp}/scanner.c
    ${GRAMMAR_SOURCE_DIR_go}/parser.c
    ${GRAMMAR_SOURCE_DIR_java}/parser.c
    ${GRAMMAR_SOURCE_DIR_javascript}/parser.c
    ${GRAMMAR_SOURCE_DIR_javascript}/scanner.c
    ${GRAMMAR_SOURCE_DIR_python}/parser.c
    ${GRAMMAR_SOURCE_DIR_python}/scanner.c
    ${GRAMMAR_SOURCE_DIR_rust}/parser.c
    ${GRAMMAR_SOURCE_DIR_rust}/scanner.c
    ${tree-sitter-typescript_SOURCE_DIR}/typescript/src/parser.c
    ${tree-sitter-typescript_SOURCE_DIR}/typescript/src/scanner.c
    ${tree-sitter-typescript_SOURCE_DIR}/tsx/src/parser.c
    ${tree-sitter-typescript_SOURCE_DIR}/tsx/src/scanner.c
)

set(TREE_SITTER_INCLUDE_DIRS
    ${GRAMMAR_SOURCE_DIR_c}
    ${GRAMMAR_SOURCE_DIR_cpp}
    ${GRAMMAR_SOURCE_DIR_go}
    ${GRAMMAR_SOURCE_DIR_java}
    ${GRAMMAR_SOURCE_DIR_javascript}
    ${GRAMMAR_SOURCE_DIR_python}
    ${GRAMMAR_SOURCE_DIR_rust}
    ${tree-sitter-typescript_SOURCE_DIR}/typescript/src
    ${tree-sitter-typescript_SOURCE_DIR}/tsx/src
    ${tree-sitter-typescript_SOURCE_DIR}/common
)

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
    set_source_files_properties(${SQLITE_VEC_SRC} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    # Generate sqlite-vec.h from template with version substitution.
    # Parse the version string: v0.1.10-alpha.4 → major=0, minor=1, patch=10
    string(REGEX MATCH "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ ${SQLITE_VEC_VERSION})
    set(VERSION_MAJOR ${CMAKE_MATCH_1})
    set(VERSION_MINOR ${CMAKE_MATCH_2})
    set(VERSION_PATCH ${CMAKE_MATCH_3})
    set(VERSION "${SQLITE_VEC_VERSION}")  # full version string for SQLITE_VEC_VERSION macro
    file(READ "${sqlite_vec_SOURCE_DIR}/sqlite-vec.h.tmpl" VEC_H_TMPL)
    string(REPLACE "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}-alpha.4"
        "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}" _dummy "${SQLITE_VEC_VERSION}")
    string(CONFIGURE "${VEC_H_TMPL}" VEC_H_CONTENT)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sqlite-vec/include/sqlite-vec.h" "${VEC_H_CONTENT}")
    target_include_directories(astgraph_engine PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/sqlite-vec/include")
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
        $<$<COMPILE_LANGUAGE:C>:-I${sqlite_vec_SOURCE_DIR}>)
endif()

target_link_libraries(astgraph_engine PUBLIC
    ${TREE_SITTER_LIB}
    ${SQLITE3_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

# ── Tests ────────────────────────────────────────────────────────
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    file(GLOB TEST_SOURCES tests/*.cpp)
    foreach(test_src ${TEST_SOURCES})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        target_link_libraries(${test_name} astgraph_engine)
    endforeach()
endif()

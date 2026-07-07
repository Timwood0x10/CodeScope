cmake_minimum_required(VERSION 3.30)
project(astgraph_engine VERSION 0.1.0 LANGUAGES C CXX)

# ── Build Optimization ───────────────────────────────────────────
# Use Ninja if available (much faster than Make)
if(NOT CMAKE_GENERATOR)
    find_program(NINJA ninja)
    if(NINJA)
        set(CMAKE_GENERATOR "Ninja" CACHE STRING "Build generator")
    endif()
endif()

# Enable ccache if available (compiler cache)
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
endif()

# Unity build for faster compilation (combine source files)
option(UNITY_BUILD "Enable unity build for faster compilation" OFF)
if(UNITY_BUILD)
    set_target_properties(astgraph_engine PROPERTIES UNITY_BUILD ON)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Parallel compilation
set(CMAKE_JOB_POOL_COMPILE compile)
set(CMAKE_JOB_POOL_LINK link)
if(NOT CMAKE_JOB_POOLS)
    cmake_host_system_information(RESULT NCPU QUERY NUMBER_OF_LOGICAL_CORES)
    set(CMAKE_JOB_POOLS "compile=${NCPU};link=${NCPU}")
endif()

# ── Compiler Cache ───────────────────────────────────────────────
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
endif()

# ── Dependencies ────────────────────────────────────────────────
find_package(PkgConfig QUIET)

# ── SQLite3 ─────────────────────────────────────────────────────
# Each platform has its own sqlite3 story:
#   macOS:  Homebrew sqlite3 (supports extension loading)
#   Linux:  system libsqlite3-dev from apt
#   Windows: MinGW sqlite3 from choco/mingw
if(APPLE)
    # Homebrew sqlite3 — first try versioned Cellar, then opt symlink
    set(SQLITE3_CANDIDATE_LIBS
        "/opt/homebrew/Cellar/sqlite/3.53.3/lib"
        "/opt/homebrew/opt/sqlite/lib"
    )
    set(SQLITE3_CANDIDATE_INCS
        "/opt/homebrew/Cellar/sqlite/3.53.3/include"
        "/opt/homebrew/opt/sqlite/include"
    )
    set(SQLITE3_FOUND_PATH "")

    # Look for sqlite3 in each candidate dir
    foreach(_dir ${SQLITE3_CANDIDATE_LIBS})
        if(EXISTS "${_dir}/libsqlite3.dylib" OR EXISTS "${_dir}/libsqlite3.a")
            set(SQLITE3_FOUND_PATH "${_dir}")
            break()
        endif()
    endforeach()

    if(SQLITE3_FOUND_PATH)
        message(STATUS "SQLite3: found at ${SQLITE3_FOUND_PATH}")
        set(SQLITE3_LIBRARY_DIRS "${SQLITE3_FOUND_PATH}")
        link_directories(${SQLITE3_FOUND_PATH})
        set(SQLITE3_LIBRARIES sqlite3)
        # Find matching include dir
        foreach(_inc ${SQLITE3_CANDIDATE_INCS})
            if(EXISTS "${_inc}/sqlite3.h")
                set(SQLITE3_INCLUDE_DIRS "${_inc}")
                break()
            endif()
        endforeach()
    else()
        message(WARNING "Homebrew sqlite3 not found; falling back to system sqlite3")
        set(SQLITE3_LIBRARIES sqlite3)
        set(SQLITE3_INCLUDE_DIRS "")
    endif()
elseif(WIN32)
    # MinGW sqlite3 — available via PATH / choco
    set(SQLITE3_LIBRARIES sqlite3)
    set(SQLITE3_INCLUDE_DIRS "")
else()
    # Linux: system libsqlite3-dev
    set(SQLITE3_LIBRARIES sqlite3)
    set(SQLITE3_INCLUDE_DIRS "")
endif()

# ── tree-sitter (core C library) ────────────────────────────────
# Dynamically loaded — queried via pkg-config first, then manual search.
# Each platform has different default paths.
if(APPLE)
    # macOS: Homebrew installs to /opt/homebrew
    set(TREE_SITTER_PATHS /opt/homebrew /usr/local /usr)
elseif(WIN32)
    # Windows: MinGW / msys2
    set(TREE_SITTER_PATHS $ENV{MINGW_PREFIX} C:/msys64 /usr /usr/local)
else()
    # Linux: system packages install to /usr
    set(TREE_SITTER_PATHS /usr /usr/local)
endif()

pkg_check_modules(TREE_SITTER QUIET tree-sitter)
if(NOT TREE_SITTER_FOUND)
    message(STATUS "tree-sitter not found via pkg-config; searching ${TREE_SITTER_PATHS}")
    find_path(TREE_SITTER_INCLUDE_DIRS tree_sitter/api.h
        PATHS ${TREE_SITTER_PATHS}
        PATH_SUFFIXES include
        NO_DEFAULT_PATH)
    find_library(TREE_SITTER_LIBRARIES tree-sitter
        PATHS ${TREE_SITTER_PATHS}
        PATH_SUFFIXES lib
        NO_DEFAULT_PATH)

    # If not found in platform paths, try system defaults as fallback
    if(NOT TREE_SITTER_INCLUDE_DIRS)
        find_path(TREE_SITTER_INCLUDE_DIRS tree_sitter/api.h)
    endif()
    if(NOT TREE_SITTER_LIBRARIES)
        find_library(TREE_SITTER_LIBRARIES tree-sitter)
    endif()

    if(NOT TREE_SITTER_LIBRARIES)
        message(WARNING "tree-sitter library not found; using -ltree-sitter with default linker search")
        set(TREE_SITTER_LIBRARIES "tree-sitter")
    endif()
endif()

# Add Homebrew and MinGW lib paths so -ltree-sitter / -lsqlite3 resolve at link time
if(APPLE)
    link_directories(/opt/homebrew/lib)
endif()
if(WIN32 AND DEFINED ENV{MINGW_PREFIX})
    link_directories($ENV{MINGW_PREFIX}/lib)
endif()

# dlopen (for dynamic grammar loading)
set(CMAKE_DL_LIBS dl)

# ── Engine Library ──────────────────────────────────────────────

# Statically-linked tree-sitter grammar sources
# Set TREE_SITTER_NPM env var to override the default path (for CI cross-platform builds).
# Priority: 1) env var TREE_SITTER_NPM, 2) platform-default npm root, 3) auto-detect via npm root -g
if(DEFINED ENV{TREE_SITTER_NPM})
    set(TREE_SITTER_NPM "$ENV{TREE_SITTER_NPM}")
elseif(APPLE)
    # macOS: Homebrew on ARM, system on Intel
    if(EXISTS "/opt/homebrew/lib/node_modules")
        set(TREE_SITTER_NPM "/opt/homebrew/lib/node_modules")
    else()
        set(TREE_SITTER_NPM "/usr/local/lib/node_modules")
    endif()
elseif(WIN32)
    # Windows: %APPDATA%/npm/node_modules or %NODE_PATH%
    if(DEFINED ENV{APPDATA} AND EXISTS "$ENV{APPDATA}/npm/node_modules")
        set(TREE_SITTER_NPM "$ENV{APPDATA}/npm/node_modules")
    elseif(DEFINED ENV{NODE_PATH})
        set(TREE_SITTER_NPM "$ENV{NODE_PATH}")
    else()
        set(TREE_SITTER_NPM "")
    endif()
else()
    # Linux and other Unix: try npm root -g
    execute_process(COMMAND npm root -g
        OUTPUT_VARIABLE NPM_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(EXISTS "${NPM_ROOT}")
        set(TREE_SITTER_NPM "${NPM_ROOT}")
    elseif(EXISTS "/usr/local/lib/node_modules")
        set(TREE_SITTER_NPM "/usr/local/lib/node_modules")
    elseif(EXISTS "/usr/lib/node_modules")
        set(TREE_SITTER_NPM "/usr/lib/node_modules")
    else()
        message(WARNING "npm global root not found; grammar sources may be missing. Set TREE_SITTER_NPM env.")
        set(TREE_SITTER_NPM "")
    endif()
endif()
message(STATUS "TREE_SITTER_NPM: \"${TREE_SITTER_NPM}\"")
set(GRAMMAR_SOURCES
    ${TREE_SITTER_NPM}/tree-sitter-c/src/parser.c
    ${TREE_SITTER_NPM}/tree-sitter-cpp/src/parser.c
    ${TREE_SITTER_NPM}/tree-sitter-cpp/src/scanner.c
    ${TREE_SITTER_NPM}/tree-sitter-go/src/parser.c
    ${TREE_SITTER_NPM}/tree-sitter-java/src/parser.c
    ${TREE_SITTER_NPM}/tree-sitter-javascript/src/parser.c
    ${TREE_SITTER_NPM}/tree-sitter-javascript/src/scanner.c
    ${TREE_SITTER_NPM}/tree-sitter-python/src/parser.c
    ${TREE_SITTER_NPM}/tree-sitter-python/src/scanner.c
    ${TREE_SITTER_NPM}/tree-sitter-rust/src/parser.c
    ${TREE_SITTER_NPM}/tree-sitter-rust/src/scanner.c
    ${TREE_SITTER_NPM}/tree-sitter-swift/src/parser.c
    ${TREE_SITTER_NPM}/tree-sitter-swift/src/scanner.c
    ${TREE_SITTER_NPM}/tree-sitter-typescript/typescript/src/parser.c
    ${TREE_SITTER_NPM}/tree-sitter-typescript/typescript/src/scanner.c
    ${TREE_SITTER_NPM}/tree-sitter-typescript/tsx/src/parser.c
    ${TREE_SITTER_NPM}/tree-sitter-typescript/tsx/src/scanner.c
)

set(ENGINE_SOURCES
    ${GRAMMAR_SOURCES}
    src/engine.cpp
    src/engine_helpers.cpp
    src/engine_lifecycle.cpp
    src/engine_index.cpp
    src/engine_scanner.cpp
    src/engine_queries.cpp
    src/engine_ffi.cpp
    src/parser/parser.cpp
    src/ir/ir.cpp
    src/ir/ir_visitor.cpp
    src/ir/ir_translator.cpp
    src/ir/ir_complexity.cpp
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
    src/ir/translators/swift_visitor.cpp
    src/ir/translators/java_translator.cpp
    src/ir/translators/swift_translator.cpp
    src/ir/translators/scanner_visitor.cpp
    src/ir/translators/tsx_translator.cpp
    src/graph/graph_types.cpp
    src/graph/graph_builder.cpp
    src/store/store.cpp
    src/query/query_engine.cpp
    src/query/graph_query.cpp
    src/query/impact_analysis.cpp
    src/query/community_detection.cpp
    src/query/vector_search.cpp
    src/lsp/lsp_client.cpp
    src/resolver/resolver.cpp
    src/resolver/project_index.cpp
    src/resolver/project_resolver.cpp
    src/resolver/resolve_cache.cpp
    src/linker/linker.cpp
    src/filter_policy.cpp
   )

add_library(astgraph_engine STATIC ${ENGINE_SOURCES})

# Enable sqlite3 extension loading (required for sqlite-vec)
target_compile_definitions(astgraph_engine PUBLIC SQLITE_ENABLE_LOAD_EXTENSION)

target_include_directories(astgraph_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SQLITE3_INCLUDE_DIRS}
    # Grammar source directories (for #include "tree_sitter/parser.h")
    ${TREE_SITTER_NPM}/tree-sitter-c/src
    ${TREE_SITTER_NPM}/tree-sitter-cpp/src
    ${TREE_SITTER_NPM}/tree-sitter-go/src
    ${TREE_SITTER_NPM}/tree-sitter-java/src
    ${TREE_SITTER_NPM}/tree-sitter-javascript/src
    ${TREE_SITTER_NPM}/tree-sitter-python/src
    ${TREE_SITTER_NPM}/tree-sitter-rust/src
    ${TREE_SITTER_NPM}/tree-sitter-swift/src
    ${TREE_SITTER_NPM}/tree-sitter-typescript/typescript/src
    ${TREE_SITTER_NPM}/tree-sitter-typescript/tsx/src
    ${TREE_SITTER_NPM}/tree-sitter-typescript/common
)

# tree-sitter include dirs: guard against NOTFOUND (e.g. Windows where tree-sitter isn't installed)
if(TREE_SITTER_INCLUDE_DIRS AND NOT "${TREE_SITTER_INCLUDE_DIRS}" MATCHES "-NOTFOUND$")
    target_include_directories(astgraph_engine PUBLIC ${TREE_SITTER_INCLUDE_DIRS})
endif()

# tree-sitter library: guard against NOTFOUND
if(TREE_SITTER_LIBRARIES AND NOT "${TREE_SITTER_LIBRARIES}" MATCHES "-NOTFOUND$")
    target_link_libraries(astgraph_engine PUBLIC ${TREE_SITTER_LIBRARIES})
else()
    target_link_libraries(astgraph_engine PUBLIC tree-sitter)
endif()

target_link_libraries(astgraph_engine PUBLIC
    ${TREE_SITTER_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

# Ensure Homebrew libraries are found on macOS
if(APPLE)
    # Add Homebrew lib path so -ltree-sitter resolves at link time
    link_directories(/opt/homebrew/lib)
    # Homebrew SQLite (needed for sqlite-vec extension loading)
    # The system SQLite doesn't provide sqlite3_enable_load_extension / sqlite3_load_extension
    find_library(HOMEBREW_SQLITE3 sqlite3 PATHS /opt/homebrew/opt/sqlite/lib NO_DEFAULT_PATH)
    if(HOMEBREW_SQLITE3)
        target_link_libraries(astgraph_engine PUBLIC ${HOMEBREW_SQLITE3})
        target_include_directories(astgraph_engine PUBLIC /opt/homebrew/opt/sqlite/include)
    endif()
endif()

# On Linux, ensure tree-sitter from apt is found
# (libtree-sitter-dev installs to standard paths, so no extra config needed)

# On Windows, ensure npm global root is in the include path
if(WIN32)
    # Grammar include dirs may be accessed via NPM root; warn if missing
    if("${TREE_SITTER_NPM}" STREQUAL "")
        message(WARNING "TREE_SITTER_NPM is empty — grammar source files for static linking will be missing. "
                        "Run: npm install -g tree-sitter-python tree-sitter-c ...")
    endif()
endif()

# ── Grammar Build Script ────────────────────────────────────────
# Grammars are compiled as .so files using gcc (C compiler) since
# tree-sitter parser.c files use C99 designated initializers.
# They are loaded at runtime via dlopen.
#
# Build with: bash grammars/build.sh

# ── Tests ───────────────────────────────────────────────────────

option(BUILD_TESTS "Build tests" ON)

if(BUILD_TESTS)
    file(GLOB TEST_SOURCES tests/*.cpp)
    foreach(test_src ${TEST_SOURCES})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        target_link_libraries(${test_name} astgraph_engine)
    endforeach()
endif()

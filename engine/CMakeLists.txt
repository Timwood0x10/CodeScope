cmake_minimum_required(VERSION 3.30)
project(astgraph_engine VERSION 0.1.0 LANGUAGES CXX)

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
find_package(PkgConfig)

# Force homebrew sqlite3 (supports extension loading with SQLITE_ENABLE_LOAD_EXTENSION)
set(SQLITE3_INCLUDE_DIRS "/opt/homebrew/Cellar/sqlite/3.53.3/include")
set(SQLITE3_LIBRARY_DIRS "/opt/homebrew/Cellar/sqlite/3.53.3/lib")
set(SQLITE3_LIBRARIES "sqlite3")

# tree-sitter is linked dynamically (the core lib)
pkg_check_modules(TREE_SITTER REQUIRED tree-sitter)

# dlopen (for dynamic grammar loading)
set(CMAKE_DL_LIBS dl)

# ── Engine Library ──────────────────────────────────────────────

set(ENGINE_SOURCES
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
    src/ir/translators/java_translator.cpp
    src/ir/translators/swift_translator.cpp
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
    src/linker/linker.cpp
)

add_library(astgraph_engine STATIC ${ENGINE_SOURCES})

# Enable sqlite3 extension loading (required for sqlite-vec)
target_compile_definitions(astgraph_engine PUBLIC SQLITE_ENABLE_LOAD_EXTENSION)

target_include_directories(astgraph_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${TREE_SITTER_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
)

target_link_libraries(astgraph_engine PUBLIC
    ${TREE_SITTER_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

# Ensure Homebrew SQLite is found on macOS (needed for sqlite-vec extension loading)
# The system SQLite doesn't provide sqlite3_enable_load_extension / sqlite3_load_extension
if(APPLE)
    find_library(HOMEBREW_SQLITE3 sqlite3 PATHS /opt/homebrew/opt/sqlite/lib NO_DEFAULT_PATH)
    if(HOMEBREW_SQLITE3)
        target_link_libraries(astgraph_engine PUBLIC ${HOMEBREW_SQLITE3})
        target_include_directories(astgraph_engine PUBLIC /opt/homebrew/opt/sqlite/include)
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

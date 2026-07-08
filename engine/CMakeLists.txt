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
set(TREE_SITTER_LIB tree-sitter)
set(TREE_SITTER_INCDIR "${ts_repo_SOURCE_DIR}/lib/include" "${ts_repo_BINARY_DIR}/lib/include")

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

# ── sqlite-vec amalgamation (download once, compile as C) ───────
# Compiled directly into the engine — no runtime load_extension needed.
# If download fails, the engine still builds; vector search is simply unavailable.
set(SQLITE_VEC_SRC "${SQLITE3_DEPS_DIR}/sqlite-vec.c")
if(NOT EXISTS "${SQLITE_VEC_SRC}")
    message(STATUS "sqlite-vec: downloading amalgamation ${SQLITE_VEC_VERSION}...")
    file(DOWNLOAD "https://github.com/asg017/sqlite-vec/releases/download/${SQLITE_VEC_VERSION}/sqlite-vec-${SQLITE_VEC_VERSION}-amalgamation.zip"
        "${SQLITE3_DEPS_DIR}/sqlite-vec-amalgamation.zip" STATUS SQLITE_VEC_DL_ST)
    if(SQLITE_VEC_DL_ST EQUAL 0)
        file(ARCHIVE_EXTRACT INPUT "${SQLITE3_DEPS_DIR}/sqlite-vec-amalgamation.zip"
            DESTINATION "${SQLITE3_DEPS_DIR}/")
    else()
        message(WARNING "sqlite-vec: amalgamation download failed (status=${SQLITE_VEC_DL_ST}), vector search will be unavailable")
    endif()
endif()

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
    src/ir/translators/java_translator.cpp
    src/ir/translators/scanner_visitor.cpp
    src/ir/translators/tsx_translator.cpp
    src/graph/graph_types.cpp
    src/graph/graph_builder.cpp
    src/store/store_core.cpp
    src/store/store.cpp
    src/query/query_engine.cpp
    src/query/query_analysis.cpp
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
    src/platform_win.cpp
    ${SQLITE3_AMAL_SRC}
)

# Add sqlite-vec.c if downloaded successfully
if(EXISTS "${SQLITE_VEC_SRC}")
    list(APPEND ENGINE_SOURCES ${SQLITE_VEC_SRC})
    message(STATUS "sqlite-vec: amalgamation found, compiling into engine")
endif()

add_library(astgraph_engine STATIC ${ENGINE_SOURCES})

# Compile sqlite3.c and sqlite-vec.c as C, not C++
set_source_files_properties(${SQLITE3_AMAL_SRC} PROPERTIES LANGUAGE C)
if(EXISTS "${SQLITE_VEC_SRC}")
    set_source_files_properties(${SQLITE_VEC_SRC} PROPERTIES LANGUAGE C)
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
    ${TREE_SITTER_INCLUDE_DIRS}
)

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

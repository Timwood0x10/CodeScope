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

# ── Dependencies ────────────────────────────────────────────────
find_package(PkgConfig QUIET)

# ── macOS ───────────────────────────────────────────────────────
# Uses Homebrew packages. CI installs: brew install sqlite tree-sitter
if(APPLE)
    set(SQLITE3_INCLUDE_DIRS /opt/homebrew/opt/sqlite/include)
    link_directories(/opt/homebrew/lib /opt/homebrew/opt/sqlite/lib)
    set(SQLITE3_LIBRARIES sqlite3)
    set(CMAKE_DL_LIBS dl)

    pkg_check_modules(TS_PKG QUIET tree-sitter)
    if(TS_PKG_FOUND)
        set(TREE_SITTER_INCDIR ${TS_PKG_INCLUDE_DIRS})
        set(TREE_SITTER_LIB ${TS_PKG_LIBRARIES})
    else()
        find_path(TREE_SITTER_INCDIR tree_sitter/api.h /opt/homebrew/include /usr/local/include)
        find_library(TREE_SITTER_LIB tree-sitter /opt/homebrew/lib /usr/local/lib)
        if(NOT TREE_SITTER_LIB)
            set(TREE_SITTER_LIB tree-sitter)
        endif()
    endif()

# ── Linux ───────────────────────────────────────────────────────
# Uses apt packages: libsqlite3-dev libtree-sitter-dev
elseif(UNIX AND NOT APPLE)
    set(SQLITE3_INCLUDE_DIRS "")
    set(SQLITE3_LIBRARIES sqlite3)
    set(CMAKE_DL_LIBS dl)

    find_path(TREE_SITTER_INCDIR tree_sitter/api.h /usr/include)
    find_library(TREE_SITTER_LIB tree-sitter /usr/lib/x86_64-linux-gnu /usr/lib)
    if(NOT TREE_SITTER_LIB)
        set(TREE_SITTER_LIB tree-sitter)
    endif()

# ── Windows ─────────────────────────────────────────────────────
# MinGW doesn't ship sqlite3-dev or tree-sitter.
# Downloads sqlite3 amalgamation + builds tree-sitter from source.
elseif(WIN32)
    set(CMAKE_DL_LIBS "")

    # -- sqlite3 amalgamation (download once, compile as C) ------
    set(SQLITE3_DEPS_DIR "${CMAKE_BINARY_DIR}/_deps")
    set(SQLITE3_AMAL_SRC "${SQLITE3_DEPS_DIR}/sqlite3.c")
    if(NOT EXISTS "${SQLITE3_AMAL_SRC}")
        message(STATUS "SQLite3: downloading amalgamation for Windows...")
        file(DOWNLOAD "https://www.sqlite.org/2025/sqlite-amalgamation-3490100.zip"
            "${SQLITE3_DEPS_DIR}/sqlite-amalgamation.zip" STATUS SQLITE_DL_ST)
        if(SQLITE_DL_ST EQUAL 0)
            file(ARCHIVE_EXTRACT INPUT "${SQLITE3_DEPS_DIR}/sqlite-amalgamation.zip"
                DESTINATION "${SQLITE3_DEPS_DIR}/")
            file(GLOB SQLITE_EXTRACTED LIST_DIRECTORIES true "${SQLITE3_DEPS_DIR}/sqlite-amalgamation-*")
            if(SQLITE_EXTRACTED)
                file(GLOB SQLITE_FILES "${SQLITE_EXTRACTED}/*")
                file(COPY ${SQLITE_FILES} DESTINATION "${SQLITE3_DEPS_DIR}/")
            endif()
        endif()
    endif()
    set(SQLITE3_INCLUDE_DIRS "${SQLITE3_DEPS_DIR}")
    set(SQLITE3_AMAL_SRC "${SQLITE3_DEPS_DIR}/sqlite3.c")

    # -- tree-sitter (build from source via FetchContent) ---------
    include(FetchContent)
    FetchContent_Declare(ts_repo GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
        GIT_TAG v0.24.7 GIT_SHALLOW TRUE SOURCE_SUBDIR lib)
    set(BUILD_SHARED_LIBS OFF)
    set(TREE_SITTER_ENABLE_TESTING OFF)
    set(TREE_SITTER_ENABLE_EXAMPLES OFF)
    FetchContent_MakeAvailable(ts_repo)
    set(TREE_SITTER_LIB tree-sitter)
    set(TREE_SITTER_INCDIR "${ts_repo_SOURCE_DIR}/include" "${ts_repo_BINARY_DIR}/include")
endif()

# ── TREE_SITTER_NPM (grammar source paths) ──────────────────────
if(DEFINED ENV{TREE_SITTER_NPM})
    set(TREE_SITTER_NPM "$ENV{TREE_SITTER_NPM}")
elseif(APPLE)
    if(EXISTS "/opt/homebrew/lib/node_modules")
        set(TREE_SITTER_NPM "/opt/homebrew/lib/node_modules")
    else()
        set(TREE_SITTER_NPM "/usr/local/lib/node_modules")
    endif()
elseif(WIN32)
    if(DEFINED ENV{APPDATA} AND EXISTS "$ENV{APPDATA}/npm/node_modules")
        set(TREE_SITTER_NPM "$ENV{APPDATA}/npm/node_modules")
    elseif(DEFINED ENV{NODE_PATH})
        set(TREE_SITTER_NPM "$ENV{NODE_PATH}")
    else()
        execute_process(COMMAND npm root -g
            OUTPUT_VARIABLE NPM_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(EXISTS "${NPM_ROOT}")
            set(TREE_SITTER_NPM "${NPM_ROOT}")
        else()
            set(TREE_SITTER_NPM "")
        endif()
    endif()
else()
    execute_process(COMMAND npm root -g
        OUTPUT_VARIABLE NPM_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(EXISTS "${NPM_ROOT}")
        set(TREE_SITTER_NPM "${NPM_ROOT}")
    elseif(EXISTS "/usr/local/lib/node_modules")
        set(TREE_SITTER_NPM "/usr/local/lib/node_modules")
    elseif(EXISTS "/usr/lib/node_modules")
        set(TREE_SITTER_NPM "/usr/lib/node_modules")
    else()
        set(TREE_SITTER_NPM "")
    endif()
endif()
message(STATUS "TREE_SITTER_NPM: \"${TREE_SITTER_NPM}\"")

# ── Grammar source files ────────────────────────────────────────
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

# ── Windows only: add sqlite3 amalgamation to sources ────────────
if(WIN32)
    list(APPEND ENGINE_SOURCES ${SQLITE3_AMAL_SRC})
endif()

add_library(astgraph_engine STATIC ${ENGINE_SOURCES})

# Windows: compile sqlite3.c as C, not C++
if(WIN32)
    set_source_files_properties(${SQLITE3_AMAL_SRC} PROPERTIES LANGUAGE C)
endif()

target_compile_definitions(astgraph_engine PUBLIC SQLITE_ENABLE_LOAD_EXTENSION)

target_include_directories(astgraph_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SQLITE3_INCLUDE_DIRS}
    ${TREE_SITTER_INCDIR}
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

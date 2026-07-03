cmake_minimum_required(VERSION 3.30)
project(astgraph_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Dependencies ────────────────────────────────────────────────
find_package(PkgConfig)

pkg_check_modules(SQLITE3 REQUIRED sqlite3)

# tree-sitter is linked dynamically (the core lib)
pkg_check_modules(TREE_SITTER REQUIRED tree-sitter)

# dlopen (for dynamic grammar loading)
set(CMAKE_DL_LIBS dl)

# ── Engine Library ──────────────────────────────────────────────

set(ENGINE_SOURCES
    src/engine.cpp
    src/parser/parser.cpp
    src/ir/ir.cpp
    src/ir/ir_visitor.cpp
    src/ir/ir_translator.cpp
    src/ir/ir_complexity.cpp
    src/ir/translators/python_translator.cpp
    src/ir/translators/go_translator.cpp
    src/ir/translators/c_translator.cpp
    src/ir/translators/cpp_translator.cpp
    src/ir/translators/rust_translator.cpp
    src/ir/translators/javascript_translator.cpp
    src/ir/translators/typescript_translator.cpp
    src/ir/translators/java_translator.cpp
    src/graph/graph_types.cpp
    src/graph/graph_builder.cpp
    src/store/store.cpp
    src/query/query_engine.cpp
    src/query/graph_query.cpp
)

add_library(astgraph_engine STATIC ${ENGINE_SOURCES})

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

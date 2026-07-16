# gen-sqlite-vec-header.cmake — regenerate third_party/sqlite-vec/sqlite-vec.h
# Mirrors the header generation that used to live in engine/CMakeLists.txt.
# Invoked by scripts/vendor-deps.sh via:
#   cmake -DSQLITE_VEC_TMPL=... -DSQLITE_VEC_OUT=... -DSQLITE_VEC_VERSION=... -P $0

if(NOT SQLITE_VEC_TMPL OR NOT SQLITE_VEC_OUT OR NOT SQLITE_VEC_VERSION)
  message(FATAL_ERROR "gen-sqlite-vec-header.cmake requires -DSQLITE_VEC_TMPL, -DSQLITE_VEC_OUT, -DSQLITE_VEC_VERSION")
endif()

# Parse the version string: v0.1.10-alpha.4 → major=0, minor=1, patch=10
string(REGEX MATCH "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ "${SQLITE_VEC_VERSION}")
set(VERSION_MAJOR ${CMAKE_MATCH_1})
set(VERSION_MINOR ${CMAKE_MATCH_2})
set(VERSION_PATCH ${CMAKE_MATCH_3})
set(VERSION "${SQLITE_VEC_VERSION}")  # full version string for SQLITE_VEC_VERSION macro

file(READ "${SQLITE_VEC_TMPL}" VEC_H_TMPL)
string(REPLACE "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}-alpha.4"
    "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}" _dummy "${SQLITE_VEC_VERSION}")
string(CONFIGURE "${VEC_H_TMPL}" VEC_H_CONTENT)
file(WRITE "${SQLITE_VEC_OUT}" "${VEC_H_CONTENT}")
message(STATUS "sqlite-vec.h written to ${SQLITE_VEC_OUT}")

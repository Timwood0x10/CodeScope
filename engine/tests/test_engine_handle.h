#ifndef TEST_ENGINE_HANDLE_H
#define TEST_ENGINE_HANDLE_H

// One engine instance handle per translation unit (TD-1 knife 3).
//
// The C ABI is handle-based: engine_create() returns the instance and every
// stateful entry point takes it as its first parameter. The suite used to call
// engine_init()/engine_shutdown(), which reached for process-global state; it
// now keeps the instance in `g_engine`, assigned by engine_create() and
// released by engine_destroy().
//
// The include guard is load-bearing: a test file and the shared test_e2e.h
// harness both include this header, and one translation unit may define the
// object only once. Because the object is `static`, every translation unit
// gets its own instance — tests stay independent without sharing a handle.
//
// Usage: `g_engine = engine_create(db_path);` ... `engine_destroy(g_engine);`.
// engine_destroy() accepts null, so clearing the handle after releasing it
// makes a second shutdown a no-op instead of a double free.

#include "../include/engine.h"

/// Handle of the engine instance the current test drives. Null before the
/// first engine_create() and again after engine_destroy().
static engine_t g_engine = nullptr;

#endif // TEST_ENGINE_HANDLE_H

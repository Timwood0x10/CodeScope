#include "test_e2e.h"

int main() {
    // Go FP verification: builtins + unexported + FFI
    const char* code = R"(package main

import "fmt"

// User-defined function — should appear in callees
func userFunction(x int) int {
    return x * 2
}

// Unexported function — should NOT be callable cross-package
// (visibility rule test: lowercase = unexported in Go)
func unexportedHelper(x int) int {
    return x + 1
}

// Exported function — should appear in callees
func ExportedWorker(x int) int {
    return x * 3
}

func mainFunc() {
    // Legitimate call to user-defined function
    r1 := userFunction(5)

    // Built-in calls that should NOT create call edges:
    // Reference: codebase-memory-mcp (MIT) go_lsp.c :: is_go_builtin_func()
    s := []int{1, 2, 3}
    _ = len(s)       // Go builtin — must NOT create "Len" call edge
    s = append(s, 4) // Go builtin — must NOT create "Append" call edge
    _ = copy(s, s)   // Go builtin — must NOT create "Copy" call edge
    _ = cap(s)       // Go builtin — must NOT create "Cap" call edge
    m := make(map[int]int, 10) // Go builtin — must NOT create "Make" call edge
    _ = m
    _ = new(int)     // Go builtin — must NOT create "New" call edge
    delete(m, 1)     // Go builtin — must NOT create "Delete" call edge

    // Call to exported function — should appear in callees
    r2 := ExportedWorker(r1)

    // fmt.Println is NOT a Go builtin — it's a package function.
    // It should NOT create a call edge to "Println" because
    // the parser sees it as a selector expression (fmt.Println),
    // not a bare "Println" call.
    fmt.Println(r2)

    // Call to unexported function in same package — should appear
    // (visibility rule only blocks cross-package, not same-package)
    _ = unexportedHelper(r2)
}
)";

    const char* builtins[] = {
        "Len", "Append", "Copy", "Cap", "Make", "New", "Delete",
        "Println", // fmt.Println is selector expression, not bare call
        nullptr
    };

    runFPVerificationTest("go", code, "/tmp/test_fp_go.go",
                          "mainFunc", "userFunction", builtins);
    return 0;
}
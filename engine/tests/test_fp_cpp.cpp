#include "test_e2e.h"

int main() {
    // C++ FP verification: builtins + stdlib + compiler intrinsics
    // NOTE: use standalone functions, not class methods, for reliable
    // call-edge detection in the old API translator.
    const char* code = R"(
#include <cstdio>

// User-defined function — should appear in callees
int user_function(int x) {
    return x * 2;
}

int mainFunc() {
    int r1 = user_function(5);

    // C stdlib calls that should NOT create call edges:
    // Reference: codebase-memory-mcp (MIT) c_lsp.c :: is_c_builtin_func()
    printf("r1 = %d\n", r1);  // printf — must NOT create "printf" call edge

    // Compiler builtins that should NOT create call edges:
    if (__builtin_expect(r1 > 0, 1)) {
        return r1;
    }

    return 0;
}
)";

    const char* builtins[] = {
        "printf", "__builtin_expect",
        nullptr
    };

    runFPVerificationTest("cpp", code, "/tmp/test_fp_cpp.cpp",
                          "mainFunc", "user_function", builtins);
    return 0;
}
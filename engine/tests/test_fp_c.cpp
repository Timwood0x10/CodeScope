#include "test_e2e.h"

int main() {
    // C FP verification: builtins + stdlib + compiler intrinsics
    const char* code = R"(
// User-defined function — should appear in callees
int user_function(int x) {
    return x * 2;
}

// A function that calls stdlib — should NOT create edges to stdlib
int mainFunc() {
    int r1 = user_function(5);

    // C stdlib calls that should NOT create call edges:
    // Reference: codebase-memory-mcp (MIT) c_lsp.c :: is_c_builtin_func()
    printf("r1 = %d\n", r1);  // printf — must NOT create "printf" call edge
    fprintf(stderr, "err");    // fprintf — must NOT create "fprintf" call edge
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", r1); // snprintf — must NOT create "snprintf" edge

    // Compiler builtins that should NOT create call edges:
    // __builtin_expect — must NOT create call edge
    if (__builtin_expect(r1 > 0, 1)) {
        return r1;
    }

    return 0;
}

// __builtin_expect wrapper — should create call edge to user_function
int wrapper(int x) {
    return user_function(x);
}
)";

    const char* builtins[] = {
        "printf", "fprintf", "snprintf",
        "__builtin_expect",
        nullptr
    };

    runFPVerificationTest("c", code, "/tmp/test_fp_c.c",
                          "mainFunc", "user_function", builtins);
    return 0;
}
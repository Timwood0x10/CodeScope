#include "test_e2e.h"

int main() {
    // TypeScript FP verification: builtins + type constructors
    const char* code = R"TS(
// User-defined function — should appear in callees
function userFunction(x: number): number {
    return x * 2;
}

function mainFunc(): number {
    let r1 = userFunction(5);

    // TS/JS built-in functions that should NOT create call edges:
    // Reference: codebase-memory-mcp (MIT) ts_lsp.c :: builtins[]
    let s: string = String(r1);          // String — must NOT create "String" call edge
    let n: number = Number("42");        // Number — must NOT create "Number" call edge
    let b: boolean = Boolean(1);         // Boolean — must NOT create "Boolean" call edge
    let arr: number[] = Array(3);        // Array — must NOT create "Array" call edge

    let parsed: number = parseInt("42"); // parseInt — must NOT create "parseInt" call edge
    let pf: number = parseFloat("3.14"); // parseFloat — must NOT create "parseFloat" call edge

    return r1;
}
)TS";

    const char* builtins[] = {
        "String", "Number", "Boolean", "Array",
        "parseInt", "parseFloat",
        nullptr
    };

    runFPVerificationTest("ts", code, "/tmp/test_fp_ts.ts",
                          "mainFunc", "userFunction", builtins);
    return 0;
}
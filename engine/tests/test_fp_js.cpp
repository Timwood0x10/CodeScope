#include "test_e2e.h"

int main() {
    // JavaScript FP verification: builtins + global constructors
    const char* code = R"(
// User-defined function — should appear in callees
function userFunction(x) {
    return x * 2;
}

function mainFunc() {
    let r1 = userFunction(5);

    // JS built-in functions that should NOT create call edges:
    // Reference: codebase-memory-mcp (MIT) ts_lsp.c :: builtins[]
    let s = String(r1);          // String — must NOT create "String" call edge
    let n = Number("42");        // Number — must NOT create "Number" call edge
    let b = Boolean(1);          // Boolean — must NOT create "Boolean" call edge
    let arr = Array(3);          // Array — must NOT create "Array" call edge
    let obj = Object();          // Object — must NOT create "Object" call edge

    let parsed = parseInt("42"); // parseInt — must NOT create "parseInt" call edge
    let pf = parseFloat("3.14"); // parseFloat — must NOT create "parseFloat" call edge
    let is_nan = isNaN(r1);      // isNaN — must NOT create "isNaN" call edge
    let is_fin = isFinite(r1);   // isFinite — must NOT create "isFinite" call edge

    // eval is a builtin — must NOT create "eval" call edge
    // (but we don't use it here for safety)

    // encodeURIComponent — must NOT create call edge
    let enc = encodeURIComponent("hello");

    return r1;
}
)";

    const char* builtins[] = {
        "String", "Number", "Boolean", "Array", "Object",
        "parseInt", "parseFloat", "isNaN", "isFinite",
        "encodeURIComponent",
        nullptr
    };

    runFPVerificationTest("js", code, "/tmp/test_fp_js.js",
                          "mainFunc", "userFunction", builtins);
    return 0;
}
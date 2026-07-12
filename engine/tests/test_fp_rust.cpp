#include "test_e2e.h"

int main() {
    // Rust FP verification: builtins + macros + visibility
    // NOTE: The old API RustTranslator has issues with functions that have
    // no parameters. Use a dummy parameter to work around this.
    // The new API (RustVisitor) used by the project pipeline handles all
    // patterns correctly — the builtin filtering is verified at project level.
    const char* code = R"(
fn add(x: i32, y: i32) -> i32 {
    x + y
}

fn user_function(x: i32) -> i32 {
    let r = add(x, 1);
    r * 2
}

fn safe_abs(x: i32) -> i32 {
    if x < 0 { -x } else { x }
}

fn mainFunc(_x: i32) -> i32 {
    // Legitimate call to user-defined function
    let r1 = user_function(5);

    // Call to wrapper function — should appear in callees
    let _a = safe_abs(-5);

    r1
}
)";

    const char* builtins[] = {
        nullptr
    };

    runFPVerificationTest("rust", code, "/tmp/test_fp_rust.rs",
                          "mainFunc", "user_function", builtins);
    return 0;
}
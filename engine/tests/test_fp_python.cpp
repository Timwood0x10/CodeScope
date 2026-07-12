#include "test_e2e.h"

int main() {
    // Python FP verification: builtins + dunder methods
    const char* code = R"PY(
# User-defined function — should appear in callees
def user_function(x):
    return x * 2

# Private function (by convention) — visibility rule test
def _private_helper(x):
    return x + 1

def mainFunc():
    # Legitimate call to user-defined function
    r1 = user_function(5)

    # Built-in calls that should NOT create call edges:
    # Reference: codebase-memory-mcp (MIT) helpers.c :: python_resolvable_builtins
    s = [1, 2, 3]
    _ = len(s)          # Python builtin — must NOT create "len" call edge
    _ = str(r1)         # Python builtin — must NOT create "str" call edge
    _ = int(r1)         # Python builtin — must NOT create "int" call edge
    _ = list(s)         # Python builtin — must NOT create "list" call edge
    _ = dict(a=1)       # Python builtin — must NOT create "dict" call edge
    _ = range(10)       # Python builtin — must NOT create "range" call edge
    _ = print(r1)       # Python builtin — must NOT create "print" call edge
    _ = type(r1)        # Python builtin — must NOT create "type" call edge
    _ = bool(r1)        # Python builtin — must NOT create "bool" call edge
    _ = float(r1)       # Python builtin — must NOT create "float" call edge
    _ = max(s)          # Python builtin — must NOT create "max" call edge
    _ = min(s)          # Python builtin — must NOT create "min" call edge
    _ = sum(s)          # Python builtin — must NOT create "sum" call edge
    _ = sorted(s)       # Python builtin — must NOT create "sorted" call edge

    # Call to private function — should be visible (same-module convention)
    _ = _private_helper(r1)
)PY";

    const char* builtins[] = {
        "len", "str", "int", "list", "dict", "range",
        "print", "type", "bool", "float", "max", "min",
        "sum", "sorted",
        nullptr
    };

    runFPVerificationTest("python", code, "/tmp/test_fp_python.py",
                          "mainFunc", "user_function", builtins);
    return 0;
}
#include "test_e2e.h"

int main() {
    const char* code = R"(fn add(a: i32, b: i32) -> i32 {
    a + b
}

fn multiply(a: i32, b: i32) -> i32 {
    let mut result = 0;
    for _ in 0..b {
        result = add(result, a);
    }
    result
}

fn compute(x: i32, y: i32) -> i32 {
    let a = add(x, y);
    let b = add(x, y);
    multiply(a, b)
}

fn main() {
    let r = compute(5, 3);
    println!("{}", r);
}
)";

    const char* defs[] = {"add", "multiply", "compute"};
    runE2eTest("rust", code, "/tmp/test_math.rs",
               defs, 3,
               "add", "multiply",
               "add",
               "compute",
               "main", nullptr);
    return 0;
}

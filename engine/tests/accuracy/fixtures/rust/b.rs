// Second Rust fixture file: cross-file callee + homonym.

fn bravo(x: i32) -> i32 {
    x
}

fn helper() -> i32 {
    // Same-name function in b.rs; a.rs does NOT call it.
    42
}

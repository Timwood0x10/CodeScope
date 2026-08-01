// Second C++ fixture file: cross-file callee + homonym.

int bravo(int x) {
    return x;
}

int helper() {
    // Same-name function in b.cpp; a.cpp does NOT call it.
    return 42;
}

// Second JS fixture file: cross-file callee + homonym.

function bravo(x) {
    return x;
}

function helper() {
    // Same-name function in b.js; a.js does NOT call it.
    return 42;
}

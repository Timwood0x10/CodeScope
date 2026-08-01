// Second TS fixture file: cross-file callee + homonym.

function bravo(x: number): number {
    return x;
}

function helper(): number {
    // Same-name function in b.ts; a.ts does NOT call it.
    return 42;
}

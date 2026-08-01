// TypeScript accuracy fixture: typed functions, method calls, builtins.

class Renderer {
    render(): number {
        return 1;
    }
}

function alpha(x: number): number {
    // Cross-file bare-name call to bravo (defined in b.ts).
    return bravo(x);
}

function mainFunc(): void {
    let r = alpha(1);
    // Builtin — must NOT create an internal call edge.
    let n = Math.max(1, 2);
    // Method call on a typed instance — may be unresolved.
    let obj = new Renderer();
    obj.render();
}

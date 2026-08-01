// TypeScript accuracy fixture: typed functions, method calls, builtins.

class Renderer {
    render(): number {
        return 1;
    }
}

class Logger {
    log(msg: string): void {
        // intentionally empty
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
    // Method call on a typed instance — type annotation enables
    // receiver_type inference for obj.render().
    let obj: Renderer = new Renderer();
    obj.render();
    // Another typed variable with a different receiver type.
    let logger: Logger = new Logger();
    logger.log("hello");
    // this.method() is not tested here (no class method body calls
    // this.method()), but class_scope_stack_ supports it.
}

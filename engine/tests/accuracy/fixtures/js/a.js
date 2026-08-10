// JavaScript accuracy fixture: functions, method calls, builtins.

function alpha(x) {
    // Cross-file bare-name call to bravo (defined in b.js).
    return bravo(x);
}

function mainFunc() {
    let r = alpha(1);
    // Builtin — must NOT create an internal call edge.
    let n = Math.max(1, 2);
    // Method call on an object — may be unresolved.
    let obj = { render: () => 1 };
    obj.render();
}

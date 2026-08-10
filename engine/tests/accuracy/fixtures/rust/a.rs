// Rust accuracy fixture: free functions, methods, associated functions.
//
// Covers Step 4 (plan §4D) scenarios:
//   - obj.method() with receiver_type inferred from let declaration
//   - self.method() with receiver_type from impl scope
//   - Type::new() associated function (constructor)
//   - stdlib call (must NOT create internal edge)

struct Container {
    val: i32,
}

impl Container {
    fn new(val: i32) -> Container {
        Container { val }
    }
    fn get(&self) -> i32 {
        self.val
    }
    fn helper(&self) -> i32 {
        // self.method() — receiver_type should be Container.
        self.get()
    }
}

// alpha calls bravo (cross-file bare name).
fn alpha(x: i32) -> i32 {
    bravo(x)
}

// main_func calls alpha (intra-file), vec! macro (stdlib),
// Container::new() (associated function), and c.get() (method call).
fn main_func() {
    let _ = alpha(1);
    let v = vec![1, 2, 3];
    let c = Container::new(5);
    let _ = c.get();
    let _ = c.helper();
    let _ = v.len();
}

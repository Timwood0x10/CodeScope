// Rust accuracy fixture: free functions, methods, stdlib.

struct Container {
    val: i32,
}

impl Container {
    fn get(&self) -> i32 {
        self.val
    }
}

// alpha calls bravo (cross-file bare name).
fn alpha(x: i32) -> i32 {
    bravo(x)
}

// main_func calls alpha (intra-file), vec! macro (stdlib), and
// c.get() (method call).
fn main_func() {
    let _ = alpha(1);
    let v = vec![1, 2, 3];
    let c = Container { val: 5 };
    let _ = c.get();
    let _ = v.len();
}

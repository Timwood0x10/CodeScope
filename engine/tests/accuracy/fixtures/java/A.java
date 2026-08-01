// Java accuracy fixture: static/instance methods, stdlib.

public class A {
    // alpha calls bravo (cross-file, same package, bare name).
    public static int alpha(int x) {
        return B.bravo(x);
    }

    // mainFunc calls alpha (intra-file), Math.max (stdlib), and
    // new B() (constructor).
    public static void mainFunc() {
        int r = alpha(1);
        int m = Math.max(1, 2);
        B b = new B();
        b.run();
    }
}

// Second Java fixture file: cross-file callee + homonym.

public class B {
    public static int bravo(int x) {
        return x;
    }

    public void run() {
        // Instance method; A.mainFunc calls it via b.run().
        System.out.println("run");
    }

    public static int helper() {
        // Same-name method in B; A does NOT call it.
        return 42;
    }
}

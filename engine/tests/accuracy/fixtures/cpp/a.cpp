// C++ accuracy fixture: method calls, static methods, constructor, stdlib.
//
// Covers Step 4 (plan §4C) scenarios:
//   - obj.method() with receiver_type inferred from declaration
//   - this->method() with receiver_type from class scope
//   - Type::staticMethod() with qualified_identifier
//   - constructor call (new Expression)
//   - stdlib call (must NOT create internal edge)

#include <vector>
#include <algorithm>

struct Point {
    int x;
    int y;
    int adder(Point &a) { return a.x; }
    static int create() { return 0; }
    int helper() { return this->adder(*this); }
};

// alpha calls bravo (cross-file, bare name).
int alpha(int x) {
    return bravo(x);
}

// mainFunc calls alpha (intra-file), std::sort (stdlib),
// p.adder() (method call on instance), Point::create() (static method),
// and p.helper() (which internally calls this->adder()).
int mainFunc() {
    int r = alpha(1);
    std::vector<int> v = {3, 1, 2};
    std::sort(v.begin(), v.end());
    Point p{1, 2};
    return p.adder(p) + Point::create() + p.helper();
}

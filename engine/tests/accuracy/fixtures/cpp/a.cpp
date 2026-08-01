// C++ accuracy fixture: same-name functions, method calls, stdlib.

#include <vector>
#include <algorithm>

struct Point {
    int x;
    int y;
    int adder(Point &a) { return a.x; }
};

// alpha calls bravo (cross-file, bare name).
int alpha(int x) {
    return bravo(x);
}

// mainFunc calls alpha (intra-file), std::sort (stdlib), and
// p.adder() (method call on instance).
int mainFunc() {
    int r = alpha(1);
    std::vector<int> v = {3, 1, 2};
    std::sort(v.begin(), v.end());
    Point p{1, 2};
    return p.adder(p);
}

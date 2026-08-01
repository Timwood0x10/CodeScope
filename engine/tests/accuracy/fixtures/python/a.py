"""Python accuracy fixture: same-name methods, cross-file calls, builtins.

Covers Step 4 (plan §4B) scenarios:
  - self.method() with receiver_type inferred from class scope
  - cls.method() with receiver_type inferred from class scope
  - obj.method() with receiver_type inferred from constructor assignment
  - cross-file bare-name call
  - builtin call (must NOT create an internal edge)
  - constructor call
  - same-name method on different classes (homonym disambiguation)
"""


def alpha(x):
    # Cross-file bare-name call to bravo (defined in b.py).
    return bravo(x)


def main_func():
    # Intra-file bare-name call.
    _ = alpha(1)
    # Builtin call — must NOT create an internal call edge.
    _ = len([1, 2])
    # Constructor call — receiver_type can be inferred for subsequent
    # method calls on the same variable.
    obj = Timeline()
    # Method call on a typed instance — receiver_type should be Timeline.
    _ = obj.render()
    # Second constructor with same-name method on different class.
    box = Box()
    _ = box.render()


class Timeline:
    def render(self):
        # self.method() — receiver_type should be Timeline.
        return self._internal()

    def _internal(self):
        return [1, 2, 3]

    @classmethod
    def create(cls):
        # cls.method() — receiver_type should be Timeline.
        return cls.render()


class Box:
    def render(self):
        # Same-name method on a different class — homonym.
        return 0

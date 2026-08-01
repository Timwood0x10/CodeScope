"""Python accuracy fixture: same-name methods, cross-file calls, builtins."""


def alpha(x):
    # Cross-file bare-name call to bravo (defined in b.py).
    return bravo(x)


def main_func():
    # Intra-file bare-name call.
    _ = alpha(1)
    # Builtin call — must NOT create an internal call edge.
    _ = len([1, 2])
    # Method call on an instance — receiver-based, may be unresolved.
    obj = Timeline()
    _ = obj.render()


class Timeline:
    def render(self):
        return [1, 2, 3]

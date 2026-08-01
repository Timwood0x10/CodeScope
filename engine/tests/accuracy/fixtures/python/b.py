"""Second Python fixture file: cross-file callee + homonym."""


def bravo(x):
    return x


def helper():
    # Same-name function in b.py; a.py does NOT call it.
    return 42

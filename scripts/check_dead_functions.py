#!/usr/bin/env python3
"""Reject functions that are defined but never referenced anywhere.

`-Wunused-function` only catches `static` definitions; externally linked
functions can rot forever. This scans the project's own C sources (src/ and
tests/, not vendored third-party code) and reports every function definition
with no reference outside its own prototype.

It understands the two indirections this codebase uses:

  * the unit-test registry, where `TEST(foo)` expands to a call to
    `test_foo`;
  * platform shims that alias libc names to `ccode_win32_*` via `#define`.

Functions marked `__attribute__((constructor))` or `((destructor))` are
invoked by the runtime and are treated as used.

Usage: scripts/check_dead_functions.py [--roots src tests]
Exit code: 0 when clean, 1 when dead definitions are found.
"""

import argparse
import os
import re
import sys

DEF_RE = re.compile(
    r'^[A-Za-z_][A-Za-z0-9_ \t\*]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;]*$')
KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "else",
            "do", "catch"}
SKIP_DIRS = {".build", "__pycache__", "fixtures"}
CTOR_RE = re.compile(r'__attribute__\s*\(\s*\(\s*(constructor|destructor)\b')


def is_definition(lines, i):
    """True when lines[i] starts a top-level function definition."""
    ln = lines[i]
    if not ln or ln[0] in " \t#}":
        return None
    if ln.rstrip().endswith(";"):
        return None
    m = DEF_RE.match(ln)
    if not m or m.group(1) in KEYWORDS:
        return None
    if "{" in ln:
        return m.group(1)
    joined = ln.rstrip()
    j = i + 1
    while j < len(lines) and "{" not in joined and ";" not in joined \
            and j < i + 12:
        joined += " " + lines[j].strip()
        j += 1
    if ";" in joined and "{" not in joined:
        return None
    return m.group(1)


def collect_definitions(roots):
    defs = []
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for fn in filenames:
                if not fn.endswith(".c"):
                    continue
                path = os.path.join(dirpath, fn)
                lines = open(path, encoding="utf-8",
                             errors="replace").read().splitlines()
                for i, _ in enumerate(lines):
                    name = is_definition(lines, i)
                    if not name:
                        continue
                    preceded = " ".join(lines[max(0, i - 2):i])
                    if CTOR_RE.search(preceded):
                        continue
                    defs.append((name, path, i + 1))
    return defs


def collect_references(roots):
    text = {}
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for fn in filenames:
                if fn.endswith((".c", ".h")):
                    path = os.path.join(dirpath, fn)
                    text[path] = open(path, encoding="utf-8",
                                      errors="replace").read()
    return text


def references(name, text):
    """Lines outside the function's own prototype that mention `name`."""
    pats = [re.compile(r'\b' + re.escape(name) + r'\b')]
    if name.startswith("test_"):
        # TEST(foo) expands to test_foo.
        pats.append(re.compile(
            r'\bTEST\s*\(\s*' + re.escape(name[5:]) + r'\s*\)'))
    out = []
    for path, body in text.items():
        for i, ln in enumerate(body.splitlines(), 1):
            if not any(p.search(ln) for p in pats):
                continue
            if path.endswith(".h"):
                stripped = ln.strip()
                if stripped.startswith("#define"):
                    pass            # macro alias is a real reference
                elif stripped.endswith(";"):
                    continue        # plain prototype
            out.append((path, i))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--roots", nargs="+", default=["src", "tests"])
    args = ap.parse_args()

    defs = collect_definitions(args.roots)
    text = collect_references(args.roots)

    dead = []
    for name, path, line in defs:
        if name == "main":
            continue
        refs = references(name, text)
        # Drop the definition site itself.
        refs = [r for r in refs if not (r[0] == path and r[1] == line)]
        if not refs:
            dead.append((name, path, line))

    if not dead:
        print("dead-code check: no defined-but-unreferenced functions")
        return 0

    print("dead-code check: %d function(s) defined but never used:" % len(dead))
    for name, path, line in sorted(dead):
        print("  %-48s %s:%d" % (name, path, line))
    print("\nDelete them, or add a real reference.")
    return 1


if __name__ == "__main__":
    sys.exit(main())

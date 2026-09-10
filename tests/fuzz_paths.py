#!/usr/bin/env python3
"""Stress harness for ccode's file-path validators.

Pipes paths to `tests/test_agent --path-probe`, which reports
is_workspace_relative_path(p,0), is_workspace_relative_path(p,1),
is_home_relative_path(p) and contains_home_path(p). Compares against a Python
reference model, then flags security-relevant acceptances (Windows-style
separators / drive letters on a POSIX validator) and omissions ($HOME).

Usage:
  tests/fuzz_paths.py [--probe PATH] [--count N] [--seed S] [--show K]
Exit codes: 0 (report produced), 2 (harness failure).
"""

import argparse
import random
import struct
import subprocess
import sys

CURATED = [
    "src/main.c", "a", "a/b/c", "a.b", "a-b_c", ".hidden", "file with spaces",
    "dir/.hidden", "a~b", "a\\b", "C:\\Windows\\System32", "a:b",
    "src/", "src//x", "src/./x", "src/../x", "../x", "..", ".",
    "/etc/passwd", "~", "~/x", "~user/x", "-flag", "/", "", "//", "a/",
    "a\\..\\..\\etc\\passwd", "..\\..\\secret", "con", "NUL",
    "a b/c d.txt", "日本語/ファイル.txt", "a/\x00b",
    "C:/Windows/System32", "\\\\server\\share\\x", "aux",
    "$HOME/.ssh/id_rsa", "${HOME}/.config/x", "foo $HOME/bar", "$HOME",
]


def ref_ws(path, allow_dot):
    if path is None or path == "" or len(path) >= 4096:
        return 0
    if path[0] in ("/", "-", "~"):
        return 0
    # Backslash and drive/ADS colons are Windows separators: rejected.
    if "\\" in path or ":" in path:
        return 0
    if allow_dot and path == ".":
        return 1
    for comp in path.split("/"):
        if comp == "" or comp == "." or comp == "..":
            return 0
    return 1


def ref_home(path):
    if not path or path[0] != "~":
        return 0
    return 1 if len(path) == 1 or path[1] in ("/", "\\") else 0


def ref_contains(text):
    i = 0
    while True:
        i = text.find("~/", i)
        if i < 0:
            return 0
        before = " " if i == 0 else text[i - 1]
        if before in " =:(,":
            return 1
        i += 2


def frame(path):
    pb = path.encode("utf-8", "surrogatepass")
    # strip NUL: the C string cannot carry it
    pb = pb.replace(b"\x00", b"")
    return struct.pack("<I", len(pb)) + pb


def run_probe(probe, paths, timeout):
    blob = b"".join(frame(p) for p in paths)
    try:
        proc = subprocess.Popen([probe, "--path-probe"],
                                stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        out, err = proc.communicate(blob, timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate()
        return None, "timeout", ""
    except OSError as exc:
        return None, "spawn failed: %s" % exc, ""
    return out.decode("utf-8", "replace").splitlines(), proc.returncode, \
        err.decode("utf-8", "replace")


CHARS = list("abcXYZ019._-~ /\\:")


def rand_path(rnd):
    return "".join(rnd.choice(CHARS) for _ in range(rnd.randint(0, 24)))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--probe", default="./tests/test_agent")
    ap.add_argument("--count", type=int, default=4000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--show", type=int, default=40)
    ap.add_argument("--timeout", type=float, default=60.0)
    args = ap.parse_args()

    rnd = random.Random(args.seed)
    paths = list(CURATED)
    while len(paths) < args.count:
        paths.append(rand_path(rnd))

    lines, rc, err = run_probe(args.probe, paths, args.timeout)
    if lines is None:
        print("FAIL: %s" % rc)
        return 2
    if rc < 0:
        print("FAIL: probe died with signal %d after %d/%d"
              % (-rc, len(lines), len(paths)))
        return 1

    mismatches = []
    windows_risk = []
    home_omissions = []
    for i, path in enumerate(paths):
        if i >= len(lines):
            print("FAIL: no output for case %d" % i)
            return 1
        ws0, ws1, home, contains = (int(x) for x in lines[i].split("\t"))
        if ws0 != ref_ws(path, 0) or ws1 != ref_ws(path, 1):
            mismatches.append((path, "ws", (ws0, ws1), (ref_ws(path, 0), ref_ws(path, 1))))
        if home != ref_home(path):
            mismatches.append((path, "home", home, ref_home(path)))
        # accepted by the workspace validator, yet contains a Windows separator
        # or drive colon that Win32 would treat as a path escape
        if ws0 == 1 and ("\\" in path or (len(path) > 1 and path[1] == ":")):
            windows_risk.append(path)
        # $HOME / ${HOME} followed by a separator are home paths that
        # contains_home_path should catch; bare "$HOME" is not a path.
        if ("$HOME/" in path or "${HOME}/" in path) and not contains:
            home_omissions.append(path)

    if err.strip():
        print("(probe stderr)\n%s" % err.strip())

    print("cases=%d  validator-mismatches=%d  windows-sep-accepted=%d  "
          "contains_home-omissions=%d"
          % (len(paths), len(mismatches), len(windows_risk), len(home_omissions)))

    if mismatches:
        print("\n== VALIDATOR MISMATCHES vs reference ==")
        for path, what, got, want in mismatches[:args.show]:
            print("  %-8s %r got=%s want=%s" % (what, path, got, want))

    if windows_risk:
        print("\n== ACCEPTED but contains \\ or drive ':' (Windows separator risk) ==")
        for path in windows_risk[:args.show]:
            print("  %r" % path)

    if home_omissions:
        print("\n== contains_home_path() misses $HOME / ${HOME} ==")
        for path in home_omissions[:args.show]:
            print("  %r" % path)

    if not mismatches and not windows_risk and not home_omissions:
        print("clean")
        return 0
    print("FAIL: %d validator mismatch(es), %d windows-sep, %d home omission(s)"
          % (len(mismatches), len(windows_risk), len(home_omissions)))
    return 1


if __name__ == "__main__":
    sys.exit(main())

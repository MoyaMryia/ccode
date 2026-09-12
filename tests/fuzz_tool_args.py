#!/usr/bin/env python3
"""Randomized fuzzer for ccode's tool-call argument parser.

Pipes framed (tool, arguments) records to `tests/test_agent --fuzz-probe`, which
calls prepare_tool() in-process and prints exactly one line per case: "OK" or a
structured {"error": ...}. It generates four tiers:

  1. valid     -- schema-valid payloads for every tool (must be accepted)
  2. wrapped   -- the same payloads wrapped in 0..N {"arguments": ...} envelopes
                  in object and JSON-string form, mixed, including past the
                  unwrap cap (must be accepted, or "nested too deep" past cap)
  3. invalid   -- modeled mutations known to be wrong: dropped / duplicated /
                  renamed / extra keys, wrong value types, truncation, trailing
                  data, a second root object, BOM, surrogates, NUL, range checks
                  (must return a structured error)
  4. chaos     -- random byte soup and random byte-level mutations of a valid
                  skeleton (only must not crash; output must be OK or a
                  structured error)

Usage:
  tests/fuzz_tool_args.py [--probe PATH] [--count N] [--seed S] [--show K]
                          [--cap N] [--timeout SEC]

Exit codes: 0 all cases behaved, 1 mismatch / crash, 2 usage or harness failure.
"""

import argparse
import copy
import json
import random
import struct
import subprocess
import sys

# Must match CCODE_MAX_TOOL_ARG_WRAP in src/agent/agent_prepare.c.
DEFAULT_CAP = 8

# ── schemas ────────────────────────────────────────────────────────────────
# Every value here is accepted by prepare_tool() today (it validates shape and
# safety, not filesystem existence). Paths stay workspace-relative and argv
# avoids shell-string form so the "valid" tier stays valid.
BASE = {
    "read_file":   [{"file_path": "src/main.c"}, {"file_path": "a"}],
    "write_file":  [{"file_path": "out.txt", "content": "hello"},
                    {"file_path": "a", "content": ""}],
    "edit_file":   [{"file_path": "f.c", "old_string": "a", "new_string": "b"}],
    "bash":        [{"command": "echo hi"}, {"command": "printf x"}],
    "delete_file": [{"file_path": "obsolete.txt"}],
    "move_file":   [{"source": "a.txt", "destination": "b.txt"}],
    "glob":        [{"pattern": "*.c"}, {"pattern": "x", "path": "src"},
                    {"pattern": "x", "regex": True}],
    "grep":        [{"pattern": "needle"}, {"pattern": "n", "include": "*.c"},
                    {"pattern": "n", "path": "src", "context": 3},
                    {"pattern": "n", "regex": 1}],
    "task": [{"action": "create", "content": "do a thing"},
             {"action": "update", "id": "1", "status": "in_progress"},
             {"action": "list"}],
    "web_search":  [{"query": "openai"}],
    "web_fetch":   [{"url": "http://127.0.0.1/"},
                    {"url": "https://example.com/x", "method": "GET",
                     "timeout": 5}],
    "agent_tool":  [{"task": "inspect the parser"},
                    {"task": "x", "read_only": "false"}],
    "bash": [{"command": "echo hi"},
                    {"argv": ["ls", "-l"], "timeout_ms": 1000}],
}

# Required keys per tool; mutate these for reliably-invalid cases. task_list
# ignores argument keys entirely, so it is excluded from key mutations.
REQUIRED = {
    "read_file": ["file_path"], "write_file": ["file_path", "content"],
    "edit_file": ["file_path", "old_string", "new_string"],
    "bash": ["command"], "delete_file": ["file_path"],
    "move_file": ["source", "destination"], "glob": ["pattern"],
    "grep": ["pattern"], "task": ["action"],
    "web_search": ["query"], "web_fetch": ["url"], "agent_tool": ["task"],
    "bash": ["command"],
}


class Case:
    __slots__ = ("tool", "args", "expect", "note")

    def __init__(self, tool, args, expect, note=""):
        self.tool = tool
        self.args = args          # str or bytes
        self.expect = expect      # ("ok",) | ("error",) | ("contains", s) | ("any",)
        self.note = note


def to_bytes(a):
    return a if isinstance(a, bytes) else a.encode("utf-8")


def frame(tool, args):
    tb = tool.encode("utf-8")
    ab = to_bytes(args)
    return (struct.pack("<I", len(tb)) + tb +
            struct.pack("<I", len(ab)) + ab)


def is_error_json(line):
    if line == "OK":
        return False
    try:
        obj = json.loads(line)
    except ValueError:
        return False
    return isinstance(obj, dict) and "error" in obj


def check(case, got):
    e = case.expect
    if e[0] == "ok":
        return got == "OK"
    if e[0] == "any":
        return got == "OK" or is_error_json(got)
    if e[0] == "error":
        return is_error_json(got)
    if e[0] == "contains":
        return e[1] in got
    return False


# ── envelope helpers ───────────────────────────────────────────────────────
def wrap(args, depth, rnd):
    s = args
    for _ in range(depth):
        if rnd.random() < 0.5:
            s = '{"arguments":' + s + '}'
        else:
            s = '{"arguments":' + json.dumps(s, ensure_ascii=False) + '}'
    return s


def dump(d, rnd):
    seps = rnd.choice([(",", ":"), (", ", ": "), (",", " : "),
                       (" , ", " : ")])
    if rnd.random() < 0.5:
        return json.dumps(d, ensure_ascii=False, separators=seps)
    return json.dumps(d, ensure_ascii=False, separators=seps,
                      indent=rnd.choice([1, 2, 4, None]))


def wrong_value(rnd, orig):
    if isinstance(orig, bool):
        return rnd.choice([1, "true", None, []])
    if isinstance(orig, int):
        return rnd.choice(["1", None, True, {}])
    if isinstance(orig, str):
        return rnd.choice([123, None, True, [], {}, ["x"]])
    if isinstance(orig, list):
        return rnd.choice(["x", 1, None, {}])
    if isinstance(orig, dict):
        return rnd.choice(["x", 1, None, []])
    return "x"


def escaped_key(k):
    return "".join("\\u%04x" % ord(c) for c in k)


def dup_key(d, key):
    s = json.dumps(d, ensure_ascii=False)
    return (s[:-1] + "," + json.dumps(key, ensure_ascii=False) + ":" +
            json.dumps(d[key], ensure_ascii=False) + "}")


# ── tiers ──────────────────────────────────────────────────────────────────
def gen_valid(rnd):
    out = []
    for tool, bases in BASE.items():
        for d in bases:
            out.append(Case(tool, dump(d, rnd), ("ok",), "valid"))
    return out


def gen_wrapped(rnd, cap):
    out = []
    for tool, bases in BASE.items():
        base = json.dumps(rnd.choice(bases), ensure_ascii=False)
        for depth in range(1, cap + 1):
            out.append(Case(tool, wrap(base, depth, rnd), ("ok",),
                            "wrap-depth-%d" % depth))
        for depth in range(cap + 1, cap + 4):
            out.append(Case(tool, wrap(base, depth, rnd),
                            ("contains", "nested too deep"),
                            "wrap-depth-%d" % depth))
    return out


def gen_invalid(rnd):
    out = []

    def add(tool, args, note):
        out.append(Case(tool, args, ("error",), note))

    for tool, bases in BASE.items():
        base = copy.deepcopy(rnd.choice(bases))
        base_s = json.dumps(base, ensure_ascii=False)
        reqs = REQUIRED[tool]

        for req in reqs:
            if req not in base:
                continue
            d = copy.deepcopy(base)
            d.pop(req)
            add(tool, json.dumps(d), "drop-" + req)

            d = copy.deepcopy(base)
            d[req] = wrong_value(rnd, base[req])
            add(tool, json.dumps(d), "wrong-type-" + req)

            d = copy.deepcopy(base)
            d[escaped_key(req)] = d.pop(req)
            add(tool, json.dumps(d), "escaped-key-" + req)

        if tool != "task":
            d = copy.deepcopy(base)
            d["__extra__"] = 1
            add(tool, json.dumps(d), "extra-key")
            key = next(iter(base)) if base else None
            if key is not None:
                add(tool, dup_key(base, key), "duplicate-key")

        if len(base_s) > 2:
            cut = rnd.randint(1, len(base_s) - 1)
            add(tool, base_s[:cut], "truncated")
        add(tool, base_s + "x", "trailing-byte")
        add(tool, base_s + base_s, "second-root")
        add(tool, "\ufeff" + base_s, "bom")
        add(tool, "[]", "root-array")
        add(tool, "123", "root-number")
        add(tool, '"s"', "root-string")

        # trailing data after an envelope: the outer strictness gap
        if base_s:
            w = '{"arguments":' + base_s + '}'
            add(tool, w + "x", "envelope-trailing")
            add(tool, w + w, "envelope-second-root")
            add(tool, w + "}", "envelope-extra-brace")

    out.append(Case("bash",
                    '{"command":"true","timeout_ms":0}', ("error",),
                    "timeout-0"))
    out.append(Case("bash",
                    '{"command":"true","timeout_ms":-1}', ("error",),
                    "timeout-neg"))
    out.append(Case("bash",
                    '{"command":"true","timeout_ms":300001}', ("error",),
                    "timeout-huge"))
    out.append(Case("bash",
                    '{"command":"true","timeout_ms":"1"}', ("error",),
                    "timeout-string"))
    out.append(Case("bash",
                    '{"command":"true","timeout_ms":1.5}', ("error",),
                    "timeout-float"))
    out.append(Case("bash", '{"command":"sh -c true"}',
                    ("contains", "error"),
                    "bash-tool-still-executes"))
    out.append(Case("grep", '{"pattern":"n","context":101}', ("error",),
                    "context-101"))
    out.append(Case("grep", '{"pattern":"n","context":-1}', ("error",),
                    "context-neg"))
    return out


CHAOS_BYTES = bytes(range(1, 256))


def byte_mutate(rnd, data):
    b = bytearray(data)
    for _ in range(rnd.randint(1, 8)):
        if not b:
            break
        op = rnd.choice("idrs")
        pos = rnd.randrange(len(b))
        if op == "i":
            b.insert(pos, rnd.randrange(1, 256))
        elif op == "d":
            del b[pos]
        elif op == "r":
            b[pos] = rnd.randrange(1, 256)
        else:
            b[pos:pos] = bytes(rnd.randrange(1, 256)
                               for _ in range(rnd.randint(1, 4)))
    return bytes(b)


def gen_chaos(rnd, n):
    out = []
    tools = list(BASE.keys())
    for _ in range(n):
        tool = rnd.choice(tools)
        r = rnd.random()
        if r < 0.4:
            base = json.dumps(rnd.choice(BASE[tool]), ensure_ascii=False)
            out.append(Case(tool, byte_mutate(rnd, base.encode("utf-8")),
                            ("any",), "byte-mutate"))
        elif r < 0.7:
            out.append(Case(tool, bytes(rnd.choice(CHAOS_BYTES)
                                        for _ in range(rnd.randint(0, 400))),
                            ("any",), "soup"))
        else:
            base = json.dumps(rnd.choice(BASE[tool]), ensure_ascii=False)
            out.append(Case(tool, wrap(base, rnd.randint(0, 12), rnd),
                            ("any",), "wrap-chaos"))
    return out


# ── driver ─────────────────────────────────────────────────────────────────
def run_probe(probe, cases, timeout):
    blob = b"".join(frame(c.tool, c.args) for c in cases)
    try:
        proc = subprocess.Popen([probe, "--fuzz-probe"],
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


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--probe", default="./tests/test_agent",
                    help="path to the --fuzz-probe capable binary")
    ap.add_argument("--count", type=int, default=4000,
                    help="minimum total cases (chaos pads up to this)")
    ap.add_argument("--seed", type=int, default=1, help="PRNG seed")
    ap.add_argument("--show", type=int, default=30,
                    help="max mismatches to print")
    ap.add_argument("--cap", type=int, default=DEFAULT_CAP,
                    help="envelope depth cap (must match the C build)")
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="probe wall-clock timeout (seconds)")
    args = ap.parse_args()

    rnd = random.Random(args.seed)

    modeled = []
    modeled += gen_valid(rnd)
    modeled += gen_wrapped(rnd, args.cap)
    modeled += gen_invalid(rnd)

    chaos_n = max(0, args.count - len(modeled))
    cases = modeled + gen_chaos(rnd, chaos_n)

    lines, rc, err = run_probe(args.probe, cases, args.timeout)
    if lines is None and rc == "timeout":
        print("FAIL: probe timed out after %ss" % args.timeout)
        return 1
    if lines is None:
        print("FAIL: %s" % rc)
        return 2
    if rc < 0:
        idx = len(lines)
        print("FAIL: probe died with signal %d after %d/%d cases"
              % (-rc, idx, len(cases)))
        if idx < len(cases):
            c = cases[idx]
            print("  next case: tool=%r note=%s args=%r"
                  % (c.tool, c.note, c.args))
        return 1

    mismatches = []
    malformed = []
    for i, case in enumerate(cases):
        if i >= len(lines):
            mismatches.append((i, case, "NO OUTPUT (crash?)", None))
            break
        got = lines[i]
        if got != "OK" and not is_error_json(got):
            malformed.append((i, case, got))
        if not check(case, got):
            mismatches.append((i, case, case.expect, got))

    if err.strip():
        print("(probe stderr)\n%s" % err.strip())

    print("cases=%d  ok=%d  mismatches=%d  malformed-output=%d  rc=%d"
          % (len(cases), len(cases) - len(mismatches), len(mismatches),
             len(malformed), rc))

    if mismatches:
        by_note = {}
        for _, c, _, _ in mismatches:
            by_note[c.note] = by_note.get(c.note, 0) + 1
        print("top mismatch kinds:")
        for note, n in sorted(by_note.items(), key=lambda kv: -kv[1])[:12]:
            print("  %-24s %d" % (note, n))
        print("first %d mismatches:" % min(args.show, len(mismatches)))
        for i, case, expect, got in mismatches[:args.show]:
            print("  #%d [%s] %s" % (i, case.tool, case.note))
            print("    args: %r" % (case.args,))
            print("    want: %s" % (expect,))
            print("    got : %r" % (got,))

    for i, case, got in malformed[:args.show]:
        print("  malformed output #%d [%s] %s: %r"
              % (i, case.tool, case.note, got))

    if not mismatches and not malformed:
        print("PASS: all %d cases behaved as modeled" % len(cases))
        return 0
    print("seed=%d (rerun: %s --seed %d --probe %s)"
          % (args.seed, sys.argv[0], args.seed, args.probe))
    return 1


if __name__ == "__main__":
    sys.exit(main())

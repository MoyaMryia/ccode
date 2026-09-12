#!/usr/bin/env python3
"""Stress 2: JSON Lines protocol fuzz against the real ccode-cli backend.

Feeds the --json backend a large stream of hostile stdin: malformed JSON,
random bytes, NULs, megabyte-sized lines (the backend reads fixed 8 KiB
chunks, so oversized events get chopped mid-JSON), deeply nested objects,
unicode line noise, valid events with hostile text fields, interspersed
with real hello/input/command/clear events answered by the mock provider.

Pass criteria: the process never crashes (no signal exit), never hangs,
every stdout line is a valid JSON object with a "type" field, the hello
produces a ready event, the real inputs produce message events, and EOF
yields a clean exit 0.

Usage: python3 tests/stress_jsonlines_protocol.py [--seed N]
"""

import argparse
import json
import os
import random
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stress_common import CCODE, MockProvider, ok


def fail(message):
    print("  FAIL: %s" % message)
    sys.exit(1)


def garbage_line(rng):
    kind = rng.random()
    if kind < 0.14:
        # Random bytes, often invalid UTF-8, sometimes with NULs.
        n = rng.randint(1, 400)
        return bytes(rng.getrandbits(8) for _ in range(n))
    if kind < 0.26:
        # Oversized line: the 8 KiB fgets buffer must chop it without
        # losing protocol sync.
        return (b'{"type":"input","text":"' + b'A' * rng.choice(
            [8192, 30000, 300000, 1000000]) + b'"}')
    if kind < 0.36:
        # Valid JSON, wrong shape or unknown type.
        return rng.choice([
            b'{"type":"frobnicate"}',
            b'{"type":"input"}',
            b'{"type":"input","text":42}',
            b'{"type":"hello","workspace":null}',
            b'[]', b'"scalar"', b'123', b'null',
        ])
    if kind < 0.46:
        # Deeply nested (jsmn/parser stress).
        depth = rng.randint(10, 400)
        return (b'{"type":"x","v":' + b'{"a":' * depth + b'1' + b'}' * depth)
    if kind < 0.56:
        # Cut mid-event: plausible prefix, no closing brace.
        return b'{"type":"input","text":"unterminated'
    if kind < 0.66:
        # Duplicate keys and type confusion.
        return rng.choice([
            b'{"type":"input","text":"a","text":"b","type":"command"}',
            b'{"type":"clear","type":"resize","type":"hello"}',
            b'{"type":"command","text":""}',
        ])
    if kind < 0.76:
        # Unicode line noise and bidi controls.
        s = "".join(rng.choice(["‮", "陈", "🙂", "\ud83d", "­", "\x1b[31m"])
                    for _ in range(rng.randint(1, 60)))
        return s.encode("utf-8", errors="ignore")
    if kind < 0.86:
        # Event-looking prefix with garbage tail (strstr-based dispatch bait).
        return (b'{"type":"input","text":"ok"}' +
                bytes(rng.getrandbits(8) for _ in range(rng.randint(1, 100))))
    # Whitespace / empty / newline noise.
    return rng.choice([b"", b"   ", b"\t\t", b"\r"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=20260912)
    ap.add_argument("--lines", type=int, default=4000)
    args = ap.parse_args()
    rng = random.Random(args.seed)

    print("=== ccode stress: JSON Lines protocol fuzz (seed=%d, lines=%d) ==="
          % (args.seed, args.lines))
    mock = MockProvider()
    env = os.environ.copy()
    env.update({
        "CCODE_API_BASE": "http://127.0.0.1:%d/v1" % mock.port,
        "CCODE_API_KEY": "stress-key",
        "CCODE_MODEL": "stress-model",
        "CCODE_REQUEST_TIMEOUT": "30",
        "CCODE_CONTEXT_TOKENS": "0",
        "CCODE_SESSION_DIR": "/tmp/ccode_stress_sessions_%d" % os.getpid(),
    })

    payload = [b'{"type":"hello","workspace":"."}']
    real_inputs = 0
    for i in range(args.lines):
        if i == args.lines // 3 or i == 2 * args.lines // 3:
            payload.append(b'{"type":"input","text":"hello real turn"}')
            real_inputs += 1
        elif i % 500 == 499:
            payload.append(b'{"type":"command","text":"/history"}')
            payload.append(b'{"type":"clear"}')
        else:
            payload.append(garbage_line(rng))
    payload.append(b'{"type":"input","text":"final real turn"}')
    body = b"\n".join(payload)  # note: no trailing newline -> EOF mid-"line"

    stdout_lines = []
    proc = subprocess.Popen(
        [CCODE, "--json", "-p", "unused"],
        env=env, stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def reader():
        for raw in proc.stdout:
            stdout_lines.append(raw)

    t = threading.Thread(target=reader)
    t.start()
    start = time.time()
    try:
        # Feed in bursts so the pipe backpressure interleaves with the
        # backend's processing instead of front-loading the pipe buffer.
        CH = 64 * 1024
        for off in range(0, len(body), CH):
            proc.stdin.write(body[off:off + CH])
            proc.stdin.flush()
            time.sleep(0.002)
        proc.stdin.close()
        rc = proc.wait(timeout=120)
    except subprocess.TimeoutExpired:
        proc.kill()
        fail("backend hung (killed after 120s)")
    t.join(timeout=5)
    stderr = proc.stderr.read().decode("utf-8", errors="replace")
    elapsed = time.time() - start
    mock.stop()

    if rc < 0:
        fail("backend died on signal %d" % -rc)
    if rc != 0:
        fail("backend exited %d\nstderr tail: %s" % (rc, stderr[-500:]))
    if len(stdout_lines) == 0:
        fail("no stdout events at all")

    events = {}
    for raw in stdout_lines:
        try:
            obj = json.loads(raw.decode("utf-8", errors="replace"))
        except ValueError:
            fail("stdout line is not valid JSON: %r" % raw[:200])
        if not isinstance(obj, dict) or "type" not in obj:
            fail("stdout event lacks type field: %r" % raw[:200])
        events[obj["type"]] = events.get(obj["type"], 0) + 1

    # strstr-based dispatch matches "type":"hello" substrings inside other
    # (often chopped) events too, so a fuzz stream yields several readies.
    if events.get("ready", 0) < 1:
        fail("expected at least one ready event, got 0")
    if events.get("message", 0) < real_inputs:
        fail("expected >= %d message events (real turns), got %d"
             % (real_inputs, events.get("message", 0)))
    if events.get("error", 0) < 100:
        fail("garbage should have produced many error events, got %d"
             % events.get("error", 0))

    print("  %d stdin events -> %d stdout events in %.1fs: %s"
          % (args.lines + 3, len(stdout_lines), elapsed,
             dict(sorted(events.items(), key=lambda kv: -kv[1]))))
    ok("json lines protocol fuzz")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Regression tests for incremental ccode-cli output."""

import json
import os
import select
import subprocess
import sys
import time


CCODE = os.path.join(os.path.dirname(__file__), "..", "ccode-cli")
MOCK_PROVIDER = os.path.join(os.path.dirname(__file__), "mock_provider.py")
PORT = 9896
TIMEOUT = 8


def environment():
    env = os.environ.copy()
    env["CCODE_API_BASE"] = "http://127.0.0.1:%d/v1" % PORT
    env["CCODE_API_KEY"] = "test-key"
    env["CCODE_MODEL"] = "test-model"
    env["CCODE_REQUEST_TIMEOUT"] = "5"
    return env


def read_until(stream, needle, timeout):
    data = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ready, _, _ = select.select([stream], [], [], deadline - time.monotonic())
        if not ready:
            break
        chunk = os.read(stream.fileno(), 4096)
        if not chunk:
            break
        data += chunk
        if needle in data:
            return data
    return data


def test_plain_streams_before_completion():
    proc = subprocess.Popen(
        [CCODE, "--prompt", "__ccode_test_stream-delayed"],
        env=environment(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    started = time.monotonic()
    first = read_until(proc.stdout, b"first", 0.7)
    elapsed = time.monotonic() - started
    out, err = proc.communicate(timeout=TIMEOUT)
    output = first + out
    return (b"first" in first and elapsed < 0.9 and b" second" in output and
            proc.returncode == 0), output + err


def test_json_protocol_streams_deltas():
    proc = subprocess.Popen(
        [CCODE, "--json"], env=environment(), stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    hello = json.dumps({"type": "hello", "model": "test-model",
                        "workspace": "."}, separators=(",", ":")) + "\n"
    prompt = json.dumps({"type": "input",
                         "text": "__ccode_test_stream-delayed"},
                        separators=(",", ":")) + "\n"
    proc.stdin.write((hello + prompt).encode("utf-8"))
    proc.stdin.flush()
    started = time.monotonic()
    first = read_until(proc.stdout, b'"text":"first"', 0.7)
    elapsed = time.monotonic() - started
    proc.stdin.close()
    proc.wait(timeout=TIMEOUT)
    rest = proc.stdout.read()
    err = proc.stderr.read()
    output = first + rest
    return (b'"type":"message_delta"' in first and elapsed < 0.9 and
            b'"text":" second"' in output and proc.returncode == 0), output + err


def main():
    mock = subprocess.Popen([sys.executable, MOCK_PROVIDER, str(PORT)],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    time.sleep(0.3)
    failed = 0
    try:
        for name, test in (("plain CLI", test_plain_streams_before_completion),
                           ("JSON protocol", test_json_protocol_streams_deltas)):
            ok, output = test()
            if ok:
                print("  PASS: %s streams incrementally" % name)
            else:
                print("  FAIL: %s did not stream incrementally: %r" %
                      (name, output[:400]))
                failed += 1
    finally:
        mock.terminate()
        try:
            mock.wait(timeout=2)
        except subprocess.TimeoutExpired:
            mock.kill()
            mock.wait()
    return failed


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Regression tests for ccode-cli output (streaming and JSON protocol)."""

import json
import os
import select
import shutil
import subprocess
import sys
import tempfile
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


def test_reasoning_keeps_real_newlines():
    """Streamed reasoning_content must render real newlines/tabs, not the
    escaped literal \\n that the single-line safe printer produces."""
    proc = subprocess.Popen(
        [CCODE, "--prompt", "__ccode_test_reasoning-newlines"],
        env=environment(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out, err = proc.communicate(timeout=TIMEOUT)
    return (b"think line one\nthink\tline two" in out and
            b"think line one\\nthink" not in out and
            b"final answer" in out and
            proc.returncode == 0), out + err


def test_json_session_list_is_human_readable():
    """`/resume --list` (and /sessions) must render as text, not the raw
    {"sessions":[...]} payload, which the fork-based TUI would show verbatim."""
    session_dir = tempfile.mkdtemp(prefix="ccode_json_sessions_")
    session_file = os.path.join(session_dir, "demo.json")
    with open(session_file, "w") as f:
        f.write('{"version":3,"messages":[{"role":"user","content":"hi"}]}')
    os.chmod(session_file, 0o600)
    env = environment()
    env["CCODE_SESSION_DIR"] = session_dir
    proc = subprocess.Popen(
        [CCODE, "--json"], env=env, stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    command = json.dumps({"type": "command", "text": "/resume --list"},
                         separators=(",", ":")) + "\n"
    out, err = proc.communicate(input=command.encode("utf-8"),
                                timeout=TIMEOUT)
    shutil.rmtree(session_dir, ignore_errors=True)
    text = ""
    for line in out.decode("utf-8", "replace").splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if event.get("type") == "message":
            text = event.get("text", "")
    return (text.startswith("Sessions:") and "demo.json" in text and
            "1 msgs)" in text and '{"sessions"' not in text and
            proc.returncode == 0), out + err


def test_max_turns_stops_loop():
    """--max-turns N caps the tool loop against a provider that never stops on
    its own: exactly N commands run, then a [turn limit] notice on stderr."""
    proc = subprocess.Popen(
        [CCODE, "--write", "--auto-approve", "--max-turns", "2",
         "--prompt", "__ccode_test_turnlimit-fixture"],
        env=environment(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out, err = proc.communicate(timeout=TIMEOUT)
    combined = out + err
    return (proc.returncode == 0 and
            b"turnlimit-1" in combined and b"turnlimit-2" in combined and
            b"turnlimit-3" not in combined and
            b"[turn limit]" in combined), combined


def main():
    mock = subprocess.Popen([sys.executable, MOCK_PROVIDER, str(PORT)],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    time.sleep(0.3)
    failed = 0
    try:
        for name, test in (("plain CLI", test_plain_streams_before_completion),
                           ("JSON protocol", test_json_protocol_streams_deltas),
                           ("reasoning newlines",
                            test_reasoning_keeps_real_newlines),
                           ("JSON session list",
                            test_json_session_list_is_human_readable),
                           ("max-turns cap", test_max_turns_stops_loop)):
            ok, output = test()
            if ok:
                print("  PASS: %s" % name)
            else:
                print("  FAIL: %s: %r" % (name, output[:400]))
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

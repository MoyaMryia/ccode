#!/usr/bin/env python3
"""TTY-backed provider-driven integration test for tool approval.

Starts the mock provider, runs ccode with a PTY terminal, interacts to approve
or deny tool requests, and verifies correct behavior in the multi-turn loop."""

import json
import os
import pty
import select
import signal
import subprocess
import sys
import time

CCODE = os.path.join(os.path.dirname(__file__), "..", "ccode-cli")
MOCK_PROVIDER = os.path.join(os.path.dirname(__file__), "mock_provider.py")
PORT = 9898
TIMEOUT = 15

TEST_FIXTURE_DIR = None


def setup_fixture():
    global TEST_FIXTURE_DIR
    TEST_FIXTURE_DIR = os.path.join(
        os.path.dirname(__file__), "fixtures", "tty_fixture_%d" % os.getpid())
    os.makedirs(TEST_FIXTURE_DIR, exist_ok=True)
    # Create a simple source file with a clear defect
    with open(os.path.join(TEST_FIXTURE_DIR, "main.c"), "w") as f:
        f.write("#include <stdio.h>\nint add(int a, int b) { return a - b; }\n")
    return TEST_FIXTURE_DIR


def teardown_fixture():
    global TEST_FIXTURE_DIR
    if TEST_FIXTURE_DIR:
        subprocess.run(["rm", "-rf", TEST_FIXTURE_DIR],
                       capture_output=True)
        TEST_FIXTURE_DIR = None


def start_mock():
    proc = subprocess.Popen(
        [sys.executable, MOCK_PROVIDER, str(PORT)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    if proc.poll() is not None:
        print("FAIL: mock provider failed to start")
        sys.exit(1)
    return proc


def run_ccode_with_tty(prompt, workspace, enable_tools=False,
                       write_mode=False, approve=True, answers=None):
    """Run ccode under a PTY, interact via TTY, capture output.

    When ``answers`` is provided, each bytes line is written once, after a small
    delay between writes, so multi-turn approval/denial sequences work.
    Otherwise a single approval/denial derived from ``approve`` is written."""
    env = os.environ.copy()
    env["CCODE_API_BASE"] = "http://127.0.0.1:%d/v1" % PORT
    env["CCODE_API_KEY"] = "test-key"
    env["CCODE_MODEL"] = "test-model"
    env["CCODE_WORKSPACE"] = workspace
    if write_mode:
        env["CCODE_WRITE_TOOLS"] = "1"
    elif enable_tools:
        env["CCODE_READ_ONLY_TOOLS"] = "1"

    master_fd, slave_fd = pty.openpty()

    proc = subprocess.Popen(
        [CCODE, "--prompt", prompt],
        env=env,
        stdin=slave_fd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE)

    os.close(slave_fd)

    # Write approval/denial to the master end of the PTY
    time.sleep(0.3)
    if answers is None:
        answers = [b"y\n" if approve else b"n\n"]
    for i, ans in enumerate(answers):
        if i > 0:
            time.sleep(0.4)
        os.write(master_fd, ans)

    # Read output with timeout
    start = time.time()
    stdout_data = b""
    stderr_data = b""

    while time.time() - start < TIMEOUT:
        rlist, _, _ = select.select(
            [proc.stdout, proc.stderr, master_fd], [], [], 0.5)
        if proc.stdout in rlist:
            data = os.read(proc.stdout.fileno(), 4096)
            if data:
                stdout_data += data
        if proc.stderr in rlist:
            data = os.read(proc.stderr.fileno(), 4096)
            if data:
                stderr_data += data
        if master_fd in rlist:
            # Drain any PTY input
            try:
                os.read(master_fd, 4096)
            except OSError:
                pass
        if proc.poll() is not None:
            break

    # Drain remaining output
    try:
        out, err = proc.communicate(timeout=2)
        if out:
            stdout_data += out
        if err:
            stderr_data += err
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate()

    os.close(master_fd)
    return stdout_data, stderr_data, proc.returncode


def test_deny_in_noninteractive():
    """Without a TTY, tool calls should be denied by default."""
    env = os.environ.copy()
    env["CCODE_API_BASE"] = "http://127.0.0.1:%d/v1" % PORT
    env["CCODE_API_KEY"] = "test-key"
    env["CCODE_MODEL"] = "test-model"
    # No CCODE_WRITE_TOOLS - tools not enabled
    env["CCODE_WORKSPACE"] = TEST_FIXTURE_DIR

    proc = subprocess.run(
        [CCODE, "--prompt", "__ccode_test_tool-calls"],
        env=env,
        capture_output=True,
        timeout=TIMEOUT)
    output = proc.stdout.decode() + proc.stderr.decode()
    if "tools are not enabled" in output:
        print("  PASS: tool call denied with no tool mode")
        return True
    else:
        print("  FAIL: tool call was not denied (output: %s)" %
              output[:200])
        return False


def test_approve_via_tty():
    """With a TTY and 'y' response, a read-only tool call should be approved."""
    stdout, stderr, rc = run_ccode_with_tty(
        "__ccode_test_tool-calls",
        TEST_FIXTURE_DIR,
        enable_tools=True,
        approve=True)
    output = stdout.decode() + stderr.decode()
    if "[run]" in output and "read_file" in output:
        print("  PASS: tool approved via TTY")
        return True
    else:
        print("  FAIL: tool was not approved (output: %s)" %
              output[:300])
        return False


def test_deny_via_tty():
    """With a TTY and 'n' response, a tool call should be denied."""
    stdout, stderr, rc = run_ccode_with_tty(
        "__ccode_test_tool-calls",
        TEST_FIXTURE_DIR,
        enable_tools=True,
        approve=False)
    output = stdout.decode() + stderr.decode()
    if "[denied]" in output and "(denied)" in output:
        print("  PASS: tool denied via TTY and recorded in session summary")
        return True
    else:
        print("  FAIL: tool was not denied (output: %s)" %
              output[:300])
        return False


def test_approve_write_tool_via_tty():
    """With TTY, --write mode, and 'y', a write tool call should execute."""
    stdout, stderr, rc = run_ccode_with_tty(
        "__ccode_test_tool-calls",
        TEST_FIXTURE_DIR,
        enable_tools=True,
        write_mode=True,
        approve=True)
    output = stdout.decode() + stderr.decode()
    if "[run]" in output:
        print("  PASS: write tool approved via TTY")
        return True
    else:
        print("  FAIL: tool was not approved (output: %s)" %
              output[:300])
        return False


def test_deny_write_and_command_no_side_effects():
    """Deny both a write_file and a run_command; assert no side effects."""
    workspace = os.path.join(
        os.path.dirname(__file__), "fixtures", "tty_deny_%d" % os.getpid())
    os.makedirs(workspace, exist_ok=True)
    write_target = os.path.join(workspace, "must_not_exist.txt")
    cmd_marker = os.path.join(workspace, "must_not_exist_marker.txt")
    if os.path.exists(write_target):
        os.remove(write_target)
    if os.path.exists(cmd_marker):
        os.remove(cmd_marker)

    stdout, stderr, rc = run_ccode_with_tty(
        "__ccode_test_deny-no-side-effects",
        workspace,
        write_mode=True,
        answers=[b"n\n", b"n\n"])
    output = stdout.decode() + stderr.decode()

    ok = True
    deny_count = output.count("[deny]")
    if deny_count < 2:
        print("  FAIL: expected >=2 denials, saw %d (output: %s)" %
              (deny_count, output[:300]))
        ok = False
    if os.path.exists(write_target):
        print("  FAIL: write_file target was created despite denial")
        ok = False
    if os.path.exists(cmd_marker):
        print("  FAIL: run_command marker was created despite denial")
        ok = False
    if ok:
        print("  PASS: denied write and command produced no side effects")
    subprocess.run(["rm", "-rf", workspace], capture_output=True)
    return ok


def main():
    tests_run = 0
    tests_failed = 0

    fixture = setup_fixture()
    mock_proc = start_mock()

    print("=== ccode TTY-backed Agent Integration Tests ===")
    print()

    tests = [
        ("deny noninteractive", test_deny_in_noninteractive),
        ("approve via TTY", test_approve_via_tty),
        ("deny via TTY", test_deny_via_tty),
        ("approve write tool via TTY", test_approve_write_tool_via_tty),
        ("deny write+command no side effects",
         test_deny_write_and_command_no_side_effects),
    ]

    for name, func in tests:
        tests_run += 1
        print("--- %s ---" % name)
        try:
            if func():
                pass
            else:
                tests_failed += 1
        except Exception as e:
            print("  FAIL: %s" % e)
            tests_failed += 1

    # Cleanup
    mock_proc.terminate()
    try:
        mock_proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        mock_proc.kill()
    teardown_fixture()

    print()
    print("=== Results: %d passed, %d failed ===" %
          (tests_run - tests_failed, tests_failed))
    return tests_failed


if __name__ == "__main__":
    sys.exit(main())

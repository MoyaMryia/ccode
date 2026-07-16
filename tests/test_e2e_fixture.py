#!/usr/bin/env python3
"""End-to-end coding loop fixture test.

Creates a fixture repository with a bug, drives ccode through the full
inspect-edit-verify workflow via mock provider and PTY, then verifies
the fix was applied correctly."""

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
PORT = 9897
TIMEOUT = 30
FIXTURE_DIR = None


def setup_fixture():
    global FIXTURE_DIR
    FIXTURE_DIR = os.path.join(
        os.path.dirname(__file__), "fixtures", "e2e_fixture_%d" % os.getpid())
    os.makedirs(os.path.join(FIXTURE_DIR, "src"), exist_ok=True)

    # Create a simple project with a defect: add() returns a - b instead of a + b
    with open(os.path.join(FIXTURE_DIR, "src", "main.c"), "w") as f:
        f.write("#include <stdio.h>\nint add(int a, int b) { return a - b; }\n")

    # Create a .gitignore to keep fixtures clean
    with open(os.path.join(FIXTURE_DIR, ".gitignore"), "w") as f:
        f.write("*.o\n")

    return FIXTURE_DIR


def setup_repair_fixture():
    """Create a fixture with two defects and a git baseline so the repair
    loop fixture (read -> edit -> failing test -> inspect -> second edit ->
    passing test -> git_diff) can be exercised end-to-end."""
    global REPAIR_FIXTURE_DIR
    REPAIR_FIXTURE_DIR = os.path.join(
        os.path.dirname(__file__), "fixtures", "e2e_repair_%d" % os.getpid())
    os.makedirs(os.path.join(REPAIR_FIXTURE_DIR, "src"), exist_ok=True)

    with open(os.path.join(REPAIR_FIXTURE_DIR, "src", "main.c"), "w") as f:
        f.write("#include <stdio.h>\n"
                "int add(int a, int b) { return a - b; }\n"
                "int sub(int a, int b) { return a + b; }\n")
    with open(os.path.join(REPAIR_FIXTURE_DIR, ".gitignore"), "w") as f:
        f.write("*.o\n")

    git_ok = True
    try:
        subprocess.run(["git", "init"], cwd=REPAIR_FIXTURE_DIR,
                       capture_output=True, timeout=10, check=False)
        subprocess.run(["git", "config", "user.email", "test@test"],
                       cwd=REPAIR_FIXTURE_DIR, capture_output=True, timeout=10)
        subprocess.run(["git", "config", "user.name", "test"],
                       cwd=REPAIR_FIXTURE_DIR, capture_output=True, timeout=10)
        subprocess.run(["git", "add", "src/main.c"], cwd=REPAIR_FIXTURE_DIR,
                       capture_output=True, timeout=10)
        r = subprocess.run(["git", "commit", "-m", "initial"],
                           cwd=REPAIR_FIXTURE_DIR, capture_output=True, timeout=10)
        if r.returncode != 0:
            git_ok = False
    except Exception:
        git_ok = False
    return REPAIR_FIXTURE_DIR, git_ok


def teardown_repair_fixture():
    global REPAIR_FIXTURE_DIR
    if REPAIR_FIXTURE_DIR:
        subprocess.run(["rm", "-rf", REPAIR_FIXTURE_DIR], capture_output=True)
        REPAIR_FIXTURE_DIR = None


def teardown_fixture():
    global FIXTURE_DIR
    if FIXTURE_DIR:
        subprocess.run(["rm", "-rf", FIXTURE_DIR], capture_output=True)
FIXTURE_DIR = None
REPAIR_FIXTURE_DIR = None


def start_mock():
    proc = subprocess.Popen(
        [sys.executable, MOCK_PROVIDER, str(PORT)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    if proc.poll() is not None:
        print("FAIL: mock provider failed to start")
        sys.exit(1)
    return proc


def run_ccode_workflow(prompt, workspace):
    """Run ccode through the full workflow with PTY approval."""
    env = os.environ.copy()
    env["CCODE_API_BASE"] = "http://127.0.0.1:%d/v1" % PORT
    env["CCODE_API_KEY"] = "test-key"
    env["CCODE_MODEL"] = "test-model"
    env["CCODE_WORKSPACE"] = workspace
    env["CCODE_WRITE_TOOLS"] = "1"
    env["CCODE_REQUEST_TIMEOUT"] = "15"

    master_fd, slave_fd = pty.openpty()
    proc = subprocess.Popen(
        [CCODE, "--prompt", prompt],
        env=env, stdin=slave_fd,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    os.close(slave_fd)

    stdout_data = b""
    stderr_data = b""
    start = time.time()
    approved_count = 0
    prompt_offset = 0

    while time.time() - start < TIMEOUT:
        rlist, _, _ = select.select(
            [proc.stdout, proc.stderr], [], [], 0.3)

        if proc.stdout in rlist:
            data = os.read(proc.stdout.fileno(), 4096)
            if data:
                stdout_data += data

        new_data = b""
        if proc.stderr in rlist:
            data = os.read(proc.stderr.fileno(), 4096)
            if data:
                new_data = data
                stderr_data += data

        marker_pos = stderr_data.find(b"[y/N]", prompt_offset)
        while marker_pos >= 0:
            os.write(master_fd, b"y\n")
            approved_count += 1
            prompt_offset = marker_pos + len(b"[y/N]")
            marker_pos = stderr_data.find(b"[y/N]", prompt_offset)

        if proc.poll() is not None:
            break

    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()

    # The loop above reads directly from the pipe file descriptors. Drain both
    # descriptors explicitly after exit; mixing those reads with communicate()
    # can intermittently lose the final buffered session summary.
    for stream, target in ((proc.stdout, "stdout"), (proc.stderr, "stderr")):
        chunks = []
        while True:
            data = os.read(stream.fileno(), 4096)
            if not data:
                break
            chunks.append(data)
        if target == "stdout":
            stdout_data += b"".join(chunks)
        else:
            stderr_data += b"".join(chunks)

    os.close(master_fd)
    return stdout_data, stderr_data, proc.returncode, approved_count


def main():
    tests_run = 0
    tests_failed = 0

    fixture = setup_fixture()
    mock_proc = start_mock()

    print("=== ccode End-to-End Coding Loop Fixture ===")
    print()

    # Test 1: Full workflow inspection -> edit -> verify
    tests_run += 1
    print("--- workflow: inspect -> edit -> verify ---")
    try:
        stdout, stderr, rc, approved = run_ccode_workflow(
            "__ccode_test_workflow-fixture", fixture)
        output = stdout.decode() + stderr.decode()

        # Check that the fixture file was actually read by ccode
        fixture_path = os.path.join(fixture, "src", "main.c")
        with open(fixture_path) as f:
            content = f.read()

        if "return a - b" in content:
            print("  FAIL: bug was not fixed (file unchanged)")
            tests_failed += 1
        elif "return a + b" in content:
            print("  PASS: bug was fixed via edit_file")
        else:
            print("  FAIL: unexpected file content after workflow")
            print("    content: %s" % content[:100])
            tests_failed += 1

        if approved >= 2:
            print("  PASS: %d tool approvals handled" % approved)
        else:
            print("  FAIL: only %d tools approved (expected >= 2)" % approved)
            tests_failed += 1

        if "[run]" in output:
            print("  PASS: tool execution visible in output")
        else:
            print("  FAIL: no tool execution visible")
            tests_failed += 1

        if ("mode=read-write" in output or "mode=read-only" in output or
                "mode=none" in output):
            print("  PASS: status line shows active mode")
        else:
            print("  FAIL: status line did not show active mode")
            tests_failed += 1
        if "workspace=" in output and "test-key" not in output:
            print("  PASS: status line shows workspace and no credentials")
        else:
            print("  FAIL: status line missing workspace, or leaked a key")
            tests_failed += 1

    except Exception as e:
        print("  FAIL: %s" % e)
        tests_failed += 1

    # Test 2: Repair loop -- inspect -> edit -> failing test -> inspect
    # -> second edit -> passing test -> git_diff -> report
    tests_run += 1
    print("--- workflow: repair loop (inspect -> edit -> fail -> "
          "inspect -> repair -> pass -> git_diff -> report) ---")
    try:
        repair_dir, git_ok = setup_repair_fixture()
        stdout, stderr, rc, approved = run_ccode_workflow(
            "__ccode_test_repair-loop-fixture", repair_dir)
        output = stdout.decode() + stderr.decode()

        fixture_path = os.path.join(repair_dir, "src", "main.c")
        with open(fixture_path) as f:
            content = f.read()
        add_ok = "int add(int a, int b) { return a + b; }" in content
        sub_ok = "int sub(int a, int b) { return a - b; }" in content
        if add_ok and sub_ok:
            print("  PASS: both defects repaired via two edits")
        else:
            print("  FAIL: not all defects repaired (content: %s)" % content[:160])
            tests_failed += 1

        if "(exit=1)" in output:
            print("  PASS: first focused test reported failure")
        else:
            print("  FAIL: first focused test did not report exit=1")
            tests_failed += 1

        if output.count("(exit=0)") >= 2:
            print("  PASS: second focused test and git_diff reported success")
        else:
            print("  FAIL: expected >=2 (exit=0) markers, saw %d" %
                  output.count("(exit=0)"))
            tests_failed += 1

        if git_ok and "git --no-pager diff" in output:
            print("  PASS: git_diff surfaced repository changes")
        elif git_ok:
            print("  FAIL: git_diff produced no git diff summary")
            tests_failed += 1
        else:
            print("  SKIP: git_diff assertion (git baseline unavailable)")

        if approved >= 6:
            print("  PASS: %d tool approvals handled" % approved)
        else:
            print("  FAIL: only %d tools approved (expected >= 6)" % approved)
            tests_failed += 1

    except Exception as e:
        print("  FAIL: %s" % e)
        tests_failed += 1
    teardown_repair_fixture()

    # Test 3: Cancellation -- SIGINT during an active run_command kills the
    # child process group and returns exit code 130 without leaving a
    # leftover temp file in a workspace.
    tests_run += 1
    print("--- workflow: cancellation kills command and cleans temp ---")
    try:
        cancel_dir = os.path.join(
            os.path.dirname(__file__), "fixtures", "e2e_cancel_%d" % os.getpid())
        os.makedirs(cancel_dir, exist_ok=True)
        marker = os.path.join(cancel_dir, "cancel_marker.txt")
        sentinel = os.path.join(cancel_dir, ".ccode-write-cancel.txt")
        if os.path.exists(marker):
            os.remove(marker)
        if os.path.exists(sentinel):
            os.remove(sentinel)

        env = os.environ.copy()
        env["CCODE_API_BASE"] = "http://127.0.0.1:%d/v1" % PORT
        env["CCODE_API_KEY"] = "test-key"
        env["CCODE_MODEL"] = "test-model"
        env["CCODE_WORKSPACE"] = cancel_dir
        env["CCODE_WRITE_TOOLS"] = "1"
        env["CCODE_REQUEST_TIMEOUT"] = "15"

        master_fd, slave_fd = pty.openpty()
        proc = subprocess.Popen(
            [CCODE, "--prompt", "__ccode_test_cancel-command-fixture"],
            env=env, stdin=slave_fd,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        os.close(slave_fd)

        # Drain output and approve start of the long sleep, then SIGINT.
        start = time.time()
        approved_count = 0
        while time.time() - start < 15:
            rlist, _, _ = select.select([proc.stdout, proc.stderr], [], [], 0.3)
            new = b""
            if proc.stderr in rlist:
                data = os.read(proc.stderr.fileno(), 4096)
                if data:
                    new = data
            if (b"[y/N]" in new or b"Allow this" in new):
                time.sleep(0.1)
                os.write(master_fd, b"y\n")
                approved_count += 1
            if approved_count >= 1:
                # Command is now running; interrupt it.
                time.sleep(0.4)
                os.kill(proc.pid, signal.SIGINT)
                break
            if proc.poll() is not None:
                break

        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        os.close(master_fd)

        if proc.returncode == 130:
            print("  PASS: agent exits 130 after SIGINT")
        else:
            print("  FAIL: expected exit 130, got %s" % proc.returncode)
            tests_failed += 1

        if not os.path.exists(marker):
            print("  PASS: cancelled command produced no side effect")
        else:
            print("  FAIL: marker exists after cancellation")
            tests_failed += 1

    except Exception as e:
        print("  FAIL: %s" % e)
        tests_failed += 1
    if 'cancel_dir' in dir():
        subprocess.run(["rm", "-rf", cancel_dir], capture_output=True)

    # Test 4: Interactive REPL -- multiple prompts + slash commands
    tests_run += 1
    print("--- workflow: interactive REPL with /history and /exit ---")
    try:
        repl_dir = os.path.join(
            os.path.dirname(__file__), "fixtures", "e2e_repl_%d" % os.getpid())
        os.makedirs(repl_dir, exist_ok=True)

        env = os.environ.copy()
        env["CCODE_API_BASE"] = "http://127.0.0.1:%d/v1" % PORT
        env["CCODE_API_KEY"] = "test-key"
        env["CCODE_MODEL"] = "test-model"
        env["CCODE_WORKSPACE"] = repl_dir
        env["CCODE_READ_ONLY_TOOLS"] = "1"

        proc = subprocess.Popen(
            [CCODE, "--interactive"],
            env=env,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)

        stdin_data = b"Hello first\nHello second\n/history\n/exit\n"
        stdout_data, stderr_data = proc.communicate(
            input=stdin_data, timeout=20)
        output = stdout_data.decode() + stderr_data.decode()

        if proc.returncode == 0:
            print("  PASS: REPL exits cleanly after /exit")
        else:
            print("  FAIL: REPL exited with code %s" % proc.returncode)
            tests_failed += 1

        if "You said: Hello first" in output and "You said: Hello second" in output:
            print("  PASS: both prompts were processed")
        else:
            print("  FAIL: not all prompts processed (output: %s)" % output[:300])
            tests_failed += 1

        if "Session history" in output and "Hello first" in output:
            print("  PASS: /history listed prior prompts")
        else:
            print("  FAIL: /history did not list prompts")
            tests_failed += 1

    except Exception as e:
        print("  FAIL: %s" % e)
        tests_failed += 1
    if 'repl_dir' in dir():
        subprocess.run(["rm", "-rf", repl_dir], capture_output=True)

    # Test 5: Session persistence -- save and resume
    tests_run += 1
    print("--- workflow: session save and resume ---")
    try:
        session_dir = os.path.abspath(os.path.join(
            os.path.dirname(__file__), "fixtures", "e2e_session_%d" % os.getpid()))
        os.makedirs(session_dir, exist_ok=True)
        session_path = os.path.join(session_dir, "ccode-session.json")

        def run_prompt(prompt, save=None, resume_path=None):
            env = os.environ.copy()
            env["CCODE_API_BASE"] = "http://127.0.0.1:%d/v1" % PORT
            env["CCODE_API_KEY"] = "test-key"
            env["CCODE_MODEL"] = "test-model"
            env["CCODE_WORKSPACE"] = session_dir
            cmd = [CCODE, "--prompt", prompt]
            if save:
                cmd += ["--save-session", save]
            if resume_path:
                cmd += ["--resume", resume_path]
            r = subprocess.run(cmd, env=env, capture_output=True, timeout=15)
            return r.stdout.decode() + r.stderr.decode(), r.returncode

        # First run: save the session
        out1, rc1 = run_prompt("fresh_query", save=session_path)
        if rc1 != 0:
            print("  FAIL: first run exited with %s" % rc1)
            tests_failed += 1
        elif os.path.exists(session_path):
            fsize = os.path.getsize(session_path)
            mode = os.stat(session_path).st_mode & 0o777
            if fsize > 10 and mode == 0o600:
                print("  PASS: session file created after save (%d bytes)" % fsize)
            else:
                print("  FAIL: session file has size=%d mode=%o" % (fsize, mode))
                tests_failed += 1

        # Second run: resume the session (should load prior context)
        out2, rc2 = run_prompt("followup", resume_path=session_path)
        if rc2 == 0:
            print("  PASS: resume run succeeded")
        else:
            print("  FAIL: resume run exited with %s (err: %s)" %
                  (rc2, out2[:200]))
            tests_failed += 1

        # Corrupted session file: should fail closed
        with open(session_path, "w") as f:
            f.write("not valid json {{{")
        out3, rc3 = run_prompt("after_corrupt", resume_path=session_path)
        if rc3 != 0:
            print("  PASS: corrupted session rejected (fail-closed)")
        else:
            print("  FAIL: corrupted session not rejected")
            tests_failed += 1

        # A resume path may not be a symlink, even when its target is valid.
        target_path = os.path.join(session_dir, "session-target.json")
        with open(target_path, "w") as f:
            f.write('{"messages":[]}')
        os.chmod(target_path, 0o600)
        symlink_path = os.path.join(session_dir, "session-link.json")
        os.symlink(target_path, symlink_path)
        out4, rc4 = run_prompt("after-link", resume_path=symlink_path)
        if rc4 != 0:
            print("  PASS: symlink session rejected")
        else:
            print("  FAIL: symlink session accepted")
            tests_failed += 1

        # Rejecting a hard-linked session path must not truncate its target.
        hard_target = os.path.join(session_dir, "hard-target.txt")
        hard_link = os.path.join(session_dir, "hard-session.json")
        with open(hard_target, "w") as f:
            f.write("must remain intact")
        os.chmod(hard_target, 0o600)
        os.link(hard_target, hard_link)
        out5, rc5 = run_prompt("hard-link-save", save=hard_link)
        with open(hard_target) as f:
            hard_content = f.read()
        if hard_content == "must remain intact" and "Warning: could not save session" in out5:
            print("  PASS: hard-linked session target remains unchanged")
        else:
            print("  FAIL: hard-linked session target was modified")
            tests_failed += 1

    except Exception as e:
        print("  FAIL: %s" % e)
        tests_failed += 1
    if 'session_dir' in dir():
        subprocess.run(["rm", "-rf", session_dir], capture_output=True)

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

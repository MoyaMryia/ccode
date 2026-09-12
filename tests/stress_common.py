#!/usr/bin/env python3
"""Shared helpers for the ccode-cli stress suites.

Each stress script drives the REAL ccode-cli binary against the real mock
provider (tests/mock_provider.py), so the whole stack -- JSON Lines/SSE
parsing, agent loop, tool execution, sandboxing, session handling -- is
exercised, not a unit-test harness.
"""

import json
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import time

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(TESTS_DIR)
CCODE = os.path.join(REPO_ROOT, "ccode-cli")
MOCK_PROVIDER = os.path.join(TESTS_DIR, "mock_provider.py")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class MockProvider:
    def __init__(self, env_extra=None):
        self.port = free_port()
        env = os.environ.copy()
        if env_extra:
            env.update(env_extra)
        self.log = tempfile.NamedTemporaryFile(
            prefix="ccode_mock_", suffix=".err", delete=False)
        self.proc = subprocess.Popen(
            [sys.executable, MOCK_PROVIDER, str(self.port)],
            stdout=subprocess.DEVNULL, stderr=self.log, env=env)
        time.sleep(0.5)
        if self.proc.poll() is not None:
            raise RuntimeError("mock provider failed to start")

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.log.close()

    def mock_errors(self):
        with open(self.log.name, "rb") as f:
            return f.read().decode("utf-8", errors="replace")


class Runner:
    """Runs ccode-cli with a scripted mock-provider tool-call plan."""

    def __init__(self, workspace, plan, mode="stress-fixture", timeout=300):
        self.workspace = workspace
        self.plan = plan
        self.mode = mode
        self.timeout = timeout
        self.tmp = tempfile.mkdtemp(prefix="ccode_stress_")
        self.plan_path = os.path.join(self.tmp, "plan.json")
        with open(self.plan_path, "w") as f:
            json.dump(plan, f)
        self.session_path = os.path.join(self.tmp, "session.json")
        self.env_extra = {
            "CCODE_MOCK_STRESS_PLAN": self.plan_path,
            "CCODE_AUTO_APPROVE": "1",
            "CCODE_WRITE_TOOLS": "1",
            "CCODE_REQUEST_TIMEOUT": "30",
        }

    mock = None

    def run(self, expect_rc=0):
        mock = MockProvider(env_extra=self.env_extra)
        self.mock = mock
        env = os.environ.copy()
        env.update(self.env_extra)
        env["CCODE_API_BASE"] = "http://127.0.0.1:%d/v1" % mock.port
        env["CCODE_API_KEY"] = "stress-key"
        env["CCODE_MODEL"] = "stress-model"
        env["CCODE_WORKSPACE"] = self.workspace
        env["CCODE_CONTEXT_TOKENS"] = "0"
        env["CCODE_SESSION_DIR"] = os.path.join(self.tmp, "sessions")
        env["CCODE_RESPECT_GITIGNORE"] = "0"
        try:
            start = time.time()
            argv = [CCODE, "--prompt", "__ccode_test_" + self.mode,
                    "--save-session", self.session_path]
            proc = subprocess.run(
                argv, env=env, cwd=self.workspace,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                timeout=self.timeout)
            self.elapsed = time.time() - start
            self.stdout = proc.stdout.decode("utf-8", errors="replace")
            self.stderr = proc.stderr.decode("utf-8", errors="replace")
            self.rc = proc.returncode
        except subprocess.TimeoutExpired as e:
            self.elapsed = time.time() - start
            self.stdout = (e.stdout or b"").decode("utf-8", errors="replace")
            self.stderr = (e.stderr or b"").decode("utf-8", errors="replace")
            self.rc = None
            if self.mock:
                self.stderr += "\n--- mock log tail ---\n" + \
                    self.mock.mock_errors()[-800:]
        finally:
            mock.stop()
        return self

    def cleanup(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    # ── assertions ──

    def assert_completed(self, ctx):
        if self.rc != 0:
            detail = "ccode-cli exited %s (expected 0)\nstderr tail: %s" % (
                self.rc, self.stderr[-800:])
            if self.mock:
                detail += "\n--- mock log tail ---\n" + \
                    self.mock.mock_errors()[-800:]
            fail(ctx, detail)
        if "STRESS-PLAN-COMPLETE n=%d" % len(self.plan) not in self.stdout:
            fail(ctx, "plan completion marker missing from stdout "
                      "(%d steps planned)\nstdout tail: %s" %
                 (len(self.plan), self.stdout[-800:]))

    def assert_result_count(self, ctx):
        got = self.stdout.count("[result]")
        if got != len(self.plan):
            fail(ctx, "expected %d [result] lines, saw %d" %
                 (len(self.plan), got))

    def assert_no_temp_files(self, ctx):
        leftovers = []
        for root, _dirs, files in os.walk(self.workspace):
            for name in files:
                if name.startswith(".ccode-write-"):
                    leftovers.append(os.path.join(root, name))
        if leftovers:
            fail(ctx, "atomic-write temp files leaked: %s" % leftovers[:5])

    def session_results(self):
        """Ordered {tool_call_id: parsed result JSON} plus the final
        assistant text, from the saved session (single JSON document,
        {"version":5,"messages":[...]})."""
        path = self.session_path
        out = {}
        order = []
        final_text = ""
        if not os.path.isfile(path):
            return out, order, final_text
        import json as _json
        with open(path, encoding="utf-8", errors="replace") as f:
            doc = _json.load(f)
        for msg in doc.get("messages", []):
            role = msg.get("role")
            if role == "tool" and msg.get("tool_call_id"):
                cid = msg["tool_call_id"]
                try:
                    out[cid] = _json.loads(msg.get("content") or "{}")
                except ValueError:
                    out[cid] = {"_raw": msg.get("content")}
                order.append(cid)
            elif role == "assistant" and msg.get("content"):
                final_text = msg["content"]
        return out, order, final_text

    def summary(self, label):
        print("  %s: %d steps in %.1fs" % (label, len(self.plan), self.elapsed))


def ok(label):
    print("  PASS: %s" % label)


def fail(ctx, message):
    print("  FAIL: %s" % message)
    sys.exit(1)


def step(sid, name, arguments):
    return {"id": sid, "name": name, "arguments": arguments}


# ── random data generation (shared by the random-workspace stress) ──

NAME_PARTS = ["data", "log", "cfg", "main", "util", "测试", "naïve",
              "long-name-component", "x", "CACHE", "node", "index"]
TEXT_WORDS = ["alpha", "beta", "gamma", "符号", "table", "func", "ret",
              "needle-marker", "TODO", "commit", "cipher", "值", "end"]
CONTROL_CHARS = ["\t", "\r", "\x01", "\x07", "\x0b", "\x1f"]


def rand_name(rng):
    parts = [rng.choice(NAME_PARTS) for _ in range(rng.randint(1, 3))]
    name = "-".join(parts)
    if rng.random() < 0.15:
        name += " " + rng.choice(NAME_PARTS)
    return name


def rand_text(rng, lines):
    out = []
    for _ in range(lines):
        n = rng.randint(1, 12)
        words = [rng.choice(TEXT_WORDS) for _ in range(n)]
        if rng.random() < 0.05:
            words.append(rng.choice(CONTROL_CHARS))
        out.append(" ".join(words))
    return "\n".join(out) + ("\n" if rng.random() < 0.9 else "")


def rand_binary(rng, size):
    chunks = []
    left = size
    while left > 0:
        n = min(left, 4096)
        chunks.append(bytes(rng.getrandbits(8) for _ in range(n)))
        left -= n
    return b"".join(chunks)


def generate_workspace(root, rng, n_files=600, n_big=6, max_depth=5):
    """Random tree: mixed names (unicode/spaces), mixed contents (empty,
    text, CRLF, control chars, binary, huge lines), deep nesting, symlinks,
    empty dirs, plus a few oversized files to force truncation+archiving."""
    made = []
    os.makedirs(root, exist_ok=True)
    for i in range(n_files):
        depth = rng.randint(0, max_depth)
        rel = root
        try:
            for _ in range(depth):
                rel = os.path.join(rel, rand_name(rng))
                os.makedirs(rel, exist_ok=True)
            path = os.path.join(rel, rand_name(rng) +
                                rng.choice([".txt", ".c", ".md", ".log", "", ".json"]))
            if path in made:
                continue
            kind = rng.random()
            if kind < 0.08:
                with open(path, "wb"):
                    pass
            elif kind < 0.18:
                with open(path, "wb") as f:
                    f.write(rand_binary(rng, rng.randint(1, 64 * 1024)))
            elif kind < 0.28:
                with open(path, "w", encoding="utf-8", newline="") as f:
                    f.write(rand_text(rng, 200) + "\r\n" + rand_text(rng, 50))
            elif kind < 0.33:
                with open(path, "w", encoding="utf-8") as f:
                    f.write("HUGE " + rand_text(rng, 1) * 20000)
            else:
                with open(path, "w", encoding="utf-8") as f:
                    f.write(rand_text(rng, rng.randint(1, 400)))
        except OSError:
            continue  # a path component collided with an earlier file etc.
        made.append(path)

    for i in range(n_big):
        path = os.path.join(root, "big_%03d.log" % i)
        line = ("log line %d with needle-marker inside\n" % i) * 20000
        with open(path, "w", encoding="utf-8") as f:
            f.write(line[:rng.choice([70 * 1024, 200 * 1024, 512 * 1024])])
        made.append(path)

    for i in range(8):
        os.makedirs(os.path.join(root, "empty_dir_%d" % i), exist_ok=True)

    src = made[rng.randrange(len(made))] if made else None
    if src and os.path.isfile(src):
        try:
            os.symlink(src, os.path.join(root, "link_to_file"))
        except OSError:
            pass
    return made


def snapshot(root):
    """Deterministic digest of a file tree (rel path + size + mtime)."""
    import hashlib
    h = hashlib.sha256()
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            p = os.path.join(dirpath, name)
            rel = os.path.relpath(p, root)
            try:
                st = os.lstat(p)
                h.update(("%s|%d|%d\n" % (rel, st.st_size, st.st_mtime)).encode())
            except OSError:
                h.update(("%s|missing\n" % rel).encode())
    return h.hexdigest()

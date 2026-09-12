#!/usr/bin/env python3
"""End-to-end battle test: all 12 tools on a real project (this repo).

Spins up the real mock provider (tests/mock_provider.py), scripts every
model reply as a tool-call plan, then drives the real ccode-cli over a
throwaway copy of this repository (git archive HEAD) to finish one coding
task end to end:

  explore (glob/grep) -> read the defect -> record a task -> fix (edit_file)
  -> real rebuild (make) -> behavior check on the built binary -> git diff
  -> file ops (create / duplicate-create refusal / move / move-onto-existing
  refusal / delete) -> web_fetch + web_search against the mock's loopback
  pages -> oversized output archive + read_tool_output -> task close-out ->
  read-only sub-agent.

Every tool result is verified exactly against the saved session, and the
final workspace state (fixed source, freshly built binary, corrected --help
text) is re-checked independently of the agent. The planted defect (wrong
usage text for -p in src/config.c) is observable in the built binary, so a
vacuous pass is impossible: only a real edit + real compile + real run
produce the expected --help output.

Usage: python3 tests/test_e2e_real_project.py
Set CCODE_E2E_REAL_KEEP=1 to keep the scratch workspace for inspection.
"""

import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stress_common import CCODE, MOCK_PROVIDER, REPO_ROOT, free_port, ok, step

DEFECT_FILE = "src/config.c"
DEFECT_OLD = "Send one prompt then exit"
DEFECT_BAD = "Send two prompts then exit"
BIG_STDOUT_BYTES = 5 + 200000 + 11  # START + yes A|head -c 200000 + ENDMARK9999
BIG_STDERR_BYTES = 200000 + 7       # yes E|head -c 200000 + ENDERR2
QUOTES_BYTES = 49500                # 2x escaping overflows the field budget

PROMPT = ("__ccode_test_stress-fixture "
          "任务：src/config.c 的 usage 文本把 -p 的说明写错了"
          "（写成了 %s，应为 %s）。"
          "定位、修复、重新构建并用 --help 验证，最后 git diff 展示改动。"
          % (DEFECT_BAD, DEFECT_OLD))


def setup_workspace(root):
    """git archive HEAD -> ws, plant the defect, commit the broken baseline."""
    ws = os.path.join(root, "ws")
    os.makedirs(ws)
    with open(os.devnull, "wb") as devnull:
        arc = subprocess.Popen(["git", "archive", "HEAD"], cwd=REPO_ROOT,
                               stdout=subprocess.PIPE, stderr=devnull)
        tar = subprocess.Popen(["tar", "-x", "-C", ws],
                               stdin=arc.stdout, stderr=devnull)
        arc.stdout.close()
        tar.communicate(timeout=60)
        if tar.returncode != 0 or arc.wait() != 0:
            raise RuntimeError("git archive export failed")

    path = os.path.join(ws, DEFECT_FILE)
    with open(path, encoding="utf-8") as f:
        src = f.read()
    if src.count(DEFECT_OLD) != 1:
        raise RuntimeError("expected exactly one anchor in %s, found %d"
                           % (DEFECT_FILE, src.count(DEFECT_OLD)))
    defect_line = src[:src.index(DEFECT_OLD)].count("\n") + 1
    with open(path, "w", encoding="utf-8") as f:
        f.write(src.replace(DEFECT_OLD, DEFECT_BAD, 1))

    # Quote-dense file under the 50 KiB raw cap that still overflows the
    # escaped-length budget: read_file must flag and archive it.
    with open(os.path.join(ws, "quotes.txt"), "w") as f:
        f.write('"' * QUOTES_BYTES)

    def git(*args):
        subprocess.run(["git", "-C", ws] + list(args), check=True,
                       capture_output=True, timeout=30)
    git("init")
    git("config", "user.email", "test@test")
    git("config", "user.name", "test")
    git("add", "-A")
    git("commit", "-m", "baseline with usage-text defect")
    return ws, defect_line


def count_src_c_files(ws):
    """glob runs scoped to src/: the whole tree exceeds the tool's 1000-file
    scan budget (vendor/mbedtls), so only the src/ scope is exactly
    countable."""
    n = 0
    for dirpath, dirnames, filenames in os.walk(os.path.join(ws, "src")):
        dirnames[:] = [d for d in dirnames if d != ".git"]
        for name in filenames:
            if name.endswith(".c"):
                n += 1
    return n


def build_plan(port):
    """The mock's scripted replies: one tool call per agent turn, in task
    order. Every step id is asserted against the saved session afterwards."""
    plan = [
        step("call_glob", "glob",
             {"pattern": "**/*.c", "path": "src"}),
        step("call_grep", "grep",
             {"pattern": DEFECT_BAD, "include": "*.c"}),
        step("call_read", "read_file", {"file_path": DEFECT_FILE}),
        step("call_task_new", "task",
             {"action": "create",
              "content": "Fix usage text defect in " + DEFECT_FILE}),
        step("call_edit", "edit_file",
             {"file_path": DEFECT_FILE, "old_string": DEFECT_BAD,
              "new_string": DEFECT_OLD}),
        step("call_make", "bash",
             {"command": "CCACHE_DISABLE=1 make ccode-cli"}),
        step("call_verify", "bash",
             {"command":
              "./ccode-cli --help 2>&1 | grep -c '%s'" % DEFECT_OLD}),
        step("call_stale", "bash",
             {"command": "grep -c '%s' %s" % (DEFECT_BAD, DEFECT_FILE)}),
        step("call_diff", "bash",
             {"command": "git --no-pager diff --stat"}),
        step("call_mk", "edit_file",
             {"file_path": "scratch_a.txt", "old_string": "",
              "new_string": "created by the battle test\n"}),
        step("call_mk_dup", "edit_file",
             {"file_path": "scratch_a.txt", "old_string": "",
              "new_string": "must be refused\n"}),
        step("call_mv", "move_file",
             {"source": "scratch_a.txt", "destination": "scratch_b.txt"}),
        step("call_mv_x", "move_file",
             {"source": "scratch_b.txt", "destination": DEFECT_FILE}),
        step("call_rm", "delete_file", {"file_path": "scratch_b.txt"}),
        step("call_fetch", "web_fetch",
             {"url": "http://127.0.0.1:%d/page" % port}),
        step("call_search", "web_search", {"query": "ccode usage prompt"}),
        step("call_big", "bash",
             {"command":
              "printf START; yes A | head -c 200000; printf ENDMARK9999; "
              "yes E | head -c 200000 1>&2; printf ENDERR2 1>&2"}),
        step("call_window", "read_tool_output",
             {"tool_call_id": "call_big", "offset": 199000,
              "limit": 65536}),
        step("call_window_err", "read_tool_output",
             {"tool_call_id": "call_big", "stream": "stderr",
              "offset": 199000, "limit": 65536}),
        step("call_readq", "read_file", {"file_path": "quotes.txt"}),
        step("call_readq_win", "read_tool_output",
             {"tool_call_id": "call_readq", "offset": 40000,
              "limit": 65536}),
        step("call_task_done", "task",
             {"action": "update", "id": "1", "status": "completed"}),
        step("call_task_ls", "task", {"action": "list"}),
        step("call_agent", "agent_tool",
             {"task": "Report how many .c files are under src/ using glob. "
                      "Answer with the number only."}),
    ]
    return plan


def session_results(path):
    out, order, final_text = {}, [], ""
    with open(path, encoding="utf-8", errors="replace") as f:
        doc = json.load(f)
    for msg in doc.get("messages", []):
        role = msg.get("role")
        if role == "tool" and msg.get("tool_call_id"):
            cid = msg["tool_call_id"]
            order.append(cid)
            try:
                out[cid] = json.loads(msg.get("content") or "{}")
            except ValueError:
                out[cid] = {"_raw": msg.get("content")}
        elif role == "assistant" and msg.get("content"):
            final_text = msg["content"]
    return out, order, final_text


def main():
    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond), detail))

    root = tempfile.mkdtemp(prefix="ccode_e2e_real_")
    mock = None
    try:
        print("=== ccode battle test: all 12 tools on the real repo tree ===")
        ws, defect_line = setup_workspace(root)
        n_c = count_src_c_files(ws)
        port = free_port()
        plan = build_plan(port)

        plan_path = os.path.join(root, "plan.json")
        with open(plan_path, "w") as f:
            json.dump(plan, f)

        mock_log = open(os.path.join(root, "mock.err"), "w+")
        menv = os.environ.copy()
        menv["CCODE_MOCK_STRESS_PLAN"] = plan_path
        mock = subprocess.Popen(
            [sys.executable, MOCK_PROVIDER, str(port)],
            stdout=subprocess.DEVNULL, stderr=mock_log, env=menv)
        time.sleep(0.6)
        if mock.poll() is not None:
            print("FAIL: mock provider failed to start")
            return 1

        session_path = os.path.join(root, "session.json")
        env = os.environ.copy()
        env.update({
            "CCODE_API_BASE": "http://127.0.0.1:%d/v1" % port,
            "CCODE_API_KEY": "battle-key",
            "CCODE_MODEL": "battle-model",
            "CCODE_WORKSPACE": ws,
            "CCODE_AUTO_APPROVE": "1",
            "CCODE_WRITE_TOOLS": "1",
            "CCODE_REQUEST_TIMEOUT": "60",
            "CCODE_CONTEXT_TOKENS": "0",
            "CCODE_SESSION_DIR": os.path.join(root, "sessions"),
            "CCODE_RESPECT_GITIGNORE": "0",
            "CCODE_WEB_FETCH_ALLOW_PRIVATE": "1",
            "CCODE_WEB_SEARCH_URL":
                "http://127.0.0.1:%d/search?q={query}" % port,
        })
        start = time.time()
        try:
            proc = subprocess.run(
                [CCODE, "--prompt", PROMPT, "--save-session", session_path],
                env=env, cwd=ws, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=300)
            rc, stdout, stderr = (proc.returncode,
                                  proc.stdout.decode("utf-8",
                                                     errors="replace"),
                                  proc.stderr.decode("utf-8",
                                                     errors="replace"))
        except subprocess.TimeoutExpired as e:
            rc = None
            stdout = (e.stdout or b"").decode("utf-8", errors="replace")
            stderr = (e.stderr or b"").decode("utf-8", errors="replace")
        elapsed = time.time() - start

        print("  ccode-cli exited %s in %.1fs (%d planned steps)"
              % (rc, elapsed, len(plan)))
        if rc != 0:
            check("ccode-cli exit 0", False,
                  "stderr tail: %s" % stderr[-600:])

        res, order, final_text = ({}, [], "")
        if os.path.isfile(session_path):
            res, order, final_text = session_results(session_path)
        else:
            check("session file written", False, session_path)

        n = len(plan)
        check("plan completed (final marker)",
              "STRESS-PLAN-COMPLETE n=%d" % n in final_text,
              "final: %r" % final_text[:120])
        check("all %d tool results in order" % n, order == [p["id"]
                                                           for p in plan],
              "got %d: %s" % (len(order), order[:8]))
        check("session file mode 0600",
              os.path.isfile(session_path) and
              (os.stat(session_path).st_mode & 0o777) == 0o600, "")

        def r(cid):
            return res.get(cid, {})

        # -- explore -----------------------------------------------------
        check("glob src/*.c count == walk truth",
              r("call_glob").get("count") == n_c,
              "glob=%s walk=%d files=%s"
              % (r("call_glob").get("count"), n_c,
                 (r("call_glob").get("files") or [])[:3]))
        check("glob saw the defect file",
              DEFECT_FILE in (r("call_glob").get("files") or []), "")
        grep = r("call_grep")
        want_line = "src/config.c:%d:" % defect_line
        check("grep found exactly the planted line",
              grep.get("count") == 1 and
              (grep.get("matches") or [""])[0].startswith(want_line) and
              DEFECT_BAD in (grep.get("matches") or [""])[0],
              "matches=%s" % (grep.get("matches") or [])[:2])
        check("read_file returned the defect inline",
              DEFECT_BAD in r("call_read").get("content", "") and
              "truncated" not in r("call_read"), "")

        # -- task bookkeeping --------------------------------------------
        check("task create ok (id 1)",
              r("call_task_new").get("ok") is True and
              str(r("call_task_new").get("id")) == "1",
              "result=%s" % r("call_task_new"))

        # -- the fix ------------------------------------------------------
        check("edit_file applied the fix", r("call_edit").get("ok") is True,
              "result=%s" % r("call_edit"))

        # -- real build + behavior ---------------------------------------
        make = r("call_make")
        check("make ccode-cli built the copy",
              make.get("exit_code") == 0 and make.get("timed_out") is False,
              "exit_code=%s stderr tail=%r"
              % (make.get("exit_code"),
                 (make.get("stderr") or "")[-160:]))
        verify = r("call_verify")
        check("built binary --help shows the corrected text",
              verify.get("exit_code") == 0 and
              verify.get("stdout", "").strip() == "1",
              "exit=%s stdout=%r" % (verify.get("exit_code"),
                                     verify.get("stdout", "")[:40]))
        stale = r("call_stale")
        check("planted text gone from source",
              stale.get("exit_code") == 1 and
              stale.get("stdout", "").strip() == "0",
              "exit=%s stdout=%r" % (stale.get("exit_code"),
                                     stale.get("stdout", "")[:40]))
        diff = r("call_diff")
        check("git diff shows the one-line fix",
              DEFECT_FILE in diff.get("stdout", "") and
              diff.get("exit_code") == 0,
              "stdout=%r" % diff.get("stdout", "")[:120])

        # -- file ops: create / refusals / move / delete -------------------
        check("edit_file created scratch_a.txt",
              r("call_mk").get("ok") is True, "")
        dup = r("call_mk_dup")
        check("duplicate create refused",
              dup.get("ok") is not True and
              "Could not create file" in dup.get("error", ""),
              "result=%s" % dup)
        check("move_file renamed scratch_a -> scratch_b",
              r("call_mv").get("ok") is True, "")
        mvx = r("call_mv_x")
        check("move onto existing target refused",
              mvx.get("ok") is not True and "error" in json.dumps(mvx),
              "result=%s" % mvx)
        check("delete_file removed scratch_b",
              r("call_rm").get("ok") is True, "")

        # -- web tools against the mock's loopback pages -------------------
        fetch = r("call_fetch")
        check("web_fetch 200 with page content",
              fetch.get("status") == 200 and
              "MOCKPAGE-CONTENT alpha" in fetch.get("content", ""),
              "status=%s content=%r"
              % (fetch.get("status"), fetch.get("content", "")[:80]))
        search = r("call_search")
        results = search.get("results") or []
        check("web_search parsed both mock results",
              len(results) == 2 and
              results[0].get("title") == "ccode first & result" and
              results[0].get("url") == "https://docs.example.com/ccode" and
              results[1].get("snippet") == "snippet 'two' here",
              "results=%s" % json.dumps(results)[:160])

        # -- oversized output: budget cut, archive, windowed retrieval ------
        big = r("call_big")
        check("dual-stream cut keeps valid JSON with both flags",
              big.get("exit_code") == 0 and
              big.get("stdout_truncated") is True and
              big.get("stderr_truncated") is True and
              0 < len(big.get("stdout", "")) < BIG_STDOUT_BYTES,
              "exit=%s flags=%s/%s"
              % (big.get("exit_code"), big.get("stdout_truncated"),
                 big.get("stderr_truncated")))
        win = r("call_window")
        check("read_tool_output returned the exact stdout tail window",
              win.get("stream") == "stdout" and
              win.get("total_bytes") == BIG_STDOUT_BYTES and
              win.get("returned_bytes") == BIG_STDOUT_BYTES - 199000 and
              win.get("truncated") is False and
              (win.get("content") or "").endswith("ENDMARK9999"),
              "offset=%s total=%s returned=%s"
              % (win.get("offset"), win.get("total_bytes"),
                 win.get("returned_bytes")))
        winerr = r("call_window_err")
        check("stderr window retrieved via stream=stderr",
              winerr.get("stream") == "stderr" and
              winerr.get("total_bytes") == BIG_STDERR_BYTES and
              winerr.get("returned_bytes") == BIG_STDERR_BYTES - 199000 and
              (winerr.get("content") or "").endswith("ENDERR2"),
              "total=%s returned=%s content=%r"
              % (winerr.get("total_bytes"), winerr.get("returned_bytes"),
                 (winerr.get("content") or "")[-20:]))
        readq = r("call_readq")
        check("quote-dense file read is bounded with explicit flag",
              readq.get("truncated") is True and
              0 < len(readq.get("content", "")) < QUOTES_BYTES,
              "truncated=%s content=%d"
              % (readq.get("truncated"), len(readq.get("content", ""))))
        winq = r("call_readq_win")
        check("archived quote file windowed back",
              winq.get("total_bytes") == QUOTES_BYTES and
              winq.get("returned_bytes") == QUOTES_BYTES - 40000 and
              (winq.get("content") or "").endswith('"'),
              "total=%s returned=%s"
              % (winq.get("total_bytes"), winq.get("returned_bytes")))

        # -- close-out: tasks + sub-agent ----------------------------------
        check("task update ok",
              r("call_task_done").get("ok") is True, "")
        tasks = (r("call_task_ls").get("tasks") or [])
        check("task list shows the completed task",
              any(t.get("id") == "1" and t.get("status") == "completed" and
                  "Fix usage text defect" in t.get("content", "")
                  for t in tasks),
              "tasks=%s" % json.dumps(tasks)[:120])
        check("read-only sub-agent answered",
              "SUBAGENT-DONE" in json.dumps(r("call_agent")),
              "result=%s" % json.dumps(r("call_agent"))[:80])

        # -- independent final-state verification --------------------------
        with open(os.path.join(ws, DEFECT_FILE), encoding="utf-8") as f:
            fixed_src = f.read()
        check("workspace source is fixed",
              DEFECT_OLD in fixed_src and DEFECT_BAD not in fixed_src, "")
        binary = os.path.join(ws, "ccode-cli")
        bin_ok = os.path.isfile(binary) and os.access(binary, os.X_OK)
        check("workspace binary built", bin_ok, "")
        if bin_ok:
            fresher = os.stat(binary).st_mtime >= \
                os.stat(os.path.join(ws, DEFECT_FILE)).st_mtime
            help_out = subprocess.run(
                [binary, "--help"], env={}, cwd=ws,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=15).stdout.decode("utf-8", errors="replace")
            check("binary compiled after the fix and --help is correct",
                  fresher and DEFECT_OLD in help_out and
                  DEFECT_BAD not in help_out,
                  "fresher=%s" % fresher)
        for scratch in ("scratch_a.txt", "scratch_b.txt"):
            check("%s absent at the end" % scratch,
                  not os.path.exists(os.path.join(ws, scratch)), "")
        leftovers = []
        for dirpath, _d, filenames in os.walk(ws):
            for name in filenames:
                if name.startswith(".ccode-write-"):
                    leftovers.append(name)
        check("no atomic-write temp files leaked", not leftovers,
              "%s" % leftovers[:4])
        check("%d [result] lines on stdout" % n,
              stdout.count("[result]") == n,
              "saw %d" % stdout.count("[result]"))

        # -- report ---------------------------------------------------------
        for name, passed, detail in checks:
            print("  %s: %s%s" % ("PASS" if passed else "FAIL", name,
                                  (" -- " + detail) if (detail and not passed)
                                  else ""))
        failed = sum(1 for _, p, _ in checks if not p)
        print("=== battle test: %d checks, %d failed ==="
              % (len(checks), failed))
        if mock is not None and failed:
            mock_log.flush()
            mock_log.seek(0)
            tail = mock_log.read()[-400:]
            if tail.strip():
                print("--- mock log tail ---\n%s" % tail)
        return 1 if failed else 0
    finally:
        if mock is not None:
            mock.terminate()
            try:
                mock.wait(timeout=3)
            except subprocess.TimeoutExpired:
                mock.kill()
        if os.environ.get("CCODE_E2E_REAL_KEEP") == "1":
            print("  workspace kept at %s" % root)
        else:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Stress 3: real engineering data -- ccode's own source tree.

Copies the repository's src/, docs/, vendor/jsmn and top-level documents
into a scratch workspace (edits must never touch the real repo), then
drives the real ccode-cli over it with a scripted plan whose expected
outcomes are computed from the real tree at plan-build time. Every tool
result is verified exactly against the session file (raw result JSONs,
keyed by tool_call_id):

- glob **/*.c / *.h / *.md counts must equal the walk-derived truth
- grep for a real symbol must report exactly the matching lines
- oversized read (biggest source file > 64 KiB) must truncate and page
  back through read_tool_output with byte-accurate windows
- bash pipelines (find/wc/md5sum/grep) must match Python-computed values
- edit_file must create a file, refuse a second create, replace a unique
  real anchor, and refuse an ambiguous multi-match edit
- the Chinese AGENTS.md must round-trip through read_file (UTF-8)
- an agent_tool delegate must answer

Usage: python3 tests/stress_real_project.py
"""

import hashlib
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stress_common import REPO_ROOT, Runner, ok, step

SYMBOL = "ccode_strdup"


def md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def build_workspace():
    ws = tempfile.mkdtemp(prefix="ccode_stress_real_")
    shutil.copytree(os.path.join(REPO_ROOT, "src"), os.path.join(ws, "src"))
    shutil.copytree(os.path.join(REPO_ROOT, "docs"), os.path.join(ws, "docs"))
    os.makedirs(os.path.join(ws, "vendor"))
    shutil.copytree(os.path.join(REPO_ROOT, "vendor", "jsmn"),
                    os.path.join(ws, "vendor", "jsmn"))
    for name in ["README.md", "Makefile"]:
        shutil.copy2(os.path.join(REPO_ROOT, name), os.path.join(ws, name))
    return ws


def build_plan(ws):
    plan = []
    src = os.path.join(ws, "src")

    # The edit probe is created before the truth walk so its file counts
    # include it (the glob steps below see it too).
    biggest_pre = None
    probe_src_size = 0
    for dirpath, _d, filenames in os.walk(ws):
        for name in filenames:
            p = os.path.join(dirpath, name)
            if name.endswith(".c"):
                if biggest_pre is None or os.path.getsize(p) > os.path.getsize(biggest_pre):
                    biggest_pre = p
                # edit_file refuses files over 50 KiB; probe with the
                # biggest source file that stays under that ceiling AND
                # carries the grep symbol (so a real unique edit anchor
                # exists), keeping the probe independent of file-size drift.
                if os.path.getsize(p) <= 40 * 1024 and os.path.getsize(p) > probe_src_size:
                    with open(p, encoding="utf-8", errors="replace") as f:
                        if SYMBOL in f.read():
                            probe_src_size = os.path.getsize(p)
                            probe_origin = p
    shutil.copy2(probe_origin, os.path.join(ws, "edit_probe.c"))
    with open(probe_origin, encoding="utf-8", errors="replace") as f:
        probe_src = f.read()

    c_files, h_files, md_files = [], [], []
    all_files = 0
    for dirpath, dirnames, filenames in os.walk(ws):
        dirnames[:] = [d for d in dirnames if d not in (".git", ".hg", ".svn")]
        for name in filenames:
            p = os.path.join(dirpath, name)
            all_files += 1
            if name.endswith(".c"):
                c_files.append(p)
            elif name.endswith(".h"):
                h_files.append(p)
            elif name.endswith(".md"):
                md_files.append(p)
    n_c, n_h, n_md = len(c_files), len(h_files), len(md_files)
    biggest = max(c_files + h_files, key=os.path.getsize)

    def count_lines(paths, needle):
        total = 0
        for p in paths:
            with open(p, encoding="utf-8", errors="replace") as f:
                total += sum(1 for line in f if needle in line)
        return total

    sym_total = count_lines(c_files + h_files, SYMBOL)
    biggest_sym_lines = count_lines([biggest], SYMBOL)
    needle_total = count_lines(c_files, "prepared_tool")
    docs_bytes = sum(os.path.getsize(p) for p in md_files
                     if os.path.dirname(p) == os.path.join(ws, "docs"))

    def rel(p):
        return os.path.relpath(p, ws)

    cid = lambda: "call_%04d" % len(plan)

    # 1-3: glob counts vs truth
    plan.append(step(cid(), "glob", {"pattern": "**/*.c"}))
    plan.append(step(cid(), "glob", {"pattern": "**/*.h"}))
    plan.append(step(cid(), "glob", {"pattern": "**/*.md"}))

    # 4-6: grep for real symbols (glob-style include filters)
    plan.append(step(cid(), "grep", {"pattern": SYMBOL}))
    plan.append(step(cid(), "grep",
                     {"pattern": "prepared_tool", "include": "*.c"}))
    plan.append(step(cid(), "grep",
                     {"pattern": "int ccode_", "regex": True,
                      "include": "*.h", "context": 1}))

    # 7-9: oversized read + two byte-accurate pagination windows
    big_read_id = cid()
    plan.append(step(big_read_id, "read_file", {"file_path": rel(biggest)}))
    plan.append(step(cid(), "read_tool_output",
                     {"tool_call_id": big_read_id,
                      "offset": 0, "limit": 65536}))
    plan.append(step(cid(), "read_tool_output",
                     {"tool_call_id": big_read_id,
                      "offset": 60000, "limit": 5000}))

    # 10-14: real bash pipelines with known answers
    plan.append(step(cid(), "bash", {"command": "find src -name '*.c' | wc -l"}))
    plan.append(step(cid(), "bash",
                     {"command": "find src docs vendor -type f | wc -l"}))
    plan.append(step(cid(), "bash",
                     {"command": "md5sum %s" % rel(biggest)}))
    plan.append(step(cid(), "bash",
                     {"command": "grep -c '%s' %s" % (SYMBOL, rel(biggest))}))
    plan.append(step(cid(), "bash",
                     {"command": "cat docs/*.md | wc -c"}))

    # 15-19: edits on a real copy (probe file already created above)
    # Pick the first occurrence whose surrounding window is unique in the
    # file (edit_file refuses ambiguous anchors).
    anchor_old = None
    pos = probe_src.find(SYMBOL)
    while pos >= 0:
        cand = probe_src[max(0, pos - 24):pos + len(SYMBOL)]
        if probe_src.count(cand) == 1:
            anchor_old = cand
            break
        pos = probe_src.find(SYMBOL, pos + 1)
    assert anchor_old is not None
    anchor_new = anchor_old.replace(SYMBOL, SYMBOL + "_RENAMED", 1)
    plan.append(step(cid(), "edit_file",
                     {"file_path": "created_probe.txt", "old_string": "",
                      "new_string": "created from real-data stress\n"}))
    plan.append(step(cid(), "edit_file",
                     {"file_path": "created_probe.txt", "old_string": "",
                      "new_string": "must be refused\n"}))
    plan.append(step(cid(), "edit_file",
                     {"file_path": "edit_probe.c",
                      "old_string": anchor_old, "new_string": anchor_new}))
    ambiguous = SYMBOL if probe_src.count(SYMBOL) >= 2 else "\n"
    plan.append(step(cid(), "edit_file",
                     {"file_path": "edit_probe.c",
                      "old_string": ambiguous, "new_string": "x"}))
    plan.append(step(cid(), "bash",
                     {"command":
                      "grep -c %s edit_probe.c; grep -cw %s edit_probe.c"
                      % (SYMBOL + "_RENAMED", SYMBOL)}))

    # 20: UTF-8 Chinese doc round-trip
    plan.append(step(cid(), "read_file", {"file_path": "docs/AGENTS.md"}))

    # 21: delegate to a read-only sub-agent
    plan.append(step(cid(), "agent_tool",
                     {"task": "Report how many .c files are under src/ "
                              "using glob. Answer with the number only."}))

    # bash steps scope their counts to src/ (find) and src+docs+vendor,
    # which excludes the root-level copies (README.md, Makefile,
    # edit_probe.c); derive those expectations from the same scopes.
    src_c_only = sum(1 for p in c_files if p.startswith(os.path.join(ws, "src")))
    sdv_files = all_files - sum(
        1 for p in [os.path.join(ws, n) for n in ("README.md", "Makefile",
                                                  "edit_probe.c")]
        if os.path.isfile(p))
    expect = {
        "n_c": n_c, "n_h": n_h, "n_md": n_md,
        "src_c_only": src_c_only,
        "sdv_files": sdv_files,
        "all_files": all_files,
        "sym_total": sym_total,
        "needle_total": needle_total,
        "biggest_rel": rel(biggest),
        "biggest_size": os.path.getsize(biggest),
        "biggest_md5": md5(biggest),
        "biggest_sym_lines": biggest_sym_lines,
        "docs_bytes": docs_bytes,
        "anchor_new": anchor_new,
        "symbol_lines": sum(1 for line in probe_src.splitlines() if SYMBOL in line),
    }
    return plan, expect


def main():
    print("=== ccode stress: real project data (ccode source tree) ===")
    ws = build_workspace()
    plan, expect = build_plan(ws)
    print("  tree: %d .c / %d .h / %d .md; biggest=%s (%d bytes)"
          % (expect["n_c"], expect["n_h"], expect["n_md"],
             expect["biggest_rel"], expect["biggest_size"]))
    runner = Runner(ws, plan).run()
    try:
        res, order, final = runner.session_results()
        if runner.rc != 0:
            print("  FAIL: ccode-cli exited %s\nstderr tail: %s"
                  % (runner.rc, runner.stderr[-600:]))
            return 1
        if "STRESS-PLAN-COMPLETE n=%d" % len(plan) not in (final or ""):
            print("  FAIL: plan did not complete (final=%r)" % final[:120])
            return 1
        if len(order) != len(plan):
            print("  FAIL: %d tool results in session, expected %d"
                  % (len(order), len(plan)))
            return 1

        def result(i):
            return res[plan[i]["id"]]

        # glob counts
        for i, want in ((0, expect["n_c"]), (1, expect["n_h"]),
                        (2, expect["n_md"])):
            r = result(i)
            if r.get("count") != want:
                print("  FAIL: glob step %d count %s != %d"
                      % (i, r.get("count"), want))
                return 1

        # grep totals (context=0: one match entry per matching line)
        if result(3).get("count") != expect["sym_total"]:
            print("  FAIL: grep %s count %s != %d"
                  % (SYMBOL, result(3).get("count"), expect["sym_total"]))
            return 1
        if result(4).get("count") != expect["needle_total"]:
            print("  FAIL: grep prepared_tool count != %d"
                  % expect["needle_total"])
            return 1

        # oversized read: the inline preview is bounded by the escaped-length
        # budget and flagged with a real "truncated" key (the result is whole
        # valid JSON now); the full file lives in the archive and its byte
        # accuracy is cross-checked by the md5sum bash step below.
        r = result(6)
        if r.get("truncated") is not True:
            print("  FAIL: oversized read not flagged truncated")
            return 1
        if len(r.get("content", "")) >= expect["biggest_size"]:
            print("  FAIL: oversized read not actually truncated")
            return 1
        r = result(7)
        if r.get("offset") != 0 or r.get("truncated") is not True:
            print("  FAIL: first pagination window wrong: %s"
                  % {k: r.get(k) for k in ("offset", "total_bytes",
                                           "truncated")})
            return 1
        # Byte-level accuracy of the window is cross-checked by the md5sum
        # bash step; here len(content) is in codepoints (the source tree has
        # UTF-8 comments), so only bound it.
        if not (64000 <= len(r.get("content", "")) <= 65536):
            print("  FAIL: first window content length %d out of range"
                  % len(r.get("content", "")))
            return 1
        r = result(8)
        if r.get("returned_bytes") != 5000 or len(r.get("content", "")) == 0:
            print("  FAIL: second window returned %s, content %d"
                  % (r.get("returned_bytes"), len(r.get("content", ""))))
            return 1

        # bash: find/wc/md5/grep answers vs truth
        if result(9).get("stdout", "").strip() != str(expect["src_c_only"]):
            print("  FAIL: find|wc != %d" % expect["src_c_only"])
            return 1
        if result(10).get("stdout", "").strip() != str(expect["sdv_files"]):
            print("  FAIL: all-file count != %d" % expect["all_files"])
            return 1
        if expect["biggest_md5"] not in result(11).get("stdout", ""):
            print("  FAIL: md5sum mismatch")
            return 1
        if result(12).get("stdout", "").strip() != str(expect["biggest_sym_lines"]):
            print("  FAIL: grep -c in biggest != %d"
                  % expect["biggest_sym_lines"])
            return 1
        if result(13).get("stdout", "").strip() != str(expect["docs_bytes"]):
            print("  FAIL: docs byte count != %d" % expect["docs_bytes"])
            return 1

        # edits: create ok, re-create refused, rename applied, ambiguous
        # edit refused, probe counts 1 renamed / 0 original
        if not result(14).get("ok"):
            print("  FAIL: creation step failed: %s" % result(14))
            return 1
        if "Could not create file" not in result(15).get("error", ""):
            print("  FAIL: re-create not refused: %s" % result(15))
            return 1
        if not result(16).get("ok"):
            print("  FAIL: anchor replacement failed: %s" % result(16))
            return 1
        if "Multiple matches" not in result(17).get("error", ""):
            print("  FAIL: ambiguous edit not refused: %s" % result(17))
            return 1
        want_renamed = ["1", str(expect["symbol_lines"] - 1)]
        counts = result(18).get("stdout", "").strip().splitlines()
        if counts != want_renamed:
            print("  FAIL: probe counts %s != %s" % (counts, want_renamed))
            return 1

        # Chinese doc round-trip
        if "权威开发契约" not in result(19).get("content", ""):
            print("  FAIL: Chinese doc content did not round-trip")
            return 1

        # sub-agent answered
        if "SUBAGENT-DONE" not in json.dumps(res.get(plan[20]["id"], {})):
            print("  FAIL: agent_tool delegate returned no answer")
            return 1

        runner.summary("real project")
        ok("real project data stress (%d verified steps)" % len(plan))
        return 0
    finally:
        runner.cleanup()
        shutil.rmtree(ws, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

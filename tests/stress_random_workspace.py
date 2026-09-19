#!/usr/bin/env python3
"""Stress 1: random workspace + random tool-call plan against ccode-cli.

Generates a workspace tree from seeded random data (unicode names, empty/
binary/CRLF/control-char files, huge lines, oversized logs, symlinks, empty
dirs), then scripts 250+ tool calls over it through the real mock provider:
search, read (with truncation and blob pagination), edit (create/replace/
collision), move/delete, bash pipelines (with >64KiB output archiving),
tasks, and deliberately malformed calls. Pass = plan completes, every call
produces exactly one rendered result, exit 0, no leaked temp files.

Usage: python3 tests/stress_random_workspace.py [--seed N] [--steps N]
"""

import argparse
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stress_common import (Runner, generate_workspace, rand_name, rand_text,
                           step, ok, fail, snapshot)

BIG_READ_IDS = []
BIG_BASH_IDS = []
EDIT_TARGETS = []


def build_plan(rng, files, n_steps):
    plan = []
    readable = [f for f in files if os.path.isfile(f)]
    big = [f for f in readable if os.path.getsize(f) > 68 * 1024]
    normal = [f for f in readable if os.path.getsize(f) <= 68 * 1024]

    def rel(p):
        return os.path.relpath(p, WS)

    i = 0
    while len(plan) < n_steps:
        cid = "call_%05d" % len(plan)
        kind = rng.random()
        if kind < 0.16:
            pattern = rng.choice([
                "*.txt", "**/*.txt", "**/*.log", "big_*.log", "*.c",
                "**/data-*", "^[a-z]+_[0-9]+\\.md$", "empty_dir_*",
            ])
            if pattern.startswith("^"):
                plan.append(step(cid, "glob",
                                 {"pattern": pattern, "regex": True}))
            else:
                plan.append(step(cid, "glob", {"pattern": pattern}))
        elif kind < 0.34:
            args = {"pattern": rng.choice([
                "needle-marker", "TODO", "符号", "alpha beta",
                "^.*func.*$", "[a-z]+_[0-9]+",
            ])}
            if args["pattern"].startswith("^") or "_[0-9]" in args["pattern"]:
                args["regex"] = True
            if rng.random() < 0.4 and big:
                args["include"] = "big_*.log"
            if rng.random() < 0.3:
                args["context"] = rng.randint(0, 3)
            plan.append(step(cid, "grep", args))
        elif kind < 0.5 and big:
            # Oversized read -> inline preview truncated, full text archived.
            # The pagination follow-up is emitted right after its producer so
            # a chunk boundary never splits the pair (tool_call_id lives only
            # inside one ccode run).
            target = rng.choice(big)
            plan.append(step(cid, "str_replace_editor",
                             {"command": "view", "file_path": rel(target)}))
            if len(plan) < n_steps:
                plan.append(step("call_%05d" % len(plan), "read_tool_output",
                                 {"tool_call_id": cid,
                                  "offset": rng.randint(0, 40000),
                                  "limit": rng.choice([1024, 16384, 65536])}))
        elif kind < 0.62:
            plan.append(step(cid, "str_replace_editor",
                             {"command": "view",
                              "file_path": rel(rng.choice(normal))}))
        elif kind < 0.7:
            if rng.random() < 0.5:
                # Create a fresh file via str_replace (empty old_string).
                name = "created_%s_%04d.txt" % (rand_name(rng).replace(" ", "_"),
                                                len(plan))
                EDIT_TARGETS.append(name)
                plan.append(step(cid, "str_replace_editor",
                                 {"command": "str_replace", "file_path": name,
                                  "old_string": "",
                                  "new_string": rand_text(rng, 30)}))
            elif EDIT_TARGETS:
                # Replace a planted snippet in a file we created earlier.
                name = rng.choice(EDIT_TARGETS)
                plan.append(step(cid, "str_replace_editor",
                                 {"command": "str_replace", "file_path": name,
                                  "old_string": "alpha", "new_string": "ALPHA"}))
            else:
                plan.append(step(cid, "str_replace_editor",
                                 {"command": "str_replace",
                                  "file_path": "fresh_edit_%04d.txt" % len(plan),
                                  "old_string": "", "new_string": "x\n"}))
        elif kind < 0.74:
            plan.append(step(cid, "move_file",
                             {"source": "created-missing-%04d.txt" % len(plan),
                              "destination": "moved-%04d.txt" % len(plan)}))
        elif kind < 0.78:
            plan.append(step(cid, "delete_file",
                             {"file_path": "never-existed-%04d.txt" % len(plan)}))
        elif kind < 0.88:
            r = rng.random()
            if r < 0.4:
                plan.append(step(cid, "bash",
                                 {"command": "wc -c < %s" % rel(rng.choice(readable))}))
            elif r < 0.7:
                # Oversized stdout (> 64 KiB) -> truncated + archived, with
                # the pagination follow-up kept adjacent (same reasoning as
                # the oversized read pair above).
                plan.append(step(cid, "bash",
                                 {"command": "yes 'stress-line' | head -c 200000"}))
                if len(plan) < n_steps:
                    plan.append(step("call_%05d" % len(plan), "read_tool_output",
                                     {"tool_call_id": cid,
                                      "offset": 0, "limit": 65536}))
            else:
                plan.append(step(cid, "bash",
                                 {"command": rng.choice([
                                     "true", "false", "echo stress-ok",
                                     "printf 'a\\tb\\nc\\n' | sort",
                                     "head -c 300 /dev/zero | tr '\\0' 'x'",
                                 ])}))
        elif kind < 0.91:
            plan.append(step(cid, "bash",
                             {"command": "grep -c needle-marker %s | head -1"
                              % rel(rng.choice(big)) if big else
                              "echo no-big"}))
        elif kind < 0.95:
            act = rng.choice(["create", "update", "list"])
            if act == "create":
                plan.append(step(cid, "task",
                                 {"action": "create",
                                  "content": "stress task %d" % len(plan)}))
            elif act == "update":
                plan.append(step(cid, "task",
                                 {"action": "update", "id": "1",
                                  "status": rng.choice(
                                      ["in_progress", "completed", "pending"])}))
            else:
                plan.append(step(cid, "task", {"action": "list"}))
        else:
            # Deliberately malformed: must yield structured errors, never
            # crashes, and still count as one rendered result each.
            bad = rng.choice([
                ("no_such_tool", {"x": 1}),
                ("glob", {"pattern": ""}),
                ("grep", {"pattern": "[invalid"}),
                ("str_replace_editor", {}),
                ("bash", {"command": "cat ~/.ssh/id_rsa"}),
                ("task", {"action": "frobnicate"}),
                ("read_tool_output", {"tool_call_id": "bogus-id"}),
                ("move_file", {"source": "a.txt", "destination": "created_"}),
            ])
            plan.append(step(cid, bad[0], bad[1]))
    return plan


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=20260912)
    ap.add_argument("--steps", type=int, default=800)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    global WS
    import tempfile
    WS = tempfile.mkdtemp(prefix="ccode_stress_ws_")
    print("=== ccode stress: random workspace (seed=%d, steps=%d) ===" %
          (args.seed, args.steps))

    files = generate_workspace(WS, rng)
    print("  workspace: %d files" % len(files))
    before = snapshot(WS)
    plan = build_plan(rng, files, args.steps)

    # ccode caps a single run at MAX_TURN_LIMIT (50) model rounds; run the
    # plan in self-contained chunks below that ceiling.
    CHUNK = 40
    total_results = 0
    elapsed = 0.0
    for start in range(0, len(plan), CHUNK):
        chunk = plan[start:start + CHUNK]
        runner = Runner(WS, chunk).run()
        try:
            runner.assert_completed("random-workspace chunk %d" % start)
            runner.assert_result_count("random-workspace chunk %d" % start)
            runner.assert_no_temp_files("random-workspace chunk %d" % start)
            total_results += len(chunk)
            elapsed += runner.elapsed
        except SystemExit:
            raise
        finally:
            runner.cleanup()

    after = snapshot(WS)
    if after == before:
        fail("workspace snapshot unchanged -- no tool side effects landed")
    print("  %d steps in %.1fs" % (total_results, elapsed))
    ok("random workspace stress")


if __name__ == "__main__":
    sys.exit(main())

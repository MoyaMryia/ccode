#!/usr/bin/env python3
"""Mutation tester for the tool-call parser and the command/path security
guards.

For each mutant it patches one source file with a deliberately wrong
implementation, forces a rebuild of tests/test_agent, runs the unit suite and
the fuzzer(s) that cover that area, restores the file, and records whether the
tests "killed" the mutant (any gate failed) or it "survived" (all gates
passed, i.e. the suite has a blind spot).

Usage:
  tests/mutate.py [--only SUBSTR] [--seed N] [--no-fuzz]

Exit codes: 0 (report produced), 2 (harness error).
"""

import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Gates run after each build. "agent" is the unit suite; the rest are fuzzers.
GATES = {
    "agent": ["./tests/test_agent"],
    "cmd": ["python3", "tests/fuzz_command_paths.py", "--probe",
            "./tests/test_agent", "--count", "300", "--seed", "1"],
    "paths": ["python3", "tests/fuzz_paths.py", "--probe",
              "./tests/test_agent", "--count", "500", "--seed", "1"],
    "args": ["python3", "tests/fuzz_tool_args.py", "--probe",
             "./tests/test_agent", "--count", "500", "--seed", "1"],
}

MUTANTS = [
    # ── command-path filter (src/security/sandbox.c) ──────────────────────────────
    dict(label="hard-match boundary after removed", area="sandbox",
         file="src/security/sandbox.c", gates=["agent", "cmd"],
         old="        if (i + pl < tl && is_filename_char((unsigned char)text[i + pl]))\n"
             "            continue;\n",
         new=""),
    dict(label="soft hit always considered inside workspace", area="sandbox",
         file="src/security/sandbox.c", gates=["agent", "cmd"],
         old="    if (!ws || ws[0] == '\\0') return 0;\n    wl = strlen(ws);\n"
             "    while (s > 0 && !is_cmd_sep((unsigned char)text[s - 1])) s--;",
         new="    wl = strlen(ws);\n    return 1;\n"
             "    while (s > 0 && !is_cmd_sep((unsigned char)text[s - 1])) s--;"),
    dict(label=".. climb check disabled", area="sandbox",
         file="src/security/sandbox.c", gates=["agent", "cmd"],
         old="    for (i = from; i < to; i++) {\n        if (text[i] != '.') continue;",
         new="    for (i = from; 0 && i < to; i++) {\n        if (text[i] != '.') continue;"),
    dict(label="rm -rf / detection disabled", area="sandbox",
         file="src/security/sandbox.c", gates=["agent", "cmd"],
         old="    while ((p = strstr(p, \"rm \")) != NULL) {",
         new="    while (0 && (p = strstr(p, \"rm \")) != NULL) {"),
    dict(label="destructive-command detection disabled", area="sandbox",
         file="src/security/sandbox.c", gates=["agent"],
         old="    for (i = 0; i < sizeof(destructive_commands) / sizeof(destructive_commands[0]); i++) {\n"
             "        if (!has_word(text, destructive_commands[i])) continue;",
         new="    for (i = 0; 0 && i < sizeof(destructive_commands) / sizeof(destructive_commands[0]); i++) {\n"
             "        if (!has_word(text, destructive_commands[i])) continue;"),
    dict(label="owner-home tolerance removed", area="sandbox",
         file="src/security/sandbox.c", gates=["agent", "cmd"],
         old="        if (!soft_hit_inside_workspace(text, i, ws) &&\n"
             "            !(owner != NULL && owner[0] != '\\0' &&\n"
             "              soft_hit_inside_workspace(text, i, owner)))\n"
             "            return 0;",
         new="        if (!soft_hit_inside_workspace(text, i, ws))\n            return 0;"),

    # ── file-path validators (src/agent/agent_fs.c, agent_args.c) ────────
    dict(label="workspace path \\ : check removed", area="paths",
         file="src/agent/agent_fs.c", gates=["agent", "paths"],
         old="    if (strchr(path, '\\\\') != NULL || strchr(path, ':') != NULL)\n"
             "        return 0;\n",
         new=""),
    dict(label="openat component \\ : check removed", area="paths",
         file="src/agent/agent_fs.c", gates=["agent", "paths"], count=-1,
         old="        if (strchr(component, '\\\\') != NULL || strchr(component, ':') != NULL) {\n"
             "            close(dir_fd);\n"
             "            return -1;\n"
             "        }\n",
         new=""),
    dict(label="contains_home_path drops $HOME", area="paths",
         file="src/agent/agent_args.c", gates=["agent"],
         old='        "~/", "~\\\\", "$HOME/", "${HOME}/"\n',
         new='        "~/", "~\\\\"\n'),

    # ── tool-call argument parser (agent_prepare.c, agent_internal.h, json.c) ─
    dict(label="envelope wrap cap 8 -> 1", area="toolargs",
         file="src/agent/agent_internal.h", gates=["agent", "args"],
         old="#define CCODE_MAX_TOOL_ARG_WRAP 8\n",
         new="#define CCODE_MAX_TOOL_ARG_WRAP 1\n"),
    dict(label="envelope trailing-data check disabled", area="toolargs",
         file="src/agent/agent_prepare.c", gates=["agent", "args"],
         old="    if (!only_whitespace_after_root(s, &tokens[0])) return 0;\n",
         new="    if (0) return 0;\n"),
    dict(label="jsmn primitive-before-bracket flush disabled", area="toolargs",
         file="vendor/json/json.c", gates=["agent", "args"],
         old="            if (token_start >= 0 && token_type == CCODE_JSMN_PRIMITIVE) {\n"
             "                if (push_token(parser, CCODE_JSMN_PRIMITIVE, token_start,\n"
             "                               (int)parser->pos, tokens, num_tokens) != 0)\n"
             "                    return -1;",
         new="            if (0) {"),

    # ── oversized tool-result archive (agent_results.c) ──────────────────
    dict(label="result archive drops the tail", area="results",
         file="src/agent/agent_results.c", gates=["agent"],
         old="        int ok = write_all(fd, preview, preview_len) == 0 &&\n"
             "                 write_all(fd, tail, tail_len) == 0;\n",
         new="        int ok = write_all(fd, preview, preview_len) == 0;\n"),
    dict(label="result read ignores offset", area="results",
         file="src/agent/agent_results.c", gates=["agent"],
         old="    if (lseek(fd, (off_t)offset, SEEK_SET) < 0 && offset != 0) {\n",
         new="    if (0) {\n"),
    dict(label="session drops result_ref on save", area="results",
         file="src/agent/message.c", gates=["agent"],
         old="        if (conv->messages[i].result_blob) {\n",
         new="        if (0 && conv->messages[i].result_blob) {\n"),
    dict(label="read_tool_output ignores offset arg", area="results",
         file="src/agent/agent_prepare.c", gates=["agent"],
         old="                prepared->result_offset = (size_t)v;\n",
         new="                prepared->result_offset = 0;\n"),
    dict(label="session delete keeps results archive", area="results",
         file="src/agent/message.c", gates=["agent"],
         old="    if (unlink(path) != 0) return -1;\n"
             "    remove_results_dir(dir, name);\n",
         new="    if (unlink(path) != 0) return -1;\n"),
    dict(label="session rename leaves results archive", area="results",
         file="src/agent/message.c", gates=["agent"],
         old="    if (rename(old_path, new_path) != 0) return -1;\n"
             "    rename_results_dir(dir, old_name, new_name);\n",
         new="    if (rename(old_path, new_path) != 0) return -1;\n"),
    dict(label="editor view skips oversized archive", area="results",
         file="src/agent/agent_fs.c", gates=["agent"],
         old="    if (file_size > read_limit && ctx->results_dir[0] != '\\0') {\n",
         new="    if (0 && file_size > read_limit && ctx->results_dir[0] != '\\0') {\n"),
    dict(label="result configure skips parent dirs", area="results",
         file="src/agent/agent_results.c", gates=["agent"],
         old="    if (mkdir_p(ctx->results_dir) != 0) {\n",
         new="    if (mkdir(ctx->results_dir, 0700) != 0) {\n"),
    dict(label="stderr archive dropped", area="results",
         file="src/agent/agent_exec.c", gates=["agent"],
         old="    if (stderr_tail.len > 0 && ctx->results_dir[0] != '\\0') {\n",
         new="    if (0 && stderr_tail.len > 0 && ctx->results_dir[0] != '\\0') {\n"),

    # ── web_fetch (src/net/webfetch.c) ──────────────────────────────────────
    dict(label="webfetch result margin too small", area="webfetch",
         file="src/net/webfetch.c", gates=["agent"],
         old="            size_t rcap = strlen(escaped) + strlen(esc_url) +\n"
             "                          strlen(esc_ct) + 128;\n",
         new="            size_t rcap = strlen(escaped) + strlen(esc_url) +\n"
             "                          strlen(esc_ct) + 1;\n"),
    dict(label="webfetch relative dot folding disabled", area="webfetch",
         file="src/net/webfetch.c", gates=["agent"],
         old="        while (loc[0] == '.' &&\n"
             "               (loc[1] == '/' ||\n"
             "                (loc[1] == '.' && (loc[2] == '/' || loc[2] == '\\0')))) {\n",
         new="        while (0 && loc[0] == '.' &&\n"
             "               (loc[1] == '/' ||\n"
             "                (loc[1] == '.' && (loc[2] == '/' || loc[2] == '\\0')))) {\n"),
]


def run(cmd, timeout):
    try:
        p = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return "timeout", ""


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", default=None,
                    help="run only mutants whose label contains this")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--no-fuzz", action="store_true",
                    help="run only the unit suite")
    ap.add_argument("--show-log", action="store_true")
    args = ap.parse_args()

    # Baseline must be green before mutating, or the report is meaningless.
    rc, out = run(["make", "tests/test_agent"], 300)
    if rc != 0:
        print("baseline build failed:\n%s" % out)
        return 2
    rc, out = run(["./tests/test_agent"], 300)
    if rc != 0:
        print("baseline unit suite failed; fix it first:\n%s" % out)
        return 2
    print("baseline: tests/test_agent green\n")

    mutants = [m for m in MUTANTS
               if args.only is None or args.only in m["label"]]
    killed = 0
    results = []

    for m in mutants:
        path = os.path.join(ROOT, m["file"])
        with open(path, "rb") as f:
            original = f.read()
        text = original.decode("utf-8")
        count = m.get("count", 1)
        if m["old"] not in text:
            results.append((m, "OLD-NOT-FOUND", ""))
            with open(path, "wb") as f:
                f.write(original)
            continue
        mutated = text.replace(m["old"], m["new"], count)
        try:
            with open(path, "w") as f:
                f.write(mutated)
            # Force a rebuild: the Makefile has no header dependency tracking.
            binpath = os.path.join(ROOT, "tests/test_agent")
            if os.path.exists(binpath):
                os.remove(binpath)
            rc, out = run(["make", "tests/test_agent"], 300)
            if rc != 0:
                verdict, detail = "KILLED", "build failed"
            else:
                gates = ["agent"] if args.no_fuzz else m["gates"]
                failed = None
                for g in gates:
                    grc, gout = run(GATES[g], 300)
                    if grc != 0:
                        failed = g
                        out = gout
                        break
                if failed:
                    verdict, detail = "KILLED", "%s failed" % failed
                else:
                    verdict, detail = "SURVIVED", "all gates passed"
        finally:
            with open(path, "wb") as f:
                f.write(original)
        if verdict == "KILLED":
            killed += 1
        results.append((m, verdict, detail))
        if args.show_log and out:
            print("  log tail: %s" % out.strip().splitlines()[-1][:200])

    print("%-42s %-9s %s" % ("mutant", "verdict", "detail"))
    print("-" * 72)
    for m, verdict, detail in results:
        print("%-42s %-9s %s" % (m["label"][:42], verdict, detail))
    total = len([r for r in results if r[1] != "OLD-NOT-FOUND"])
    print("-" * 72)
    print("mutation score: %d/%d killed" % (killed, total))
    return 0


if __name__ == "__main__":
    sys.exit(main())

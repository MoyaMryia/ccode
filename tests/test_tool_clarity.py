#!/usr/bin/env python3
"""Tool-result clarity regression test (docs/TOOL-CLARITY-AUDIT.md).

Drives ccode through a scripted plan of adversarial tool calls against an
embedded mock provider, records every request body, and asserts on the
serialized tool results exactly as the model would receive them. Each case
guards a fix from the 2026-10-05 clarity audit: error messages must name
the real cause, invalid regex must error (never silently fall back), and
misleading spellings accepted by one tool must be accepted by all.
"""

import atexit
import http.server
import json
import os
import shutil
import subprocess
import sys
import threading

CCODE = os.path.join(os.path.dirname(__file__), "..", "ccode-cli")
PORT = 0
TIMEOUT = 60

WS = None
DUMP = None

# id -> tool call; one step is emitted per completed tool result, in order.
PLAN = [
    {"id": "v01", "name": "str_replace_editor", "arguments":
        {"command": "view", "file_path": "./hello.txt"}},
    {"id": "v02", "name": "str_replace_editor", "arguments":
        {"command": "view", "file_path": "ABSWS/hello.txt"}},
    {"id": "v03", "name": "str_replace_editor", "arguments":
        {"command": "view", "file_path": "/etc/passwd"}},
    {"id": "v04", "name": "str_replace_editor", "arguments":
        {"command": "view", "file_path": "no_such_file_xyz.txt"}},
    {"id": "v05", "name": "str_replace_editor", "arguments":
        {"command": "view", "file_path": "subdir"}},
    {"id": "v06", "name": "str_replace_editor", "arguments":
        {"command": "view", "file_path": "hello.txt", "start_line": 0}},
    {"id": "v07", "name": "str_replace_editor", "arguments":
        {"command": "view", "file_path": "bin.dat"}},
    {"id": "g01", "name": "glob", "arguments":
        {"pattern": "(", "regex": True}},
    {"id": "g02", "name": "grep", "arguments":
        {"pattern": "(", "regex": True}},
    {"id": "g03", "name": "grep", "arguments":
        {"pattern": "clarityneedle"}},
    {"id": "d01", "name": "delete_file", "arguments":
        {"file_path": "ghost_xyz.txt"}},
    {"id": "d02", "name": "delete_file", "arguments":
        {"file_path": "subdir"}},
    {"id": "e01", "name": "str_replace_editor", "arguments":
        {"command": "str_replace", "file_path": "multi.txt",
         "old_string": "dup", "new_string": "x"}},
    {"id": "e02", "name": "str_replace_editor", "arguments":
        {"command": "str_replace", "file_path": "hello.txt",
         "old_string": "", "new_string": "clobber"}},
    {"id": "t01", "name": "task", "arguments":
        {"action": "update", "id": "999", "status": "bogus"}},
    {"id": "s01", "name": "web_search", "arguments":
        {"query": "template check"}},
    {"id": "w01", "name": "web_fetch", "arguments":
        {"url": "http://127.0.0.1:%d/nope404" % PORT}},
    {"id": "w02", "name": "web_fetch", "arguments":
        {"url": "http://127.0.0.1:%d/page" % PORT}},
    {"id": "v08", "name": "str_replace_editor", "arguments":
        {"command": "view", "file_path": "hello.txt", "byte_offset": 999999}},
    {"id": "v09", "name": "str_replace_editor", "arguments":
        {"command": "view", "file_path": "long.txt", "byte_offset": 300001}},
    {"id": "v10", "name": "str_replace_editor", "arguments":
        {"command": "view", "file_path": "utf8-long.txt"}},
    {"id": "v11", "name": "str_replace_editor", "arguments":
        {"command": "view", "file_path": "utf8-long.txt", "byte_offset": "$v10-next"}},
    {"id": "g04", "name": "glob", "arguments": {"pattern": ""}},
    {"id": "g05", "name": "glob", "arguments": {"pattern": "*.txt"}},
    {"id": "g06", "name": "glob", "arguments":
        {"pattern": "lib/*.c", "path": "src"}},
    {"id": "g07", "name": "glob", "arguments": {"pattern": "*.nothing"}},
    {"id": "p01", "name": "grep", "arguments":
        {"pattern": "clarity", "context": 101}},
    {"id": "p02", "name": "grep", "arguments":
        {"pattern": "clarity", "path": "hello.txt"}},
    {"id": "p03", "name": "grep", "arguments":
        {"pattern": "clarity", "path": "missing-search-root"}},
    {"id": "m01", "name": "move_file", "arguments":
        {"source": "missing-source.txt", "destination": "moved.txt"}},
    {"id": "m02", "name": "move_file", "arguments":
        {"source": "hello.txt", "destination": "missing-parent/moved.txt"}},
    {"id": "e03", "name": "str_replace_editor", "arguments":
        {"command": "str_replace", "file_path": "embedded-nul.txt",
         "old_string": "before", "new_string": "updated"}},
    {"id": "w03", "name": "web_fetch", "arguments":
        {"url": "http://127.0.0.1:%d/large" % PORT, "max_size": 1048576}},
    {"id": "a01", "name": "agent_tool", "arguments":
        {"task": "mock delegate true", "read_only": True}},
    {"id": "a02", "name": "agent_tool", "arguments":
        {"task": "mock delegate false", "read_only": False}},
    {"id": "a03", "name": "agent_tool", "arguments":
        {"task": "mock delegate legacy true", "read_only": "true"}},
    {"id": "a04", "name": "agent_tool", "arguments":
        {"task": "mock delegate legacy false", "read_only": "false"}},
    {"id": "a05", "name": "agent_tool", "arguments":
        {"task": "mock delegate http failure", "read_only": True}},
    {"id": "a06", "name": "agent_tool", "arguments":
        {"task": "mock delegate whitespace answer", "read_only": True}},
]


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith("/large"):
            payload = b"LLLL max_size parameter accepted"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        elif self.path.startswith("/page"):
            body = ("<!doctype html><html><head><title>T</title></head><body>"
                    "<h1>Mock &amp; Page</h1><p>MOCKPAGE-CONTENT alpha</p>"
                    "</body></html>")
            payload = body.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        else:
            self.send_error(404, "Not found")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        req = json.loads(self.rfile.read(n).decode("utf-8"))
        with open(DUMP, "a") as f:
            f.write(json.dumps(req) + "\n")
        messages = req.get("messages", [])
        # Match the actual delegate system prompt, not a tool call id or the
        # parent's prompt, then return a canned final answer without tools.
        is_delegate = any(m.get("role") == "system" and
                          "delegate sub-agent of ccode" in m.get("content", "")
                          for m in messages)
        tool_results = sum(1 for m in messages if m.get("role") == "tool")
        if is_delegate:
            task_text = next((m.get("content", "") for m in messages
                              if m.get("role") == "user"), "")
            if "http failure" in task_text:
                self.send_response(503)
                self.send_header("Content-Type", "application/json")
                payload = b'{"error":{"message":"mock upstream failure"}}'
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                return
            answer = "   " if "whitespace answer" in task_text else "MOCK-DELEGATE-ANSWER"
            events = [{"choices": [{"index": 0,
                                    "delta": {"content": answer},
                                    "finish_reason": "stop"}]}]
        elif tool_results >= len(PLAN):
            events = [{"choices": [{"index": 0,
                                    "delta": {"content": "CLARITY-COMPLETE"},
                                    "finish_reason": "stop"}]}]
        else:
            step = PLAN[tool_results]
            args = dict(step["arguments"])
            if args.get("byte_offset") == "$v10-next":
                prior = next((m.get("content", "") for m in req.get("messages", [])
                              if m.get("role") == "tool" and
                              m.get("tool_call_id") == "v10"), "")
                try:
                    args["byte_offset"] = json.loads(prior)["next_byte_offset"]
                except (ValueError, KeyError, TypeError):
                    args["byte_offset"] = 0
            # v02 carries an absolute path into the per-run workspace.
            for k, v in args.items():
                if isinstance(v, str) and v.startswith("ABSWS/"):
                    args[k] = os.path.join(WS, v[len("ABSWS/"):])
            events = [
                {"choices": [{"index": 0, "delta": {"tool_calls": [
                    {"index": 0, "id": step["id"], "type": "function",
                     "function": {"name": step["name"],
                                  "arguments": json.dumps(args)}}]},
                    "finish_reason": None}]},
                {"choices": [{"index": 0, "delta": {},
                              "finish_reason": "tool_calls"}]},
            ]
        sse = "".join("data: %s\n\n" % json.dumps(e) for e in events)
        sse += "data: [DONE]\n\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(sse.encode())))
        self.end_headers()
        self.wfile.write(sse.encode())


PASS = 0
FAIL = 0


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        print("  PASS: %s" % name)
        PASS += 1
    else:
        print("  FAIL: %s%s" % (name, (" -- " + detail) if detail else ""))
        FAIL += 1


def main():
    global WS, DUMP
    WS = "/tmp/ccode_clarity_ws_%d" % os.getpid()
    DUMP = os.path.join(WS, "requests.jsonl")
    shutil.rmtree(WS, ignore_errors=True)
    atexit.register(shutil.rmtree, WS, ignore_errors=True)
    os.makedirs(os.path.join(WS, "subdir"))
    with open(os.path.join(WS, "hello.txt"), "w") as f:
        f.write("clarity hello\n")
    with open(os.path.join(WS, "multi.txt"), "w") as f:
        f.write("alpha dup\nbeta\ngamma dup\n")
    with open(os.path.join(WS, "bin.dat"), "wb") as f:
        f.write(b"bin \xbe\xad\xff\xfe more \x80\x81\x82\x83 end\n")
    with open(os.path.join(WS, "needle.dat"), "wb") as f:
        f.write(b"clarityneedle \xde\xad\xbe\xff garbage\n")
    os.makedirs(os.path.join(WS, "src", "lib"))
    with open(os.path.join(WS, "src", "lib", "example.c"), "w") as f:
        f.write("int example;\n")
    nul_original = b"before " + b"a" * 207 + b"\x00after\n"
    with open(os.path.join(WS, "embedded-nul.txt"), "wb") as f:
        f.write(nul_original)
    with open(os.path.join(WS, "long.txt"), "wb") as f:
        f.write(b"a" * 400000 + b"\n")
    with open(os.path.join(WS, "utf8-long.txt"), "wb") as f:
        f.write(b"a" * 51199 + "😀".encode("utf-8") + b"b" * 1000 + b"\n")

    server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    PORT = server.server_address[1]
    for step in PLAN:
        for key, value in step["arguments"].items():
            if isinstance(value, str):
                step["arguments"][key] = value.replace("127.0.0.1:0", "127.0.0.1:%d" % PORT)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    env = os.environ.copy()
    env.update({
        "CCODE_API_BASE": "http://127.0.0.1:%d/v1" % PORT,
        "CCODE_API_KEY": "test-key",
        "CCODE_MODEL": "test-model",
        "CCODE_WORKSPACE": WS,
        "CCODE_WRITE_TOOLS": "1",
        "CCODE_REQUEST_TIMEOUT": "15",
        "CCODE_MAX_TURNS": "60",
        "CCODE_CONTEXT_TOKENS": "1000000",
        "CCODE_WEB_FETCH_ALLOW_PRIVATE": "1",
        # Invalid on purpose: no {query} marker. Must error, never fall
        # back to the live web.
        "CCODE_WEB_SEARCH_URL": "http://127.0.0.1:%d/search?q=%%s" % PORT,
    })
    try:
        proc = subprocess.run(
            [CCODE, "--auto-approve", "--prompt", "clarity probe"],
            env=env, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=TIMEOUT)
    finally:
        server.shutdown()
        server.server_close()

    reqs = [json.loads(l) for l in open(DUMP)]
    results = {}
    for m in reqs[-1]["messages"]:
        if m.get("role") == "tool":
            results[m["tool_call_id"]] = m.get("content", "")
    check("process returned success", proc.returncode == 0,
          "returncode=%d stderr=%s" % (proc.returncode, proc.stderr.decode(errors="replace")[-300:]))
    check("plan completed", len(results) == len(PLAN),
          "got %d/%d results" % (len(results), len(PLAN)))
    delegate_requests = [r for r in reqs if any(
        m.get("role") == "system" and
        "delegate sub-agent of ccode" in m.get("content", "")
        for m in r.get("messages", []))]

    def has(tid, *subs):
        c = results.get(tid, "")
        return all(s in c for s in subs)

    def hasnt(tid, *subs):
        c = results.get(tid, "")
        return all(s not in c for s in subs)

    # C1: the ./ spelling that grep output teaches must be accepted.
    check("view accepts ./ prefix", has("v01", "clarity hello"))
    # C2: absolute paths inside the workspace are normalized, not refused.
    check("view accepts absolute under workspace", has("v02", "clarity hello"))
    # C3: distinct causes get distinct messages.
    check("absolute outside says outside", has("v03", "Path outside workspace"))
    check("missing says not found", has("v04", "File not found"))
    check("directory says directory", has("v05", "Is a directory"))
    # C4: argument errors are concise (no embedded JSON schema dump).
    check("bad line range is specific", has("v06", "start_line"))
    check("bad line range has no schema dump",
          hasnt("v06", '"properties"') and len(results.get("v06", "x" * 999)) < 400,
          "len=%d" % len(results.get("v06", "")))
    # C7: mostly-broken-UTF-8 files are binary, not mojibake.
    check("view rejects binary utf8", has("v07", "Binary"))
    # C5/C6: invalid regex errors instead of silently lying.
    check("glob invalid regex errors", has("g01", "Invalid regular expression"))
    check("grep invalid regex errors", has("g02", "Invalid regular expression"))
    # C7: grep skips binary files (no mojibake match line).
    check("grep skips binary file", hasnt("g03", "needle.dat", "\\ufffd"))
    # M6: delete names the cause.
    check("delete missing says not found", has("d01", "File not found"))
    check("delete directory says directory", has("d02", "Is a directory"))
    # L3: multiple matches carry count and line numbers.
    check("multiple matches has count+lines",
          has("e01", "Multiple matches found", '"count":2', '"lines":[1,3]'))
    # L1: create-over-existing is definitive.
    check("create existing is definitive", has("e02", "already exists"))
    # t01 uses a bogus id AND bogus status: either error is acceptable,
    # but an Invalid status error must list the valid statuses.
    c = results.get("t01", "")
    check("task errors name valid statuses",
          "Task not found" in c or "pending" in c, c[:120])
    # C8: bad search template errors instead of hitting the live web.
    check("bad search template errors", has("s01", "{query}"))
    # M3: HTTP errors are concise, no error-page HTML in the error field.
    check("fetch 404 is concise",
          has("w01", "HTTP 404", '"status":404') and hasnt("w01", "<html"),
          results.get("w01", "")[:120])
    # M4: block-level elements are word-separated after HTML stripping.
    check("html blocks separated",
          has("w02", "Mock & Page MOCKPAGE-CONTENT"),
          results.get("w02", "")[:160])

    check("byte_offset beyond EOF is specific", has("v08", "byte_offset is beyond end of file"))
    check("byte_offset above 300000 accepted",
          "error" not in results.get("v09", "") and
          "a" * 100 in results.get("v09", ""))
    check("empty glob rejected specifically", has("g04", "glob pattern must not be empty"))
    check("glob empty result is an empty match list", "[]" in results.get("g07", "") or
          '"matches":[]' in results.get("g07", ""), results.get("g07", "")[:120])
    check("glob pattern is relative to search root",
          has("g06", "src/lib/example.c"), results.get("g06", "")[:120])
    check("grep context bound is clear", has("p01", "context", "0", "100"))
    check("grep file search root says not a directory",
          has("p02", "Not a directory", "hello.txt") and
          hasnt("p02", "not found"), results.get("p02", "")[:120])
    check("grep missing search root is clear", has("p03", "not found"))
    check("move missing source is clear", has("m01", "not found"))
    check("move missing destination parent is clear", has("m02", "directory") or has("m02", "not found"))
    check("embedded NUL edit rejected", has("e03", "NUL") or has("e03", "null"),
          results.get("e03", "")[:120])
    check("NUL edit left file bytes unchanged",
          open(os.path.join(WS, "embedded-nul.txt"), "rb").read() == nul_original)
    check("web_fetch accepts max_size 1048576", "error" not in results.get("w03", "") and
          "LLLL" in results.get("w03", ""), results.get("w03", "")[:120])
    check("boolean and legacy read_only values accepted",
          all("MOCK-DELEGATE-ANSWER" in results.get(tid, "")
              for tid in ("a01", "a02", "a03", "a04")),
          " | ".join(results.get(tid, "")[:100] for tid in ("a01", "a02", "a03", "a04")))
    check("delegate provider error retains HTTP root cause",
          "503" in results.get("a05", "") or "mock upstream failure" in results.get("a05", ""),
          results.get("a05", "")[:200])
    check("whitespace-only delegate answer is treated as empty",
          "no answer" in results.get("a06", "").lower(), results.get("a06", "")[:160])
    check("delegate detection uses actual system message", len(delegate_requests) == 6,
          "matched %d delegate requests" % len(delegate_requests))

    # Compare the actual UTF-8 bytes emitted at the continuation seam, not a
    # replacement-character rendering. Continuation offset is supplied from
    # v10's returned next_byte_offset by the mock provider.
    try:
        first = json.loads(results.get("v10", ""))
        second = json.loads(results.get("v11", ""))
        first_bytes = first.get("content", "").encode("utf-8")
        second_bytes = second.get("content", "").encode("utf-8")
        source = open(os.path.join(WS, "utf8-long.txt"), "rb").read()
        offset = first["next_byte_offset"]
        boundary_ok = (first.get("truncated") is True and
                       first_bytes == source[:offset] and
                       first_bytes + second_bytes == source)
    except (ValueError, KeyError, TypeError):
        boundary_ok = False
    check("UTF-8 continuation preserves exact bytes at boundary", boundary_ok)

    shutil.rmtree(WS, ignore_errors=True)
    print("=== Results: %d passed, %d failed ===" % (PASS, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())

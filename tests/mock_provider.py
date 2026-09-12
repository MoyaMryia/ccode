#!/usr/bin/env python3
"""OpenAI-compatible mock provider for ccode integration tests."""

import http.server
import json
import sys
import os
import socket
import struct
import time
import threading

RESPONSES = {}

# Timestamps of in-flight sub-agent requests (parallel-subagents fixture).
_SUBAGENT_REQ_TIMES = []
_SUBAGENT_REQ_LOCK = threading.Lock()

def chunk_content_events(content, chunk=4096):
    """Emit content as a stream of small deltas, like a real streaming
    provider. ccode's SSE parser holds at most IO_BUF_SIZE (32 KiB) per
    event line, so any answer larger than that must be split across events."""
    out = []
    for i in range(0, len(content), chunk):
        out.append({"data": json.dumps({
            "choices": [{"index": 0,
                         "delta": {"content": content[i:i + chunk]},
                         "finish_reason": None}]
        })})
    out.append({"data": json.dumps({
        "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]
    })})
    return out

def load_responses(fixtures_dir):
    for fname in os.listdir(fixtures_dir):
        if fname.endswith('.json'):
            path = os.path.join(fixtures_dir, fname)
            with open(path) as f:
                data = json.load(f)
            test_name = fname.replace('.json', '')
            RESPONSES[test_name] = data


def build_sse_response(events):
    """Build an SSE response body from a list of event dicts.

    Each event dict has:
      - data: string (the JSON data line content)
      - delay: float (seconds to wait before sending, optional)
    """
    lines = []
    for ev in events:
        lines.append("data: {}\n\n".format(ev["data"]))
    lines.append("data: [DONE]\n\n")
    return "".join(lines)


def build_chunked_body(sse_body, chunk_size=1, chunk_ext=None):
    """Encode an arbitrary body using HTTP chunk framing."""
    chunks = []
    pos = 0
    while pos < len(sse_body):
        end = min(pos + chunk_size, len(sse_body))
        chunk_data = sse_body[pos:end]
        ext = ";{}".format(chunk_ext) if chunk_ext else ""
        chunks.append("{:x}{}\r\n{}\r\n".format(len(chunk_data), ext, chunk_data))
        pos = end
    chunks.append("0\r\nX-Mock-Trailer: accepted\r\n\r\n")
    return "".join(chunks)


def build_chunked_response(events, chunk_size=1, chunk_ext=None):
    """Build a chunked HTTP response body."""
    return build_chunked_body(build_sse_response(events), chunk_size, chunk_ext)


class MockHandler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path == "/reset/chat/completions":
            self.connection.setsockopt(
                socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            self.connection.close()
            self.close_connection = True
            return

        content_len = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_len).decode("utf-8") if content_len else ""

        # Validate basic request shape
        if self.path != "/v1/chat/completions":
            self.send_error(404, "Not found")
            return

        auth = self.headers.get("Authorization", "")
        if not auth.startswith("Bearer "):
            self.send_error(401, "Missing or invalid Authorization header")
            return

        try:
            req = json.loads(body)
        except json.JSONDecodeError:
            self.send_error(400, "Invalid JSON body")
            return

        test_mode = self.headers.get("X-Test-Mode", "normal")
        if test_mode == "normal" or test_mode == "chunked":
            # Fixture routing keys off the magic __ccode_test_ prefix. Use
            # the LAST prefixed user message: in multi-turn sessions the
            # first user message is turn 1's, which would pin the whole
            # conversation to that turn's fixture.
            for msg in req.get("messages", []):
                if (msg.get("role") == "user" and
                        isinstance(msg.get("content"), str) and
                        msg["content"].startswith("__ccode_test_")):
                    test_mode = msg["content"][len("__ccode_test_"):].split(" ")[0]
        test_chunked = self.headers.get("X-Test-Chunked", "").lower() == "true"
        test_chunk_size = int(self.headers.get("X-Test-Chunk-Size", "1"))
        test_chunk_ext = self.headers.get("X-Test-Chunk-Ext", "")

        events = []

        if test_mode == "normal" or test_mode == "chunked":
            prompt = ""
            if req.get("messages"):
                for msg in req["messages"]:
                    if msg.get("role") == "user":
                        prompt = msg.get("content", "")
            response_text = "You said: {}".format(prompt)
            events = [
                {"data": json.dumps({
                    "id": "mock-1",
                    "object": "chat.completion.chunk",
                    "choices": [{
                        "index": 0,
                        "delta": {"content": response_text},
                        "finish_reason": None
                    }]
                })},
                {"data": json.dumps({
                    "id": "mock-2",
                    "object": "chat.completion.chunk",
                    "choices": [{
                        "index": 0,
                        "delta": {},
                        "finish_reason": "stop"
                    }]
                })},
            ]

        elif test_mode == "fragmented-headers":
            # Send headers in two parts (simulated by slow response)
            events = [
                {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "Hello"}, "finish_reason": None}]
                })},
                {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]
                })},
            ]

        elif test_mode == "fragmented-sse":
            content = "Hello world"
            events = [
                {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": content[i:i+2]}, "finish_reason": None}]
                })}
                for i in range(0, len(content), 2)
            ]
            events.append({
                "data": json.dumps({
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]
                })
            })

        elif test_mode == "stream-delayed":
            first = "data: {}\n\n".format(json.dumps({
                "choices": [{"index": 0, "delta": {"content": "first"},
                             "finish_reason": None}]
            }))
            second = "data: {}\n\n".format(json.dumps({
                "choices": [{"index": 0, "delta": {"content": " second"},
                             "finish_reason": "stop"}]
            })) + "data: [DONE]\n\n"
            body = first + second
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(body.encode("utf-8"))))
            self.end_headers()
            self.wfile.write(first.encode("utf-8"))
            self.wfile.flush()
            time.sleep(1.0)
            self.wfile.write(second.encode("utf-8"))
            self.wfile.flush()
            return

        elif test_mode == "non-200":
            err_body = json.dumps({
                "error": {
                    "message": "Bad request",
                    "type": "invalid_request_error",
                    "code": "bad_request"
                }
            })
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(err_body.encode("utf-8"))))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(err_body.encode("utf-8"))
            self.close_connection = True
            return

        elif test_mode == "non-200-controls":
            err_body = json.dumps({
                "error": {
                    "message": "Bad\x1b[31m\nmessage\u202e",
                    "type": "invalid_request_error",
                }
            })
            raw = ("HTTP/1.1 400 Bad Request\r\n"
                   "Content-Type: application/json\r\n"
                   "Content-Length: {}\r\nConnection: close\r\n\r\n".format(
                       len(err_body.encode("utf-8"))))
            self.connection.sendall(raw.encode("utf-8") + err_body.encode("utf-8"))
            self.close_connection = True
            return

        elif test_mode == "tool-calls":
            has_tool_result = any(msg.get("role") == "tool"
                                  for msg in req.get("messages", []))
            if has_tool_result:
                events = [
                    {"data": json.dumps({
                        "choices": [{
                            "index": 0,
                            "delta": {"content": "Tool result received."},
                            "finish_reason": "stop"
                        }]
                    })}
                ]
            else:
                events = [
                    {"data": json.dumps({
                        "choices": [{
                            "index": 0,
                            "delta": {"content": "Let me check..."},
                            "finish_reason": None
                        }]
                    })},
                    {"data": json.dumps({
                        "choices": [{
                            "index": 0,
                            "delta": {
                                "tool_calls": [{
                                    "index": 0,
                                    "id": "call_abc123",
                                    "type": "function",
                                    "function": {
                                        "name": "read_file",
                                        "arguments": '{"file_path":"test.txt"}'
                                    }
                                }]
                            },
                            "finish_reason": None
                        }]
                    })},
                    {"data": json.dumps({
                        "choices": [{
                            "index": 0,
                            "delta": {},
                            "finish_reason": "tool_calls"
                        }]
                    })},
                ]

        elif test_mode == "thinking-tools":
            # DeepSeek thinking mode with tools: every historical assistant
            # message MUST carry reasoning_content or the API returns 400.
            thinking_on = (isinstance(req.get("thinking"), dict) and
                           req["thinking"].get("type") == "enabled")
            has_tools = bool(req.get("tools"))
            if thinking_on and has_tools:
                for msg in req.get("messages", []):
                    if (msg.get("role") == "assistant" and
                            not isinstance(msg.get("reasoning_content"), str)):
                        err_body = json.dumps({"error": {
                            "message": "reasoning_content is required in the "
                                       "assistant message when tools are used",
                            "type": "invalid_request_error",
                            "code": "invalid_request_error"}})
                        self.send_response(400)
                        self.send_header("Content-Type", "application/json")
                        self.send_header("Content-Length",
                                         str(len(err_body.encode("utf-8"))))
                        self.send_header("Connection", "close")
                        self.end_headers()
                        self.wfile.write(err_body.encode("utf-8"))
                        self.close_connection = True
                        return
            has_tool_result = any(msg.get("role") == "tool"
                                  for msg in req.get("messages", []))
            if has_tool_result:
                events = [
                    {"data": json.dumps({"choices": [{"index": 0, "delta": {
                        "reasoning_content": "The tool returned; answer now."},
                        "finish_reason": None}]})},
                    {"data": json.dumps({"choices": [{"index": 0, "delta": {
                        "content": "thinking-tools done"},
                        "finish_reason": "stop"}]})},
                ]
            else:
                events = [
                    {"data": json.dumps({"choices": [{"index": 0, "delta": {
                        "reasoning_content": "I should inspect the file."},
                        "finish_reason": None}]})},
                    {"data": json.dumps({"choices": [{"index": 0, "delta": {
                        "tool_calls": [{
                            "index": 0, "id": "call_think1",
                            "type": "function",
                            "function": {"name": "read_file",
                                         "arguments": '{"file_path":"test.txt"}'}}]},
                        "finish_reason": None}]})},
                    {"data": json.dumps({"choices": [{"index": 0, "delta": {},
                        "finish_reason": "tool_calls"}]})},
                ]

        elif test_mode == "tool-calls-write":
            has_tool_result = any(msg.get("role") == "tool"
                                  for msg in req.get("messages", []))
            if has_tool_result:
                events = [
                    {"data": json.dumps({
                        "choices": [{
                            "index": 0,
                            "delta": {"content": "Tool result received."},
                            "finish_reason": "stop"
                        }]
                    })}
                ]
            else:
                events = [{"data": json.dumps({
                    "choices": [{
                        "index": 0,
                        "delta": {"tool_calls": [{
                            "index": 0,
                            "id": "call_write1",
                            "type": "function",
                            "function": {
                                "name": "write_file",
                                "arguments": '{"file_path":"test.txt","content":"x"}'
                            }
                        }]},
                        "finish_reason": "tool_calls"
                    }]
                })}]

        elif test_mode == "write-calls":
            has_tool_result = any(msg.get("role") == "tool"
                                  for msg in req.get("messages", []))
            if has_tool_result:
                events = [{"data": json.dumps({
                    "choices": [{"index": 0,
                                 "delta": {"content": "Write result received."},
                                 "finish_reason": "stop"}]
                })}]
            else:
                events = [{"data": json.dumps({
                    "choices": [{
                        "index": 0,
                        "delta": {"tool_calls": [{
                            "index": 0,
                            "id": "call_write123",
                            "type": "function",
                            "function": {
                                "name": "write_file",
                                "arguments": '{"file_path":"integration-write.txt","content":"written by mock\\n"}'
                            }
                        }]},
                        "finish_reason": "tool_calls"
                    }]
                })}]

        elif test_mode == "deny-no-side-effects":
            msgs = req.get("messages", [])
            # Count tool results produced by THIS turn only: a session
            # chain carries earlier turns' tool results, which must not
            # advance this fixture's script.
            last_user = len(msgs) - 1
            while last_user >= 0 and msgs[last_user].get("role") != "user":
                last_user -= 1
            tool_count = sum(1 for m in msgs[last_user + 1:]
                             if m.get("role") == "tool")
            has_tool_result = tool_count > 0
            if has_tool_result and tool_count >= 2:
                events = [{"data": json.dumps({
                    "choices": [{"index": 0,
                                 "delta": {"content": "All tool requests denied."},
                                 "finish_reason": "stop"}]
                })}]
            elif has_tool_result and tool_count == 1:
                events = [{"data": json.dumps({
                    "choices": [{
                        "index": 0,
                        "delta": {"tool_calls": [{
                            "index": 0,
                            "id": "call_cmd1",
                            "type": "function",
                            "function": {
                                "name": "bash",
                                "arguments": '{"command":"touch must_not_exist_marker.txt"}'
                            }
                        }]},
                        "finish_reason": "tool_calls"
                    }]
                })}]
            else:
                events = [{"data": json.dumps({
                    "choices": [{
                        "index": 0,
                        "delta": {"tool_calls": [{
                            "index": 0,
                            "id": "call_write1",
                            "type": "function",
                            "function": {
                                "name": "write_file",
                                "arguments": '{"file_path":"must_not_exist.txt","content":"evil\\n"}'
                            }
                        }]},
                        "finish_reason": "tool_calls"
                    }]
                })}]

        elif test_mode == "cancel-command-fixture":
            has_tool_result = any(msg.get("role") == "tool"
                                   for msg in req.get("messages", []))
            if has_tool_result:
                events = [{"data": json.dumps({
                    "choices": [{"index": 0,
                                 "delta": {"content": "Cancelled."},
                                 "finish_reason": "stop"}]
                })}]
            else:
                events = [{"data": json.dumps({
                    "choices": [{
                        "index": 0,
                        "delta": {"tool_calls": [{
                            "index": 0,
                            "id": "call_sleep",
                            "type": "function",
                            "function": {
                                "name": "bash",
                                "arguments": "{\"command\":\"python3 -c 'import time; time.sleep(30); open(\\\"cancel_marker.txt\\\",\\\"w\\\").close()'\"}"
                            }
                        }]},
                        "finish_reason": "tool_calls"
                    }]
                })}]

        elif test_mode == "repair-loop-fixture":
            tool_count = sum(1 for m in req.get("messages", [])
                             if m.get("role") == "tool")
            if tool_count == 0:
                events = [{"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "Inspecting the code..."}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_inspect1", "type": "function", "function": {"name": "read_file", "arguments": '{"file_path":"src/main.c"}'}}]}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]
                })}]
            elif tool_count == 1:
                events = [{"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "First edit..."}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_edit1", "type": "function", "function": {"name": "edit_file", "arguments": '{"file_path":"src/main.c","old_string":"int add(int a, int b) { return a - b; }","new_string":"int add(int a, int b) { return a + b; }"}'}}]}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]
                })}]
            elif tool_count == 2:
                events = [{"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "Verifying the focused test..."}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_test1", "type": "function", "function": {"name": "bash", "arguments": "{\"command\":\"grep -q 'sub.*return a - b' src/main.c\"}"}}]}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]
                })}]
            elif tool_count == 3:
                events = [{"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "Focused test failed; re-inspecting..."}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_inspect2", "type": "function", "function": {"name": "read_file", "arguments": '{"file_path":"src/main.c"}'}}]}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]
                })}]
            elif tool_count == 4:
                events = [{"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "Second edit..."}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_edit2", "type": "function", "function": {"name": "edit_file", "arguments": '{"file_path":"src/main.c","old_string":"int sub(int a, int b) { return a + b; }","new_string":"int sub(int a, int b) { return a - b; }"}'}}]}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]
                })}]
            elif tool_count == 5:
                events = [{"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "Re-verifying..."}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_test2", "type": "function", "function": {"name": "bash", "arguments": "{\"command\":\"grep -q 'sub.*return a - b' src/main.c\"}"}}]}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]
                })}]
            elif tool_count == 6:
                events = [{"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "Showing diff..."}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_diff", "type": "function", "function": {"name": "bash", "arguments": "{\"command\":\"git --no-pager diff\"}"}}]}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]
                })}]
            else:
                events = [{"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "Both defects repaired. Final report: add() now returns a + b and sub() returns a - b."}, "finish_reason": "stop"}]
                })}]

        elif test_mode == "workflow-fixture":
            msgs = req.get("messages", [])
            tool_count = sum(1 for m in msgs if m.get("role") == "tool")
            if tool_count == 0:
                # Turn 1: emit read_file for src/main.c
                events = [{"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "Let me inspect the code..."}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_inspect", "type": "function", "function": {"name": "read_file", "arguments": '{"file_path":"src/main.c"}'}}]}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]
                })}]
            elif tool_count == 1:
                # Turn 2: emit edit_file to fix the bug
                events = [{"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "I see the bug. Let me fix it..."}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_edit1", "type": "function", "function": {"name": "edit_file", "arguments": '{"file_path":"src/main.c","old_string":"a - b","new_string":"a + b"}'}}]}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]
                })}]
            elif tool_count == 2:
                # Turn 3: emit bash to verify
                events = [{"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "Now let me verify the fix..."}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_test", "type": "function", "function": {"name": "bash", "arguments": '{"command":"echo fix verified"}'}}]}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]
                })}]
            elif tool_count == 3:
                # Turn 4: emit bash git diff to show changes
                events = [{"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "Changes made. Showing diff..."}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_diff", "type": "function", "function": {"name": "bash", "arguments": "{\"command\":\"git --no-pager diff\"}"}}]}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]
                })}]
            else:
                # Turn 5: done
                events = [{"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "Fix complete."}, "finish_reason": "stop"}]
                })}]

        elif test_mode == "parallel-subagents-fixture":
            msgs = req.get("messages", [])
            tool_count = sum(1 for m in msgs if m.get("role") == "tool")
            prompt = ""
            for m in msgs:
                if m.get("role") == "user" and isinstance(m.get("content"), str):
                    prompt = m.get("content", "")
                    break
            if " sub-a-delegate" in prompt or " sub-b-delegate" in prompt:
                # A sub-agent's own request: answer after a delay so the
                # parent can prove the two delegates ran concurrently.
                now = time.time()
                with _SUBAGENT_REQ_LOCK:
                    overlapping = any(now - t < 1.0
                                      for t in _SUBAGENT_REQ_TIMES)
                    _SUBAGENT_REQ_TIMES.append(now)
                padding = int(os.environ.get("MOCK_SUBAGENT_PADDING", "0"))
                content = "answer:{};overlap:{}".format(
                    prompt, "yes" if overlapping else "no")
                if padding > 0 and "sub-a-delegate" in prompt:
                    content += "p" * padding
                events = chunk_content_events(content)
                time.sleep(1.0)
            elif tool_count == 0:
                # Parent turn 1: emit two read-only agent_tool calls.
                events = [{"data": json.dumps({
                    "choices": [{"index": 0,
                                 "delta": {"content": "Delegating in parallel..."},
                                 "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0,
                                 "delta": {"tool_calls": [
                                     {"index": 0, "id": "call_sub1",
                                      "type": "function",
                                      "function": {"name": "agent_tool",
                                                   "arguments": '{"task":"__ccode_test_parallel-subagents-fixture sub-a-delegate"}'}},
                                     {"index": 1, "id": "call_sub2",
                                      "type": "function",
                                      "function": {"name": "agent_tool",
                                                   "arguments": '{"task":"__ccode_test_parallel-subagents-fixture sub-b-delegate"}'}}
                                 ]},
                                 "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {},
                                 "finish_reason": "tool_calls"}]
                })}]
            else:
                # Parent turn 2: echo what the delegates returned.
                parts = []
                for m in msgs:
                    if m.get("role") == "tool":
                        parts.append(str(m.get("content", "")))
                content = "Parallel results: " + " | ".join(parts)
                events = chunk_content_events(content)

        elif test_mode == "subagent-reads-fixture":
            # A read-only delegate must run its own read tool without asking
            # the user for approval: the forked child shares the parent's
            # terminal, so a prompt would stop it with SIGTTIN (its process
            # group is not the foreground group) and deadlock the parent.
            msgs = req.get("messages", [])
            prompt = ""
            for m in msgs:
                if m.get("role") == "user" and isinstance(m.get("content"), str):
                    prompt = m.get("content", "")
                    break
            has_tool_result = any(m.get("role") == "tool" for m in msgs)
            if " sub-read-delegate" in prompt:
                if has_tool_result:
                    events = [{"data": json.dumps({
                        "choices": [{"index": 0,
                                     "delta": {"content": "sub-read-done"},
                                     "finish_reason": "stop"}]})}]
                else:
                    events = [{"data": json.dumps({
                        "choices": [{"index": 0,
                                     "delta": {"tool_calls": [
                                         {"index": 0, "id": "call_subread",
                                          "type": "function",
                                          "function": {"name": "read_file",
                                                       "arguments": '{"file_path":"probe.txt"}'}}]},
                                     "finish_reason": None}]})},
                              {"data": json.dumps({
                                  "choices": [{"index": 0, "delta": {},
                                               "finish_reason": "tool_calls"}]})}]
            elif has_tool_result:
                parts = [str(m.get("content", "")) for m in msgs
                         if m.get("role") == "tool"]
                events = chunk_content_events(
                    "parent-saw: " + " | ".join(parts))
            else:
                events = [{"data": json.dumps({
                    "choices": [{"index": 0,
                                 "delta": {"tool_calls": [
                                     {"index": 0, "id": "call_subread1",
                                      "type": "function",
                                      "function": {"name": "agent_tool",
                                                   "arguments": '{"task":"__ccode_test_subagent-reads-fixture sub-read-delegate"}'}}]},
                                 "finish_reason": None}]})},
                          {"data": json.dumps({
                              "choices": [{"index": 0, "delta": {},
                                           "finish_reason": "tool_calls"}]})}]

        elif test_mode == "reasoning-newlines":
            # Chain-of-thought must render real newlines/tabs, not the
            # escaped "\n" form used for single-line safety rendering.
            events = [
                {"data": json.dumps({
                    "choices": [{"index": 0,
                                 "delta": {"reasoning_content":
                                           "think line one\nthink\tline two"},
                                 "finish_reason": None}]
                })},
                {"data": json.dumps({
                    "choices": [{"index": 0,
                                 "delta": {"content": "final answer"},
                                 "finish_reason": None}]
                })},
                {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]
                })},
            ]

        elif test_mode == "tui-context":
            # Context-inheritance probe: turn 1 introduces a name, turn 2
            # asks for it back. The answer is correct only when the request
            # carries the first turn's user message.
            user_msgs = [m.get("content", "") for m in req.get("messages", [])
                         if m.get("role") == "user" and
                         isinstance(m.get("content"), str)]
            prior = user_msgs[:-1] if user_msgs else []
            answer = ("Your name is Alice." if any("Alice" in u for u in prior)
                      else "Nice to meet you.")
            events = [{"data": json.dumps({
                "choices": [{"index": 0, "delta": {"content": answer},
                             "finish_reason": "stop"}]})}]

        elif test_mode == "tui-error-then-ok":
            # Provider error followed by recovery: the prefixed prompt gets
            # a 400; every later unprompted turn echoes normally, proving
            # the frontend survived the error.
            msgs = req.get("messages", [])
            user_msgs = [m.get("content", "") for m in msgs
                         if m.get("role") == "user" and
                         isinstance(m.get("content"), str)]
            last = user_msgs[-1] if user_msgs else ""
            if last.startswith("__ccode_test_"):
                err_body = json.dumps({"error": {"message": "Bad request"}})
                self.send_response(400)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length",
                                 str(len(err_body.encode())))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(err_body.encode())
                self.close_connection = True
                return
            events = [{"data": json.dumps({
                "choices": [{"index": 0,
                             "delta": {"content": "You said: {}".format(last)},
                             "finish_reason": "stop"}]})}]

        elif test_mode == "tui-stream-delta":
            # Several content deltas so a TUI frontend must append each
            # fragment to the streaming message instead of replacing it.
            # The first delta is written immediately, the rest after a pause,
            # so the TUI redraws at least twice mid-stream.
            first = json.dumps({
                "choices": [{"index": 0, "delta": {"content": "You "},
                             "finish_reason": None}]})
            rest = [
                {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "said: "},
                                 "finish_reason": None}]})},
                {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "hello world"},
                                 "finish_reason": None}]})},
                {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {},
                                 "finish_reason": "stop"}]})},
            ]
            body_first = "data: {}\n\n".format(first)
            body_rest = build_sse_response(rest)
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length",
                             str(len(body_first.encode()) +
                                 len(body_rest.encode())))
            self.end_headers()
            self.wfile.write(body_first.encode())
            self.wfile.flush()
            time.sleep(0.4)
            self.wfile.write(body_rest.encode())
            self.wfile.flush()
            return

        elif test_mode == "tui-markdown":
            # Multi-line assistant answer with a fenced code block and bold
            # run: the TUI must keep the code fence open across rendered
            # lines and strip the markdown markers from the display.
            content = ("Plan:\n\n```c\nint add(int a, int b) {\n"
                       "    return a + b;\n}\n```\n\nDone: **added safely**")
            events = [
                {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": content},
                                 "finish_reason": None}]})},
                {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {},
                                 "finish_reason": "stop"}]})},
            ]

        elif test_mode == "result-spill-fixture":
            # Turn 1: run a command whose stdout far exceeds the 64 KiB inline
            # preview. Turn 2: the model must notice stdout_truncated and pull
            # the archived tail with read_tool_output. Turn 3: verify the tail
            # marker survived the archive/read round-trip.
            msgs = req.get("messages", [])
            tool_msgs = [m for m in msgs if m.get("role") == "tool"]
            read_results = [m for m in tool_msgs
                            if "returned_bytes" in str(m.get("content", ""))]
            if not tool_msgs:
                events = [
                    {"data": json.dumps({"choices": [{"index": 0, "delta": {
                        "content": "Generating a huge output..."},
                        "finish_reason": None}]})},
                    {"data": json.dumps({"choices": [{"index": 0, "delta": {
                        "tool_calls": [{
                            "index": 0, "id": "call_spill1", "type": "function",
                            "function": {
                                "name": "bash",
                                "arguments": json.dumps({"command":
                                    "printf START; yes A | head -c 200000; "
                                    "printf ENDMARK9999"})}}]},
                        "finish_reason": None}]})},
                    {"data": json.dumps({"choices": [{"index": 0, "delta": {},
                        "finish_reason": "tool_calls"}]})},
                ]
            elif not read_results:
                tcid = None
                for m in tool_msgs:
                    if "stdout_truncated" in str(m.get("content", "")):
                        tcid = m.get("tool_call_id")
                if tcid is None:
                    events = [{"data": json.dumps({"choices": [{
                        "index": 0, "delta": {"content": "RESULT_NOT_TRUNCATED"},
                        "finish_reason": "stop"}]})}]
                else:
                    events = [
                        {"data": json.dumps({"choices": [{"index": 0, "delta": {
                            "content": "Pulling the archived tail..."},
                            "finish_reason": None}]})},
                        {"data": json.dumps({"choices": [{"index": 0, "delta": {
                            "tool_calls": [{
                                "index": 0, "id": "call_read1",
                                "type": "function",
                                "function": {
                                    "name": "read_tool_output",
                                    "arguments": json.dumps({
                                        "tool_call_id": tcid,
                                        "offset": 199000,
                                        "limit": 65536})}}]},
                            "finish_reason": None}]})},
                        {"data": json.dumps({"choices": [{"index": 0, "delta": {},
                            "finish_reason": "tool_calls"}]})},
                    ]
            else:
                got = str(read_results[-1].get("content", ""))
                marker = "ENDMARK9999" if "ENDMARK9999" in got else "TAIL_MISSING"
                events = [{"data": json.dumps({"choices": [{"index": 0, "delta": {
                    "content": "spill " + marker},
                    "finish_reason": "stop"}]})}]

        elif test_mode == "result-spill-stderr":
            # Same as result-spill-fixture but the huge stream is stderr, so
            # only the stderr archive should exist and stream=stderr must
            # select it.
            msgs = req.get("messages", [])
            tool_msgs = [m for m in msgs if m.get("role") == "tool"]
            read_results = [m for m in tool_msgs
                            if "returned_bytes" in str(m.get("content", ""))]
            if not tool_msgs:
                events = [
                    {"data": json.dumps({"choices": [{"index": 0, "delta": {
                        "content": "Generating a huge stderr..."},
                        "finish_reason": None}]})},
                    {"data": json.dumps({"choices": [{"index": 0, "delta": {
                        "tool_calls": [{
                            "index": 0, "id": "call_spillerr", "type": "function",
                            "function": {
                                "name": "bash",
                                "arguments": json.dumps({"command":
                                    "printf OUT; yes E | head -c 200000 1>&2; "
                                    "printf ENDERR2 1>&2"})}}]},
                        "finish_reason": None}]})},
                    {"data": json.dumps({"choices": [{"index": 0, "delta": {},
                        "finish_reason": "tool_calls"}]})},
                ]
            elif not read_results:
                tcid = None
                for m in tool_msgs:
                    if "stderr_truncated" in str(m.get("content", "")):
                        tcid = m.get("tool_call_id")
                if tcid is None:
                    events = [{"data": json.dumps({"choices": [{
                        "index": 0, "delta": {"content": "STDERR_NOT_TRUNCATED"},
                        "finish_reason": "stop"}]})}]
                else:
                    events = [
                        {"data": json.dumps({"choices": [{"index": 0, "delta": {
                            "content": "Pulling archived stderr..."},
                            "finish_reason": None}]})},
                        {"data": json.dumps({"choices": [{"index": 0, "delta": {
                            "tool_calls": [{
                                "index": 0, "id": "call_readerr",
                                "type": "function",
                                "function": {
                                    "name": "read_tool_output",
                                    "arguments": json.dumps({
                                        "tool_call_id": tcid,
                                        "stream": "stderr",
                                        "offset": 199000,
                                        "limit": 65536})}}]},
                            "finish_reason": None}]})},
                        {"data": json.dumps({"choices": [{"index": 0, "delta": {},
                            "finish_reason": "tool_calls"}]})},
                    ]
            else:
                got = str(read_results[-1].get("content", ""))
                marker = "ENDERR2" if "ENDERR2" in got else "STDERR_TAIL_MISSING"
                events = [{"data": json.dumps({"choices": [{"index": 0, "delta": {
                    "content": "stderr-spill " + marker},
                    "finish_reason": "stop"}]})}]

        elif test_mode == "incomplete":
            events = [
                {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "Hello"}, "finish_reason": None}]
                })},
            ]
            # No [DONE] marker

        elif test_mode == "escape-test":
            special = 'He said "hello" with \\ backslash\nand newline'
            events = [
                {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": special}, "finish_reason": None}]
                })},
                {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]
                })},
            ]

        elif test_mode in ("sse-format", "te-token", "false-200",
                            "content-length-missing", "content-length-short",
                            "content-length-extra",
                            "done-terminal", "trailing-json"):
            events = []

        elif test_mode in RESPONSES:
            fixture = RESPONSES[test_mode]
            events = fixture.get("events", fixture)

        else:
            self.send_error(500, "Unknown test mode: {}".format(test_mode))
            return

        include_done = test_mode != "incomplete"
        if test_mode == "sse-format":
            payload = json.dumps({
                "choices": [{"index": 0, "delta": {"content": "SSE format ok"},
                             "finish_reason": "stop"}]
            })
            split = payload.index('"delta"')
            sse_body = "data:{}\n".format(payload[:split])
            sse_body += "data: {}\n\n".format(payload[split:])
            sse_body += "data:[DONE]\n\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(sse_body)))
            self.end_headers()
            self.wfile.write(sse_body.encode("utf-8"))
            return
        if test_mode == "te-token":
            sse_body = build_sse_response([{"data": json.dumps({
                "choices": [{"index": 0, "delta": {"content": "TE token ok"},
                             "finish_reason": "stop"}]
            })}])
            raw = ("HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/event-stream\r\n"
                   "Transfer-Encoding: identity, ChUnKeD\r\n"
                   "Connection: close\r\n\r\n")
            raw += build_chunked_body(sse_body, chunk_size=7, chunk_ext="mock=1")
            self.connection.sendall(raw.encode("utf-8"))
            self.close_connection = True
            return
        if test_mode == "false-200":
            raw = ("HTTP/1.1 400 Contains 200 text\r\n"
                   "Content-Type: application/json\r\n"
                   "Content-Length: 36\r\nConnection: close\r\n\r\n"
                   '{"error":{"message":"strict status"}}')
            self.connection.sendall(raw.encode("utf-8"))
            self.close_connection = True
            return
        if test_mode in ("content-length-missing", "content-length-short",
                         "content-length-extra",
                         "done-terminal", "trailing-json"):
            valid = "data: [DONE]\n\n"
            trailing = b""
            if test_mode == "content-length-missing":
                declared = None
                body = valid
            elif test_mode == "content-length-short":
                declared = len(valid) + 5
                body = valid
            elif test_mode == "content-length-extra":
                declared = len(valid)
                body = valid
                trailing = b"extra"
            elif test_mode == "done-terminal":
                body = valid + "data: not-json\n\n"
                declared = len(body)
            else:
                payload = json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "ok"},
                                 "finish_reason": None}]
                })
                body = "data: {} {{}}\n\n".format(payload)
                body += valid
                declared = len(body)
            raw = ("HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/event-stream\r\n")
            if declared is not None:
                raw += "Content-Length: {}\r\n".format(declared)
            raw += "Connection: close\r\n\r\n"
            self.connection.sendall(raw.encode("utf-8") + body.encode("utf-8"))
            if trailing:
                time.sleep(0.05)
                self.connection.sendall(trailing)
            self.close_connection = True
            return
        if test_chunked or test_mode == "chunked":
            if test_mode == "chunked":
                test_chunked = True
                test_chunk_size = 3
                test_chunk_ext = "test=1"
            body = build_chunked_response(events, test_chunk_size, test_chunk_ext)
            if not include_done:
                body = body.rsplit("data: [DONE]", 1)[0]
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self.wfile.write(body.encode("utf-8"))
        else:
            sse_body = build_sse_response(events)
            if not include_done:
                sse_body = sse_body.rsplit("data: [DONE]", 1)[0]
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(sse_body)))
            self.end_headers()
            self.wfile.write(sse_body.encode("utf-8"))

    def log_message(self, format, *args):
        if os.environ.get("MOCK_VERBOSE"):
            super().log_message(format, *args)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    fixtures_dir = os.path.join(os.path.dirname(__file__), "fixtures")
    if os.path.isdir(fixtures_dir):
        load_responses(fixtures_dir)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), MockHandler)
    print("Mock provider listening on port {}".format(port), file=sys.stderr)
    server.serve_forever()


if __name__ == "__main__":
    main()

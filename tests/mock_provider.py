#!/usr/bin/env python3
"""OpenAI-compatible mock provider for ccode integration tests."""

import http.server
import json
import sys
import os
import socket
import struct
import time

RESPONSES = {}

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
            for msg in req.get("messages", []):
                if msg.get("role") == "user" and isinstance(msg.get("content"), str):
                    prefix = "__ccode_test_"
                    if msg["content"].startswith(prefix):
                        test_mode = msg["content"][len(prefix):]
                    break
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
            has_tool_result = any(msg.get("role") == "tool"
                                   for msg in req.get("messages", []))
            tool_count = sum(1 for m in req.get("messages", [])
                             if m.get("role") == "tool")
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
                                "name": "run_command",
                                "arguments": '{"argv":["touch","must_not_exist_marker.txt"]}'
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
                                "name": "run_command",
                                "arguments": '{"argv":["python3","-c","import time; time.sleep(30); open(\\"cancel_marker.txt\\",\\"w\\").close()"]}'
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
                    "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_test1", "type": "function", "function": {"name": "run_command", "arguments": '{"argv":["grep","-q","sub.*return a - b","src/main.c"]}'}}]}, "finish_reason": None}]
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
                    "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_test2", "type": "function", "function": {"name": "run_command", "arguments": '{"argv":["grep","-q","sub.*return a - b","src/main.c"]}'}}]}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]
                })}]
            elif tool_count == 6:
                events = [{"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "Showing diff..."}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_diff", "type": "function", "function": {"name": "git_diff", "arguments": "{}"}}]}, "finish_reason": None}]
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
                # Turn 3: emit run_command to verify
                events = [{"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "Now let me verify the fix..."}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_test", "type": "function", "function": {"name": "run_command", "arguments": '{"argv":["echo","fix verified"]}'}}]}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]
                })}]
            elif tool_count == 3:
                # Turn 4: emit git_diff to show changes
                events = [{"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "Changes made. Showing diff..."}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_diff", "type": "function", "function": {"name": "git_diff", "arguments": "{}"}}]}, "finish_reason": None}]
                })}, {"data": json.dumps({
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]
                })}]
            else:
                # Turn 5: done
                events = [{"data": json.dumps({
                    "choices": [{"index": 0, "delta": {"content": "Fix complete."}, "finish_reason": "stop"}]
                })}]

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
    server = http.server.HTTPServer(("127.0.0.1", port), MockHandler)
    print("Mock provider listening on port {}".format(port), file=sys.stderr)
    server.serve_forever()


if __name__ == "__main__":
    main()

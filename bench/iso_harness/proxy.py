#!/usr/bin/env python3
"""proxy.py — the independent referee (F63): a logging HTTP proxy between
ONE harness and the colibri engine. Every request/response is recorded to
a JSONL file: model, stream, tools/response_format presence, status,
latency, first-byte ms, token usage (parsed from JSON body or the SSE
usage line). Neither harness's self-report is trusted for any number in
the final comparison — this log + the filesystem are the ground truth.

Usage: proxy.py <listen-port> <engine-port> <logfile.jsonl>
Non-streaming passthrough AND streaming (SSE) both supported; read1()
keeps first-byte timing honest on streamed responses.

Prompt-identity instrumentation: every request logs `req_sha256` (exact
request-body bytes) so replica prompts can be compared for identity; set
ISO_PROXY_CAPTURE=1 to ALSO store the full `req_body` (needed to *categorize*
diffs, e.g. ephemeral workspace uuid vs engine-side). Normal runs stay lean.
Every entry also carries `req_ts_ms`, the request-RECEIPT timestamp (F124
Phase 1): call->row ownership is decided on receipt time, never on the
completion-side `ts_ms` (adjacent replica rows touch at the same ms, so a
completion stamp at a row boundary steals or drops calls).
"""
import http.client
import http.server
import hashlib
import os
import json
import re
import sys
import threading
import time

LISTEN_PORT, ENGINE_PORT, LOGFILE = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
LOCK = threading.Lock()
USAGE_RE = re.compile(rb'"usage"\s*:\s*(\{[^{}]*\})')
CAPTURE = os.environ.get("ISO_PROXY_CAPTURE") == "1"


def log(entry):
    line = json.dumps(entry, separators=(",", ":"))
    with LOCK:
        with open(LOGFILE, "a") as f:
            f.write(line + "\n")


def extract_usage(data: bytes):
    m = None
    for m in USAGE_RE.finditer(data):
        pass  # last usage block wins (SSE trailing chunk / JSON body)
    if not m:
        return {}
    try:
        u = json.loads(m.group(1))
        return {k: u.get(k) for k in ("prompt_tokens", "completion_tokens", "total_tokens") if k in u}
    except Exception:
        return {}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        pass

    def _forward(self, method, body=None):
        t0 = time.time()
        conn = http.client.HTTPConnection("127.0.0.1", ENGINE_PORT, timeout=300)
        headers = {k: v for k, v in self.headers.items()
                   if k.lower() not in ("host", "content-length", "connection")}
        # F124 Phase 1: capture t_recv at REQUEST RECEIPT (before conn.request).
        # This is the whole basis for receipt-time attribution: the current
        # ts_ms (stamped AFTER the response write) true-ly measures completion,
        # not receipt.  Leaving ts_ms untouched for legacy label continuity.
        t_recv = time.time()
        conn.request(method, self.path, body=body, headers=headers)
        r = conn.getresponse()
        first_byte = None
        chunks = []
        while True:
            c = r.read1(65536)
            if not c:
                break
            if first_byte is None:
                first_byte = (time.time() - t0) * 1000.0
            chunks.append(c)
        data = b"".join(chunks)
        self.send_response(r.status)
        for k, v in r.getheaders():
            if k.lower() not in ("transfer-encoding", "connection", "content-length"):
                self.send_header(k, v)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        # referee log
        entry = {
            "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "ts_ms": int(time.time() * 1000),
            # F124 Phase 1: REQUEST-RECEIPT stamp. t0 is taken at _forward
            # entry (before the upstream call); ts_ms above is stamped AFTER
            # the response is written back to the client (completion side),
            # which is why call->row attribution used to steal boundary
            # calls. Consumers attribute on req_ts_ms; `ts_ms - latency_ms`
            # reconstructs it for captures made before this field existed.
            "req_ts_ms": int(t0 * 1000),
            "method": method, "path": self.path, "status": r.status,
            "latency_ms": round((time.time() - t0) * 1000.0),
            "first_byte_ms": round(first_byte) if first_byte is not None else None,
            "resp_bytes": len(data), "usage": extract_usage(data),
        }
        if body:
            entry["req_sha256"] = hashlib.sha256(body).hexdigest()
            if CAPTURE and method == "POST":
                entry["req_body"] = body.decode("utf-8", "replace")
            try:
                j = json.loads(body)
                entry["model"] = j.get("model")
                entry["stream"] = bool(j.get("stream"))
                entry["has_tools"] = bool(j.get("tools"))
                entry["response_format"] = (j.get("response_format") or {}).get("type")
                entry["req_bytes"] = len(body)
                entry["n_messages"] = len(j.get("messages") or [])
            except Exception:
                entry["req_bytes"] = len(body)
        log(entry)
        conn.close()

    def do_GET(self):
        self._forward("GET")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        self._forward("POST", self.rfile.read(n) if n else b"")


http.server.ThreadingHTTPServer(("127.0.0.1", LISTEN_PORT), Handler).serve_forever()

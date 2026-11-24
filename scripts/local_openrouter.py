#!/usr/bin/env python3
"""Local stand-in for the OpenRouter paths rvbbit posts to.

Speaks HTTP and HTTPS. The first decision call returns 429 with Retry-After
and no body crash; the next call returns 200. Chat text containing
EMPTY_CONTENT comes back with a null message so the extension must not turn
that into a silent NULL.
"""

import argparse
import json
import ssl
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


DECISIONS = {"/api/alpha/decisions"}
CHAT = {"/api/v1/chat/completions"}
EMBED = {"/api/v1/embeddings"}
state = {"decisions": 0}
lock = threading.Lock()


def decision_body(request):
    questions = request.get("questions") or {}
    answers = {}
    for name, question in questions.items():
        kind = (question or {}).get("type")
        if kind == "choice":
            criteria = (question or {}).get("criteria") or {}
            labels = list(criteria.keys()) or ["other"]
            winner = labels[0]
            probs = {label: (0.8 if label == winner else 0.2 / max(1, len(labels) - 1)) for label in labels}
            answers[name] = {"type": "choice", "choice": winner, "probabilities": probs, "confidence": 0.8}
        elif kind == "box":
            answers[name] = {
                "type": "box",
                "box": {"top": 0.2, "bottom": 0.5, "left": 0.15, "right": 0.7},
                "confidence": 0.82,
            }
        elif kind == "score":
            levels = (question or {}).get("criteria") or ["low", "high"]
            legend = {str(i): level for i, level in enumerate(levels)}
            probs = {str(i): (0.7 if i == 1 else 0.15) for i in range(len(levels))}
            answers[name] = {
                "type": "score",
                "score": 1,
                "legend": legend,
                "probabilities": probs,
                "confidence": 0.6,
            }
        else:
            answers[name] = {"type": "noul", "noul": 0.91}
    model = request.get("model") or "mock"
    return {"model": model + "-local", "answers": answers, "usage": {"cost": 0, "total_tokens": 1}}


def chat_body(request):
    raw = json.dumps(request)
    content = None if "EMPTY_CONTENT" in raw else "positive"
    return {"model": request.get("model") or "mock", "choices": [{"message": {"role": "assistant", "content": content}}]}


def embed_body(request):
    dim = int(request.get("dimensions") or 8)
    data = []
    for index, _text in enumerate(request.get("input") or []):
        data.append({"object": "embedding", "index": index, "embedding": [0.1] * dim})
    return {"object": "list", "model": request.get("model") or "mock", "data": data}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        return

    def do_GET(self):
        body = b'{"ok":true}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b"{}"
        try:
            request = json.loads(raw.decode("utf-8"))
        except json.JSONDecodeError:
            request = {}
        path = self.path.split("?", 1)[0]
        if path in DECISIONS:
            with lock:
                state["decisions"] += 1
                first = state["decisions"] == 1
            if first:
                body = b'{"error":"slow down"}'
                self.send_response(429)
                self.send_header("Content-Type", "application/json")
                self.send_header("Retry-After", "1")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            payload = decision_body(request)
        elif path in CHAT:
            payload = chat_body(request)
        elif path in EMBED:
            payload = embed_body(request)
        else:
            body = b'{"error":"not found"}'
            self.send_response(404)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        body = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def serve(httpd):
    httpd.serve_forever()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--http-port", type=int, required=True)
    parser.add_argument("--https-port", type=int, required=True)
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    args = parser.parse_args()
    httpd = ThreadingHTTPServer(("127.0.0.1", args.http_port), Handler)
    httpsd = ThreadingHTTPServer(("127.0.0.1", args.https_port), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(args.cert, args.key)
    httpsd.socket = context.wrap_socket(httpsd.socket, server_side=True)
    threading.Thread(target=serve, args=(httpd,), daemon=True).start()
    serve(httpsd)


if __name__ == "__main__":
    main()

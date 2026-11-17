#!/usr/bin/env python3
"""Local chat-completions stand-in.

One process, threaded, HTTP/1.1. It is not a model server. Point the harness
at a real IPv4 upstream when you have one. Kestrel's own upstream requests
send Connection: close. A direct wrk2 client keeps the connection.
"""

import argparse
import socket
import socketserver
import sys
import time

BODY = b'{"choices":[{"message":{"role":"assistant","content":"pong"}}]}'
RESPONSE_KEEP = (
    b"HTTP/1.1 200 OK\r\n"
    b"Content-Type: application/json\r\n"
    b"Content-Length: " + str(len(BODY)).encode("ascii") + b"\r\n"
    b"Connection: keep-alive\r\n\r\n" + BODY
)
RESPONSE_CLOSE = (
    b"HTTP/1.1 200 OK\r\n"
    b"Content-Type: application/json\r\n"
    b"Content-Length: " + str(len(BODY)).encode("ascii") + b"\r\n"
    b"Connection: close\r\n\r\n" + BODY
)
HEALTH = b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 3\r\nConnection: close\r\n\r\nok\n"

DELAY_S = 0.0


class Handler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        conn = self.request
        conn.settimeout(5)
        pending = b""
        while True:
            while b"\r\n\r\n" not in pending:
                try:
                    chunk = conn.recv(4096)
                except socket.timeout:
                    return
                if not chunk:
                    return
                pending += chunk
                if len(pending) > 1024 * 1024:
                    return
            head, pending = pending.split(b"\r\n\r\n", 1)
            first = head.split(b"\r\n", 1)[0]
            parts = first.split(b" ")
            if len(parts) < 2:
                return
            method, target = parts[0], parts[1]
            headers = {}
            for line in head.split(b"\r\n")[1:]:
                if b":" not in line:
                    continue
                name, value = line.split(b":", 1)
                headers[name.strip().lower()] = value.strip().lower()
            length = int(headers.get(b"content-length", b"0"))
            while len(pending) < length:
                try:
                    chunk = conn.recv(4096)
                except socket.timeout:
                    return
                if not chunk:
                    return
                pending += chunk
            pending = pending[length:]
            if method == b"GET" and target in (b"/health", b"/healthz"):
                conn.sendall(HEALTH)
                return
            if DELAY_S > 0:
                time.sleep(DELAY_S)
            close = headers.get(b"connection") == b"close"
            try:
                conn.sendall(RESPONSE_CLOSE if close else RESPONSE_KEEP)
            except OSError:
                return
            if close:
                return


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main() -> int:
    global DELAY_S
    parser = argparse.ArgumentParser(description="local chat-completions stand-in")
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--delay-ms", type=float, default=0.0)
    args = parser.parse_args()
    if args.port < 1 or args.port > 65535 or args.delay_ms < 0:
        print("bad bind", file=sys.stderr)
        return 2
    DELAY_S = args.delay_ms / 1000.0
    try:
        server = Server((args.bind, args.port), Handler)
    except OSError as exc:
        print(f"mock bind failed: {exc}", file=sys.stderr)
        return 1
    print(f"mock listening on {args.bind}:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

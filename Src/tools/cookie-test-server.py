#!/usr/bin/env python3
# cookie-test-server.py -- the M1 instrument.
#
# Serves three pages whose <title> the driver's `diag` prints, so a cookie's presence can be
# read out of autocmd-log.txt without screenshots or a page-text verb:
#
#   /set     Set-Cookie: apo=m1; Path=/; Max-Age=86400; HttpOnly   -> <title>SET</title>
#   /clear   Set-Cookie: apo=; Path=/; Max-Age=0                  -> <title>CLEARED</title>
#   /check   reflects the Cookie request header                    -> <title>COOKIE=apo=m1</title>
#
# The cookie is host-only (no Domain attribute), which is the case the Netscape seed has to keep
# distinct from a domain cookie. Bind on all interfaces: the app's AppContainer may not be allowed
# to reach loopback, in which case the bench's LAN address is used instead.
#
# Usage: python cookie-test-server.py [port]   (default 8099)
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8099


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, title, body, extra_headers=()):
        payload = (
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            f"<title>{title}</title></head><body><pre>{body}</pre></body></html>"
        ).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        for name, value in extra_headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        cookie = self.headers.get("Cookie", "")
        print(f"{self.client_address[0]} {self.path} cookie={cookie!r}", flush=True)

        if self.path.startswith("/set"):
            self._send("SET", "cookie set", [("Set-Cookie", "apo=m1; Path=/; Max-Age=86400; HttpOnly")])
        elif self.path.startswith("/clear"):
            self._send("CLEARED", "cookie cleared", [("Set-Cookie", "apo=; Path=/; Max-Age=0")])
        else:
            self._send(f"COOKIE={cookie}", f"Cookie: {cookie or '(none)'}")

    def log_message(self, fmt, *args):
        pass


print(f"cookie-test-server listening on 0.0.0.0:{PORT}", flush=True)
ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()

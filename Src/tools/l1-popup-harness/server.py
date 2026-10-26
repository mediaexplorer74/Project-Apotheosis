# server.py -- L1 popup harness server (x64 bench, 2026-10-26).
#
# Serves this directory on 0.0.0.0:8765 (so the same server serves the x64 bench at
# 127.0.0.1 AND a phone at 192.168.1.34<lan-ip> later) and logs every request with its
# query string to server.log -- the log IS the acceptance evidence: a popup that opened
# shows as GET /popup-target.html?src=... or /beacon?fn=window.open&r=... .
#
# Usage:  python server.py            (then Ctrl-C / kill)
import http.server
import os
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(ROOT, "server.log")


class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=ROOT, **kw)

    def log_message(self, fmt, *args):
        line = time.strftime("%H:%M:%S ") + (fmt % args) + \
            " host=" + str(self.headers.get("Host", "-")) + \
            " referer=" + str(self.headers.get("Referer", "-"))
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(line + "\n")
        sys.stderr.write(line + "\n")
        sys.stderr.flush()


if __name__ == "__main__":
    # Port 8765 = the main origin; port 8766 = the second origin for cross-origin probes
    # (scheme+host+port == origin, so a different port IS a different origin — same trick
    # the phone probe used against the LAN).
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", port), H)
    sys.stderr.write("l1-popup-harness listening on 0.0.0.0:%d root=%s\n" % (port, ROOT))
    sys.stderr.flush()
    srv.serve_forever()
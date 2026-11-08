#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Receives files from the TV (curl -T FILE http://host:8766/NAME) into DIR.
putrecv.py PORT DIR [BIND-ADDRESS]  (default bind: 127.0.0.1; the TV needs the PC's LAN address)"""
import http.server, os, sys

DIR = sys.argv[2]

class H(http.server.BaseHTTPRequestHandler):
    def do_PUT(self):
        name = os.path.basename(self.path.split("?")[0]) or "upload.bin"
        n = int(self.headers.get("Content-Length", "0"))
        data = self.rfile.read(n)
        with open(os.path.join(DIR, name), "wb") as f:
            f.write(data)
        self.send_response(201)
        self.end_headers()
        self.wfile.write(b"ok\n")

    def log_message(self, *a):
        pass

http.server.HTTPServer((sys.argv[3] if len(sys.argv) > 3 else "127.0.0.1", int(sys.argv[1])), H).serve_forever()

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""License server for the qemu EME test with TVF's simulated CDM (TVF
tools/webmedia-test/fake_cdm.c): any POST is a license request ("CHALLENGE:..."),
the answer frees the given key ids ("LICENSE:" + 32 hex digits each).

    license.py PORT KID[,KID...]        (KID as UUID or 32 hex digits)
"""
import http.server
import sys

PORT = int(sys.argv[1])
KIDS = [k.replace('-', '').lower() for k in sys.argv[2].split(',')]
BODY = ('LICENSE:' + ''.join(KIDS)).encode()


class H(http.server.BaseHTTPRequestHandler):
    def cors(self):
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Headers', '*')
        self.send_header('Access-Control-Allow-Methods', 'POST, OPTIONS')

    def do_OPTIONS(self):
        self.send_response(204)
        self.cors()
        self.end_headers()

    def do_POST(self):
        n = int(self.headers.get('Content-Length', 0))
        req = self.rfile.read(n)
        print('Anfrage', len(req), 'Bytes:', req[:40], flush=True)
        self.send_response(200)
        self.cors()
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Length', str(len(BODY)))
        self.end_headers()
        self.wfile.write(BODY)

    def log_message(self, *a):
        pass


http.server.HTTPServer(('127.0.0.1', PORT), H).serve_forever()

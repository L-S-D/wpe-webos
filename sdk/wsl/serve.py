#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Transfer to/from the WSL build machine (LAN only, temporary).

serve.py IP PORT TOKEN DIR UPLOADDIR
GET /TOKEN/<file>          a file from DIR (setup script, archives)
PUT /TOKEN/upload/<name>   stores the body in UPLOADDIR/<name> (bundle, logs)
Everything else: 404. Prints one line per request.
"""
import http.server, os, re, shutil, sys

IP, PORT, TOKEN, DIR, UP = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5]
MAX_UP = 2 << 30


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def name(self, prefix):
        p = self.path.split("?")[0]
        if not p.startswith(prefix):
            return None
        n = p[len(prefix):]
        return n if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,99}", n) else None

    def do_GET(self):
        n = self.name(f"/{TOKEN}/")
        f = os.path.join(DIR, n) if n else None
        if not f or not os.path.isfile(f):
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(os.path.getsize(f)))
        self.end_headers()
        try:
            with open(f, "rb") as h:
                shutil.copyfileobj(h, self.wfile, 1 << 20)
            print(f"GET {n} fertig ({self.client_address[0]})", flush=True)
        except (BrokenPipeError, ConnectionResetError):
            print(f"GET {n} abgebrochen ({self.client_address[0]})", flush=True)

    def do_PUT(self):
        n = self.name(f"/{TOKEN}/upload/")
        size = int(self.headers.get("Content-Length", "-1"))
        if not n or size < 0 or size > MAX_UP:
            self.send_error(404 if not n else 411 if size < 0 else 413)
            return
        os.makedirs(UP, exist_ok=True)
        tmp = os.path.join(UP, "." + n + ".part")
        left = size
        with open(tmp, "wb") as h:
            while left:
                b = self.rfile.read(min(left, 1 << 20))
                if not b:
                    break
                h.write(b)
                left -= len(b)
        if left:
            os.unlink(tmp)
            self.send_error(400)
            return
        os.replace(tmp, os.path.join(UP, n))
        self.send_response(201)
        self.end_headers()
        self.wfile.write(b"ok\n")
        print(f"PUT {n} {size} B ({self.client_address[0]})", flush=True)

    def log_message(self, *a):
        pass


http.server.ThreadingHTTPServer((IP, PORT), H).serve_forever()

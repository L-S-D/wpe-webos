#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Mixed content and loopback (patch 0015) under qemu, from the host: an https page on
# 127.0.0.1 (own CA, given to the network process via SSL_CERT_FILE) loads a script from
# http://127.0.0.1 like the OIPF replacement loads dash.js from TVF's relay.
#   sh tests/mixed/run-qemu.sh [BUNDLE]
# Expected with 0015: "geladen, ausgefuehrt=true" and "fetch ... (no-cors): erlaubt, opaque";
# without it both are blocked ("[blocked] The page at https://... requested insecure content").
set -e
H=$(cd "$(dirname "$0")/../.." && pwd)
T=$H/tests/mixed
O=$H/out/mixed
B=${1:-$H/out/tvf-web-sdk-media-armhf}
Q=$H/out/tvf-web-sdk-media-qemu
[ -x "$B/bin/tvf-web" ] || { echo "kein Bundle in $B"; exit 1; }
rm -rf "$Q" && cp -a "$B" "$Q" && rm -f "$Q/lib/libstdc++.so.6" "$Q/lib/libgcc_s.so.1"
rm -rf "$O" && mkdir -p "$O/https" "$O/http"
cp "$T/mixed.html" "$O/https/"
echo 'window.__loopback = 1;' > "$O/http/loopback.js"
# CA and certificate for 127.0.0.1
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=tvf-test-ca -keyout "$O/ca.key" -out "$O/ca.pem" 2> /dev/null
openssl req -newkey rsa:2048 -nodes -subj /CN=127.0.0.1 -keyout "$O/srv.key" -out "$O/srv.csr" 2> /dev/null
printf 'subjectAltName=IP:127.0.0.1\nbasicConstraints=CA:FALSE\n' > "$O/ext.cnf"
openssl x509 -req -in "$O/srv.csr" -CA "$O/ca.pem" -CAkey "$O/ca.key" -CAcreateserial -days 2 \
    -extfile "$O/ext.cnf" -out "$O/srv.pem" 2> /dev/null
python3 -I -c '
import http.server, ssl, sys, functools
h = functools.partial(http.server.SimpleHTTPRequestHandler, directory=sys.argv[1])
s = http.server.HTTPServer(("127.0.0.1", 8793), h)
c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); c.load_cert_chain(sys.argv[2], sys.argv[3])
s.socket = c.wrap_socket(s.socket, server_side=True)
s.serve_forever()' "$O/https" "$O/srv.pem" "$O/srv.key" > /dev/null 2>&1 &
s1=$!
python3 -m http.server --bind 127.0.0.1 8794 --directory "$O/http" > /dev/null 2>&1 &
s2=$!
trap 'kill $s1 $s2 2>/dev/null' EXIT
sleep 1
docker run --rm --network host -e SETTLE=15000 -e SSL_CERT_FILE=/o/ca.pem \
    -e "URL=https://127.0.0.1:8793/mixed.html" -v "$Q:/b:ro" -v "$H/tests/mse:/t:ro" -v "$O:/o" tvf-web-qemu sh /t/page.sh
grep -E '^\[console\]' "$O/mse.log" | sed 's/^\[console\] [a-z]* //' | head -12
grep -i -E 'insecure content|mixed' "$O/mse.log" | grep -v '^\[console\] log \[mixed\]' | cut -c1-200 | head -4 || true

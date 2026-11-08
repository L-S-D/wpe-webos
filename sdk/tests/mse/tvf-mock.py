#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stand-in for TVF's media server (PROTOCOL-MEDIA.md) for offline tests.

tvf-mock.py [SOCKET] - default @tvf-media-test (abstract). A simulated TV decoder per
session: it plays in real time from the first data of an epoch while WebKit is playing,
takes data only up to 2 s ahead of the picture (credit via ACK), reports the position
every 200 ms and ends the stream (EOSDONE) when every stream has sent EOS and the
picture has reached the last frame. Prints one line per event and a summary per epoch.
"""
import json, socket, struct, sys, threading, time

NAME = sys.argv[1] if len(sys.argv) > 1 else "@tvf-media-test"
WINDOW = {"video": 1 << 20, "audio": 128 << 10}  # small: the credit wait is exercised
AHEAD = 2_000_000_000
NO_TIME = -(1 << 63)
M = {1: "HELLO", 2: "CAPS", 3: "DATA", 4: "FLUSH", 5: "EOS", 6: "STATE", 7: "RECT", 8: "VOLUME"}
lock = threading.Lock()
sessions = {}
t0 = time.monotonic()


def t(ns):
    return "-" if ns == NO_TIME else "%.3f" % (ns / 1e9)


def log(msg):
    print("[mock %7.3f] %s" % (time.monotonic() - t0, msg), flush=True)


class Stream:
    def __init__(self, sess, kind, conn):
        self.sess, self.kind, self.conn = sess, kind, conn
        self.wlock = threading.Lock()
        self.epoch = 0
        self.reset()

    def reset(self):
        self.items = []  # (pts, size) not yet taken by the "decoder"
        self.consumed = 0
        self.frames = self.bytes = self.keys = 0
        self.first = self.last = self.end = None
        self.eos = False
        self.max_out = 0
        self.sent_bytes = 0

    def send(self, typ, payload):
        try:
            with self.wlock:
                self.conn.sendall(struct.pack("<II", typ, len(payload)) + payload)
        except OSError:
            pass


class Session:
    def __init__(self, key):
        self.key = key
        self.streams = {}
        self.playing = False
        self.pos = None  # ns, None = not started
        self.end = None
        self.eos_sent = False
        self.last_tick = time.monotonic()

    def start_pos(self):
        firsts = [s.first for s in self.streams.values() if s.first is not None]
        v = self.streams.get("video")
        return v.first if v and v.first is not None else (min(firsts) if firsts else None)


def summary(st, why):
    log("%s %s Epoche %d: %d Puffer, %d Bytes, %d Keyframes, pts %s .. %s, max. ausstehend %d B (Fenster %d)" % (
        st.kind, why, st.epoch, st.frames, st.bytes, st.keys,
        "-" if st.first is None else "%.3f" % (st.first / 1e9), "-" if st.last is None else "%.3f" % (st.last / 1e9),
        st.max_out, WINDOW[st.kind]))


def handle(conn):
    st = None
    buf = b""
    try:
        while True:
            head = conn.recv(8, socket.MSG_WAITALL)
            if len(head) < 8:
                break
            typ, n = struct.unpack("<II", head)
            body = b""
            while len(body) < n:
                chunk = conn.recv(n - len(body))
                if not chunk:
                    raise OSError("short")
                body += chunk
            name = M.get(typ, str(typ))
            with lock:
                if typ == 1:
                    j = json.loads(body)
                    sess = sessions.setdefault(j["session"], Session(j["session"]))
                    st = Stream(sess, j["stream"], conn)
                    sess.streams[st.kind] = st
                    log("HELLO %s %s" % (j["session"], st.kind))
                    st.send(101, json.dumps({"window": WINDOW[st.kind]}).encode())
                elif st is None:
                    log("%s vor HELLO" % name)
                elif typ == 2:
                    log("CAPS %s %s" % (st.kind, body.decode()[:160]))
                elif typ == 3:
                    pts, dts, dur, flags, epoch = struct.unpack("<qqqII", body[:32])
                    size = n - 32
                    if epoch != st.epoch:
                        log("DATA %s alte Epoche %d (jetzt %d) verworfen" % (st.kind, epoch, st.epoch))
                        continue
                    if st.frames < 2 or (flags & 1 and st.kind == "video" and st.keys < 3):
                        log("DATA %s pts %s dts %s %d B%s" % (st.kind, t(pts), t(dts), size, " key" if flags & 1 else ""))
                    if pts == NO_TIME:
                        log("DATA %s ohne pts" % st.kind)
                        continue
                    st.frames += 1
                    st.bytes += size
                    st.keys += flags & 1
                    st.first = pts if st.first is None else min(st.first, pts)
                    st.last = pts if st.last is None else max(st.last, pts)
                    e = pts + (dur if dur != NO_TIME else 0)
                    st.end = e if st.end is None else max(st.end, e)
                    st.items.append((pts, size))
                    st.sent_bytes += size
                    st.max_out = max(st.max_out, st.sent_bytes - st.consumed)
                elif typ == 4:
                    (epoch,) = struct.unpack("<I", body[:4])
                    summary(st, "FLUSH nach")
                    st.epoch = epoch
                    st.reset()
                    st.sess.pos = None
                    st.sess.end = None
                    st.sess.eos_sent = False
                    log("FLUSH %s -> Epoche %d" % (st.kind, epoch))
                elif typ == 5:
                    st.eos = True
                    summary(st, "EOS nach")
                elif typ == 6:
                    st.sess.playing = json.loads(body)["playing"]
                    log("STATE %s playing=%s" % (st.kind, st.sess.playing))
                else:
                    log("%s %s %s" % (name, st.kind, body.decode()))
    except OSError:
        pass
    with lock:
        if st:
            log("%s getrennt" % st.kind)
            summary(st, "Ende")
            st.sess.streams.pop(st.kind, None)


def ticker():
    last_pos_report = 0
    while True:
        time.sleep(0.05)
        now = time.monotonic()
        with lock:
            for sess in list(sessions.values()):
                dt = int((now - sess.last_tick) * 1e9)
                sess.last_tick = now
                if not sess.streams:
                    continue
                if sess.pos is None:
                    sp = sess.start_pos()
                    if sp is None:
                        continue
                    sess.pos = sp
                    log("Bild ab %.3f s" % (sp / 1e9))
                elif sess.playing:
                    sess.pos += dt
                ends = [s.end for s in sess.streams.values() if s.end is not None]
                all_eos = all(s.eos for s in sess.streams.values())
                if all_eos and ends:
                    # like TVF: at the end the position is the end of the last frame
                    sess.pos = min(sess.pos, max(ends))
                    if not sess.eos_sent and sess.pos >= max(ends):
                        sess.eos_sent = True
                        log("EOSDONE bei %.3f s" % (sess.pos / 1e9))
                        for s in sess.streams.values():
                            s.send(104, struct.pack("<I", s.epoch))
                for s in sess.streams.values():
                    take = 0
                    while s.items and s.items[0][0] <= sess.pos + AHEAD:
                        take += s.items.pop(0)[1]
                    if take:
                        s.consumed += take
                        s.send(102, struct.pack("<IIQ", s.epoch, 0, s.consumed))
                if now - last_pos_report >= 0.2:
                    for s in sess.streams.values():
                        s.send(103, struct.pack("<q", sess.pos))
        if now - last_pos_report >= 0.2:
            last_pos_report = now


srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind("\0" + NAME[1:] if NAME.startswith("@") else NAME)
srv.listen(8)
log("bereit auf %s" % NAME)
threading.Thread(target=ticker, daemon=True).start()
while True:
    c, _ = srv.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host-side test of the tvf-web protocol (plays TVF's part).

proto_client.py TVF_WEB_BINARY URL OUT.png [more tvf-web args]
Spawns tvf-web --fd N on a socketpair, receives the frame memfd, acks frames
and walks through load, keys, eval, resize, held acks and close.
"""
import array, json, mmap, os, select, socket, struct, subprocess, sys, time, zlib

exe, url, out_png = sys.argv[1], sys.argv[2], sys.argv[3]
a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
p = subprocess.Popen([exe, "--fd", str(b.fileno()), "--width", "640", "--height", "360"] + sys.argv[4:],
                     pass_fds=[b.fileno()])
b.close()
T0 = time.monotonic()
ms = lambda: 1000 * (time.monotonic() - T0)
buf, fds = b"", []
st = {"mm": None, "w": 0, "h": 0, "size": 0, "frames": 0, "last_buf": None, "last_frame_t": 0,
      "hold": False, "held": [], "events": [], "damage": []}

def send(obj):
    a.sendall((json.dumps(obj) + "\n").encode())

def map_buffers(ev):
    fd = fds.pop(0)
    st.update(w=ev["width"], h=ev["height"], size=ev["bufferSize"])
    st["mm"] = mmap.mmap(fd, 2 * ev["bufferSize"], mmap.MAP_SHARED, mmap.PROT_READ)
    os.close(fd)

def handle(ev):
    if "frame" in ev:
        st["frames"] += 1
        st["last_buf"], st["last_frame_t"] = ev["buf"], ms()
        st["damage"].append(ev["damage"])
        if st["hold"]:
            st["held"].append(ev["frame"])
        else:
            send({"ack": ev["frame"]})
        return
    if ev.get("event") in ("ready", "buffers"):
        map_buffers(ev)
    st["events"].append((ms(), ev))
    print(f"{ms():6.0f} ms  {json.dumps(ev)[:150]}")

def pump(seconds, until=None):
    """read messages for up to `seconds`; stop early when until() is true"""
    global buf
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if until and until():
            return True
        r, _, _ = select.select([a], [], [], max(0, end - time.monotonic()))
        if not r:
            continue
        fdarr = array.array("i")
        data, anc, _, _ = a.recvmsg(65536, socket.CMSG_SPACE(4 * 4))
        for level, typ, cdata in anc:
            if level == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
                fdarr.frombytes(cdata[:len(cdata) - (len(cdata) % 4)])
        fds.extend(fdarr)
        if not data:
            return False
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            handle(json.loads(line))
    return bool(until and until())

def has_event(name, after=0):
    return any(e.get("event") == name and t >= after for t, e in st["events"])

assert pump(10, lambda: has_event("ready"))
send({"cmd": "load", "url": url})
assert pump(20, lambda: has_event("loaded") and st["frames"] > 0), "kein Bild"
print(f"erstes Bild nach {st['last_frame_t']:.0f} ms (Fenster 640x360)")
lat = []
for code in (37, 403, 461, 13, 39):
    n0, t = st["frames"], ms()
    send({"cmd": "key", "code": code, "down": True})
    send({"cmd": "key", "code": code, "down": False})
    pump(3, lambda: st["frames"] > n0)
    lat.append((code, round(st["last_frame_t"] - t) if st["frames"] > n0 else None))
    pump(0.2)
print("Taste -> neues Bild (ms):", lat)
send({"cmd": "eval", "id": 7, "js": "document.getElementById('o').textContent"})
pump(3, lambda: has_event("evalResult"))
t = ms()
send({"cmd": "resize", "w": 800, "h": 450})
pump(5, lambda: st["w"] == 800 and st["frames"] and st["damage"][-1][2] == 800)
print(f"resize -> Bild 800x450 nach {st['last_frame_t'] - t:.0f} ms")
# animation, acks held for 1.5 s: tvf-web may announce at most the free buffer
send({"cmd": "eval", "id": 8, "js": "var n=0;setInterval(function(){document.getElementById('o').textContent='tick '+(n++)},40);1"})
st["hold"] = True
pump(1.5)
held = list(st["held"])
st["hold"] = False
for f in held:
    send({"ack": f})
n0 = st["frames"]
pump(1.0)
print(f"Acks 1,5 s zurueckgehalten: {len(held)} Bilder angekuendigt (erwartet 1), danach in 1 s {st['frames'] - n0} Bilder")
w, h, size, lb = st["w"], st["h"], st["size"], st["last_buf"]
raw = st["mm"][lb * size:lb * size + w * h * 4]
rows = b"".join(b"\0" + bytes(c for x in range(w) for c in (raw[(y * w + x) * 4 + 2], raw[(y * w + x) * 4 + 1], raw[(y * w + x) * 4]))
                for y in range(h))
ch = lambda t_, d: struct.pack(">I", len(d)) + t_ + d + struct.pack(">I", zlib.crc32(t_ + d) & 0xffffffff)
open(out_png, "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                          ch(b"IDAT", zlib.compress(rows)) + ch(b"IEND", b""))
send({"cmd": "close"})
pump(5, lambda: has_event("exit"))
rc = p.wait(timeout=10)
print(f"Bilder gesamt {st['frames']}, Schadensrechtecke (letzte 3) {st['damage'][-3:]}, exit {rc}")

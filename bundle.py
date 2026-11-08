#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Self-contained tvf-web bundle from the Debian packages of the build image.

Runs inside the tvf-web-build container: bundle.py armhf|amd64 OUTDIR
Layout: bin/tvf-web, lib/ (all libraries incl. Debian's loader, GIO TLS
module), libexec/ (WebKit processes, injected bundle), etc/ (fonts.conf, CA
bundle), share/ (fonts, glvnd vendor file, xkb data). Nothing from the TV is
used; start with run.sh (bundled loader + --library-path).
"""
import glob, json, os, shutil, subprocess, sys

arch, out = sys.argv[1], sys.argv[2]
TRIPLET = {"armhf": "arm-linux-gnueabihf", "amd64": "x86_64-linux-gnu"}[arch]
LOADER = {"armhf": "ld-linux-armhf.so.3", "amd64": "ld-linux-x86-64.so.2"}[arch]
L = f"/usr/lib/{TRIPLET}"
SEARCH = [L, f"/lib/{TRIPLET}"]
BUILD = f"/work/build/{arch}"

shutil.rmtree(out, ignore_errors=True)
for d in ["bin", "lib/gio/modules", "libexec/injected-bundle", "etc/fonts", "etc/ssl",
          "share/fonts", "share/glvnd/egl_vendor.d"]:
    os.makedirs(f"{out}/{d}", exist_ok=True)

def needed(path):
    r = subprocess.run(["readelf", "-d", path], capture_output=True, text=True, check=True).stdout
    return [l.split("[", 1)[1].rstrip("]") for l in r.splitlines() if "(NEEDED)" in l]

def find(name):
    for d in SEARCH:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    return None

copied, missing, queue = set(), set(), []

def add_file(src, dst):
    shutil.copy2(os.path.realpath(src), dst)
    queue.append(dst)

def add_lib(name):
    if name in copied or name == LOADER:
        return
    p = find(name)
    if not p:
        missing.add(name)
        return
    copied.add(name)
    add_file(p, f"{out}/lib/{name}")

add_file(f"{BUILD}/tvf-web", f"{out}/bin/tvf-web")
add_file(f"{BUILD}/libtvfca.so", f"{out}/lib/libtvfca.so")
copied.add("libtvfca.so")
for p in ["WPEWebProcess", "WPENetworkProcess", "WPEGPUProcess"]:
    if os.path.exists(f"{L}/wpe-webkit-2.0/{p}"):
        add_file(f"{L}/wpe-webkit-2.0/{p}", f"{out}/libexec/{p}")
add_file(f"{L}/wpe-webkit-2.0/injected-bundle/libWPEInjectedBundle.so",
         f"{out}/libexec/injected-bundle/libWPEInjectedBundle.so")
add_file(f"{L}/gio/modules/libgiognutls.so", f"{out}/lib/gio/modules/libgiognutls.so")
# opened with dlopen(): EGL/GLES (epoxy, glvnd) and Mesa's EGL vendor library
for name in ["libEGL.so.1", "libGLESv2.so.2", "libEGL_mesa.so.0"]:
    add_lib(name)
shutil.copy2(os.path.realpath(f"{L}/{LOADER}"), f"{out}/lib/{LOADER}")
queue.append(f"{out}/lib/{LOADER}")

while queue:
    f = queue.pop()
    for n in needed(f):
        add_lib(n)

# data
shutil.copy2("/etc/ssl/certs/ca-certificates.crt", f"{out}/etc/ssl/ca-certificates.crt")
for f in glob.glob("/usr/share/fonts/truetype/dejavu/*.ttf"):
    shutil.copy2(f, f"{out}/share/fonts/")
with open(f"{out}/etc/fonts/fonts.conf", "w") as fc:
    fc.write("""<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<!-- tvf-web: bundled fonts (relative to this file) and the TV's own fonts -->
<fontconfig>
  <dir prefix="relative">../../share/fonts</dir>
  <dir>/usr/share/fonts</dir>
  <cachedir prefix="xdg">fontconfig</cachedir>
  <alias><family>sans-serif</family><prefer><family>DejaVu Sans</family></prefer></alias>
  <alias><family>serif</family><prefer><family>DejaVu Serif</family></prefer></alias>
  <alias><family>monospace</family><prefer><family>DejaVu Sans Mono</family></prefer></alias>
  <alias><family>Tiresias</family><prefer><family>DejaVu Sans</family></prefer></alias>
</fontconfig>
""")
with open(f"{out}/share/glvnd/egl_vendor.d/50_mesa.json", "w") as f:
    json.dump({"file_format_version": "1.0.0", "ICD": {"library_path": "libEGL_mesa.so.0"}}, f)
shutil.copytree("/usr/share/X11/xkb", f"{out}/share/X11/xkb", symlinks=False)
with open(f"{out}/run.sh", "w") as f:
    f.write(f"""#!/bin/sh
# tvf-web from this bundle: Debian loader and libraries, nothing from the system
D=$(cd "$(dirname "$0")" && pwd)
exec "$D/lib/{LOADER}" --library-path "$D/lib" "$D/bin/tvf-web" "$@"
""")
os.chmod(f"{out}/run.sh", 0o755)
pk = ["libwpewebkit-2.0-1", "libc6", "mesa-libgallium", "libllvm22", "glib-networking", "libgnutls30t64",
      "libsoup-3.0-0", "ca-certificates"]
r = subprocess.run(["dpkg-query", "-W", "-f", "${Package} ${Version}\n"] + [f"{p}:{arch}" if p != "ca-certificates" else p for p in pk],
                   capture_output=True, text=True)
with open(f"{out}/VERSIONS", "w") as f:
    f.write(r.stdout)
print("fehlend:", " ".join(sorted(missing)) or "-")
print("Bibliotheken:", len(copied))

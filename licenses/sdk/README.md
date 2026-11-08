# Licence texts of the components taken from the webOS SDK and Debian

`sdk/scripts/bundle-sdk.py` puts these into every tvf-web bundle
(`share/licenses/`), next to the licence files it reads from the source
tarballs of everything built here (`sdk/sources.lock`).

Where the SDK sysroot ships the upstream text it is used (ICU `icu.txt`,
libjpeg-turbo, PCRE2). For the other libraries the SDK has none; their texts
are the Debian/Ubuntu copyright files of the same upstream projects (Ubuntu
24.04, taken 2026-11-07), which reproduce the upstream notices; versions in the
bundle are the SDK's (see the bundle's `share/licenses/NOTICE.txt`). Texts they
refer to (`/usr/share/common-licenses/...`) are in `../` (GPL, LGPL, MPL,
Apache). The data files (DejaVu fonts, xkeyboard-config, CA certificates) come
from the Debian build (`out/tvf-web-armhf`).

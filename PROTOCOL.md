# tvf-web ↔ TVF protocol

TVF starts the renderer with one end of a Unix stream socketpair:

```
<bundle>/run.sh --fd N [--width 1280] [--height 720] [--data-dir DIR] [--user-agent UA] [--media]
```

(`run.sh` = `lib/ld-linux-armhf.so.3 --library-path lib bin/tvf-web "$@"`; TVF can also exec the loader
itself.) `--data-dir` must be writable: cookies/localStorage (`data/`), cache, fontconfig and Mesa shader
cache, GStreamer registry. Without it the session is ephemeral. Logs go to stderr.

Both directions carry JSON objects, one per line (`\n`). Frame pixels never travel over the socket.

## Shared memory

The first message from tvf-web is `ready`; it carries a memfd as `SCM_RIGHTS` ancillary data on the same
`sendmsg()`. TVF receives it with `recvmsg()` and maps `2 * bufferSize` bytes `MAP_SHARED, PROT_READ`.

```json
{"event":"ready","width":1280,"height":720,"stride":5120,"format":"BGRA8-premultiplied","buffers":2,"bufferSize":3686400,"pid":1234}
```

* Buffer *i* starts at offset `i * bufferSize`; rows are `stride` bytes (`width * 4`), no padding.
* Pixel bytes B, G, R, A (`SDL_PIXELFORMAT_ARGB8888` on little endian), **premultiplied alpha**. Transparent
  where the page has no background - the live picture shows through (draw with
  `SDL_ComposeCustomBlendMode(ONE, ONE_MINUS_SRC_ALPHA, ADD, ONE, ONE_MINUS_SRC_ALPHA, ADD)`).
* After `resize` the same message arrives again as `"event":"buffers"` with a **new** memfd; unmap the old
  one. Frames after it refer to the new buffers.

## Frames

```json
{"frame":17,"buf":1,"damage":[x,y,w,h]}
```

* `buf` holds the complete new frame. `damage` is the bounding box of the pixels that differ from the
  **previous announced frame** - updating a texture that shows the previous frame with just this rectangle
  is enough. The first frame (and the first after `buffers`) has the full size.
* Frames are only announced when something changed.
* Until TVF answers `{"ack":17}` the buffer belongs to TVF and tvf-web does not write to it. With both
  buffers unacknowledged tvf-web keeps the newest rendering aside and announces it right after the next ack
  (no frame queue builds up, intermediate frames are dropped). Ack as soon as the rectangle is copied into
  the texture.

## Commands (TVF → tvf-web)

| command | fields | effect |
| --- | --- | --- |
| `{"ack":N}` | | frame N copied, its buffer may be reused |
| `{"cmd":"load","url":U,"userAgent":UA,"proxy":P}` | `userAgent`, `proxy` optional | load U. `proxy` applies to all later requests of the session, HTTP and HTTPS (CONNECT): `"http://<PC-IP>:8906"`, `""` = direct. Omitted = unchanged |
| `{"cmd":"key","code":C,"down":true\|false}` | | key to the page. 37-40, 13, 8, 27, 32, 33/34, 48-57 and the colour keys 403-406 are real (trusted) key events (`key` = `ArrowLeft`, `ColorF0Red`, ...). Other codes (VK_BACK 461, 415, 19, 413, 417, 412, ...) become a synthetic `KeyboardEvent` with that `keyCode`/`which` on `document.activeElement` (`isTrusted` false) |
| `{"cmd":"pointer","type":T,"x":X,"y":Y}` | `T` = `move`, `down`, `up`, `leave` | pointer in page coordinates (1280x720 view) as real mouse events (primary button): `mousemove`/`mouseover`, `mousedown`/`mouseup`/`click`, `leave` ends hover. TVF sends it only to pages that set `keyset.supportsPointer` (OIPF DAE 7.2.5.2) |
| `{"cmd":"scroll","dy":D,"x":X,"y":Y}` | `x`, `y` optional (centre) | wheel steps at X,Y: `D` > 0 up, < 0 down (`wheel` event, scrolls the element under X,Y) |
| `{"cmd":"eval","js":S,"id":I}` | | evaluate S in the page's main world; answer `evalResult` |
| `{"cmd":"userScript","js":S}` | | inject S at document start into all frames of every **later** load (OIPF polyfill) |
| `{"cmd":"clearUserScripts"}` | | remove them (the console hook stays) |
| `{"cmd":"resize","w":W,"h":H}` | | new size; answered by `buffers` |
| `{"cmd":"visible","on":false}` | | page hidden/visible (WebKit throttles hidden pages) |
| `{"cmd":"close"}` | | quit; tvf-web sends `exit` and ends |

## Events (tvf-web → TVF)

| event | fields |
| --- | --- |
| `ready`, `buffers` | see above (memfd attached) |
| `loaded` | `url` - main document finished |
| `loadFailed` | `url`, `error` |
| `tlsError` | `url`, `flags` (GTlsCertificateFlags: 1 unknown CA, 2 bad identity, 8 expired, ...) - the load fails, nothing is accepted |
| `msg` | `data` - what the page sent with `window.webkit.messageHandlers.tvf.postMessage(x)` (JSON value) |
| `console` | `text` - `console.*` output and uncaught errors of the page (`"error ReferenceError: ... @url:line"`) |
| `evalResult` | `id`, `value` (JSON value) or `error` |
| `crashed` | `reason`: `crashed`, `memory` or `terminated` - the WebProcess died; `load` again to recover |
| `exit` | tvf-web ends (after `close`, or when the socket closes) |

If TVF closes the socket, tvf-web quits. If tvf-web dies, TVF sees EOF and starts a new one.

## HbbTV documents

Pages served as `application/vnd.hbbtv.xhtml+xml` / CE-HTML are not displayable for WebKit. For the main
document tvf-web refuses WebKit's response, fetches the page once more (same proxy and user agent) and loads
it as `text/html` with its real URL as base, so origin and relative addresses stay the server's. WebKit
reports the refused first response as `loadFailed` "Frame load interrupted" before the real `loaded` -
ignore it when a `loaded` for the same URL follows.

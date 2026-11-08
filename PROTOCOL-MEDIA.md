# Media protocol: TVF sinks <-> TVF

With a media build of tvf-web (`<bundle>/features` contains `media`, tvf-web started with `--media`)
WebKit plays `<video>`/`<audio>` and MSE itself. Its GStreamer pipeline ends in `tvfvideosink` and
`tvfaudiosink` (`helper/gsttvf.c`), which hand the encoded streams to TVF; TVF feeds them to the TV's
hardware decoder (StarfishMediaAPIs, BUFFERSTREAM) and shows the picture through the hole WebKit's
compositor punches.

## Connection

TVF listens on a Unix stream socket and passes its name to tvf-web in `TVF_MEDIA_SOCK` (the
WebProcess inherits it); `@name` is an abstract socket. Each sink opens its own connection when it
starts. Without the variable, or when the connect fails, the sinks only log (offline tests).

## Framing

Every message: 8-byte head `uint32 type, uint32 length` (little endian), then `length` bytes. JSON
payloads are UTF-8 without a terminating zero. Times are int64 nanoseconds of stream time (the
media element's timeline), `INT64_MIN` = none.

## Sink -> TVF

| type | name | payload |
| --- | --- | --- |
| 1 | HELLO | `{"v":1,"session":"<key>","stream":"video"\|"audio"}` - at the first caps; both sinks of one pipeline send the same session key |
| 2 | CAPS | `{"codec":"H264"\|"H265"\|"AAC"\|"AC3"\|"EAC3"\|"MPA", "width","height","fps", "rate","channels","aot","layer", "protection":"<system id>", "caps":"<GStreamer caps>"}` - after HELLO and whenever the caps change (only the fields that apply; `protection`: the stream is encrypted, the system id (UUID) of its DRM, `""` if unknown - see EME) |
| 3 | DATA | 32-byte head `int64 pts, int64 dts, int64 duration, uint32 flags (1 = key frame, 4 = encrypted), uint32 epoch`, with flag 4 the crypto block, then one access unit: H.264/HEVC as byte-stream with parameter sets before key frames, AAC as ADTS, AC-3/E-AC-3/MPEG audio frames as they are |
| 4 | FLUSH | `uint32 epoch` - after a flush (seek): data of older epochs is void, the credit starts at 0 |
| 5 | EOS | `uint32 epoch` - no more data in this epoch; the sink waits for EOSDONE before WebKit sees the end |
| 6 | STATE | `{"playing":true\|false}` - WebKit's pipeline goes to PLAYING / back to PAUSED |
| 7 | RECT | `{"x","y","w","h"}` - video sink only: the video element in view pixels (tvf-web's view, 1280x720 for HbbTV) |
| 8 | VOLUME | `{"volume":0.0-10.0,"mute":bool}` - audio sink only |

## TVF -> sink

| type | name | payload |
| --- | --- | --- |
| 101 | WINDOW | `{"window":<bytes>}` - credit window of this stream (default until then: video 4 MB, audio 512 KB) |
| 102 | ACK | `uint32 epoch, uint32 0, uint64 consumed` - bytes of DATA payload of this epoch that went into the decoder; the sink sends while `sent - consumed + size <= window` (or nothing is outstanding) |
| 103 | POS | `int64` - stream time of the picture on screen (WebKit's currentTime); at the end the end of the last frame |
| 104 | EOSDONE | `uint32 epoch` - the TV has played everything; the sink lets EOS through |
| 105 | ERROR | text - the sink posts an element error (media element `error`) |

## Rules

* TVF always reads its connections without delay and keeps what the decoder does not take yet in
  its own queue - backpressure only through the credit. The sinks never block on the socket, so a
  pause or a flush always reaches them (they wait on a condition, `unlock` wakes them).
* A session (HELLO key) is one TV pipeline with up to one video and one audio stream. TVF loads it
  once the streams that said HELLO have sent CAPS and data; a HELLO up to a moment later still makes
  it into the same load.
* FLUSH: TVF drops the queued data of the old epoch and restarts the decoder at the first data of
  the new one (seek). Positions refer to stream time, so a seek target arrives unchanged.
* STATE `playing` plays, `false` pauses the TV pipeline. With sync off in the sinks WebKit pushes
  ahead as far as the credit allows; the TV keeps the time and reports it with POS.
* A sink's connection ending = its stream ends; when both are gone the session is over (TVF unloads
  the decoder and gives the picture back to live TV).

## EME

With `ENABLE_THUNDER` WebKit's Thunder CDM runs against TVF's OpenCDM client (`helper/tvf-ocdm.c`,
`libocdm.so` in the bundle): TVF hosts the TV's CDMs (Chromium library CDMs, `/usr/lib/libwidevine-wrapper.so`,
`/usr/lib/libplayreadycdm.so`), the license exchange goes through it, and encrypted samples stay
encrypted until TVF. WebKit's decryptor only tags them (a GstProtectionMeta
`application/x-tvf-cenc`: the demuxer's fields plus `tvf-session`), the sinks send them with a crypto
block, TVF's CDM turns each sample into the input of the TV's secure video path, and TVF loads the
TV pipeline for DRM (`option.drm.type`, `externalStreamingInfo.svpVersion`) - like LG's own browser
and Kodi on webOS.

Crypto block (DATA flag 4), little endian:

| field | |
| --- | --- |
| `uint32 length` | of the rest of the block |
| `uint8 scheme, uint8 iv_len, uint8 kid_len, uint8 sid_len` | scheme 1 = cenc (AES-CTR), 2 = cbcs (AES-CBC, pattern) |
| `uint32 crypt, uint32 skip` | pattern in 16-byte blocks (cbcs; 0 = none) |
| `uint32 count` | subsamples |
| `iv[iv_len], kid[kid_len], session id[sid_len]` | IV (8 or 16 bytes, the constant IV for cbcs), key id, CDM session (as in DRM below) |
| `count x (uint32 clear, uint32 encrypted)` | subsample map of the access unit as sent (bytes the sink inserts - start codes, parameter sets, ADTS header - count as clear) |

The credit counts the access unit only, not the block. A stream whose caps carry `protection`
belongs to a DRM pipeline from the start, its clear samples (clear lead) included.

### DRM connection (OpenCDM client <-> TVF)

The client opens one connection per process; its HELLO is `{"drm":1}`. Payload fields are little
endian; `str`/`bytes` = `uint32 length` + data. A request carries a request id and gets one REPLY.
`sys` and `sess` are the client's own ids (an OpenCDM system per media keys object, a session per
media key session); the session id of the CDM is what TVF answers to CREATE.

| type | name | payload |
| --- | --- | --- |
| 20 | OPEN | `u32 sys, str key_system` - no reply; TVF creates the CDM instance when first needed |
| 21 | CLOSE | `u32 sys` - TVF closes its sessions and the instance |
| 22 | CERT | `u32 req, u32 sys, str key_system, bytes certificate` - server certificate (Widevine) |
| 23 | CREATE | `u32 req, u32 sys, str key_system, u32 sess, u32 license_type (0 temporary, 1 persistent usage record, 2 persistent license), str init_data_type ("cenc", "keyids", "webm"), bytes init_data` - REPLY text = session id; the license request follows as MESSAGE |
| 24 | LOAD | `u32 req, u32 sys, str key_system, u32 sess, str session_id` - persistent session |
| 25 | UPDATE | `u32 req, u32 sess, bytes response` - the license server's answer |
| 26 | REMOVE | `u32 req, u32 sess` |
| 27 | SCLOSE | `u32 req, u32 sess` |
| 28 | RELEASE | `u32 sess` - the client dropped the session (TVF closes it if still open) |
| 29 | QUERY | `u32 req, str key_system, str mime_type` - REPLY status 0 = supported |

TVF -> client:

| type | name | payload |
| --- | --- | --- |
| 120 | REPLY | `u32 req, u32 status (0 = ok), str text` (session id, or the error) |
| 121 | MESSAGE | `u32 sess, u32 type (0 license request, 1 renewal, 2 release, 3 individualization), str url, bytes message` |
| 122 | KEYS | `u32 sess, u32 count, count x (bytes key_id, u32 status)` - status as OpenCDM's KeyStatus (0 usable, 1 expired, 2 released, 3 output restricted, 5 output downscaled, 6 pending, 7 internal error) |
| 123 | ERROR | `u32 sess, str text` |
| 124 | CLOSED | `u32 sess` - closed by the CDM (its keys are released) |

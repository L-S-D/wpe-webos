/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gst-tvf: GStreamer sinks for WebKit's media code on webOS (TVF).
 *
 * tvfvideosink and tvfaudiosink take the streams still encoded and hand them
 * to TVF, which feeds the TV's hardware decoder. That is WebKit's "Rialto"
 * model (a media server owns the decoder), selected by GStreamerQuirkTVF and
 * GStreamerHolePunchQuirkTVF (WebKit patch 0009, WEBKIT_GST_QUIRKS=tvf,
 * WEBKIT_GST_HOLE_PUNCH_QUIRK=tvf). The klass contains "Decoder" so that
 * WebKit's registry scanner counts the sinks as decoders for these formats
 * (MediaSource.isTypeSupported, canPlayType).
 *
 * Formats: H.264/HEVC access units either as byte-stream or length-prefixed
 * (avc/avc3, hvc1/hev1 - what MSE delivers from MP4), AAC as ADTS or raw
 * (MP4), MPEG audio, AC-3/E-AC-3. The sinks turn everything into what the
 * TV's ES input takes: byte-stream with the parameter sets from codec_data
 * in front of each key frame (unless it carries its own), AAC with an ADTS
 * header per frame.
 *
 * With TVF_MEDIA_SOCK (set by TVF, "@name" = abstract socket) each sink
 * connects to TVF (protocol in ../PROTOCOL-MEDIA.md): the converted access
 * units go there with their stream time, TVF feeds the TV's decoder and
 * reports back how much it took (flow control: the sink waits for credit,
 * never on the socket, so pause and flush always get through), the position
 * of the picture on screen (answers WebKit's position query - currentTime)
 * and the real end of the stream (EOS waits for it). sync is off then: the
 * TV keeps the time. Without TVF the sinks only log and pace on the
 * pipeline clock (offline checks under qemu).
 *
 * TVF_SINK_LOG=all logs every buffer (default: the first 30 per sink, then
 * key frames and every 200th; with TVF only the first 3). TVF_SINK_DUMP=<dir>
 * writes the converted streams to <dir>/tvf-<kind>-<n>.<ext>, one file per
 * start.
 */
#include <errno.h>
#include <gst/audio/streamvolume.h>
#include <gst/base/gstbasesink.h>
#include <gst/gst.h>
#include <gst/gstprotection.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#define VIDEO_CAPS                                                                                                     \
    "video/x-h264, stream-format=(string){ byte-stream, avc, avc3 }, alignment=(string)au; "                          \
    "video/x-h265, stream-format=(string){ byte-stream, hvc1, hev1 }, alignment=(string)au"
#define AUDIO_CAPS                                                                                                     \
    "audio/mpeg, mpegversion=(int){ 2, 4 }, stream-format=(string){ adts, raw }; "                                    \
    "audio/mpeg, mpegversion=(int)1; "                                                                                \
    "audio/x-ac3; audio/x-eac3"

/* protocol (PROTOCOL-MEDIA.md): 8-byte head (type, payload length; little
 * endian), sink -> TVF 1..8, TVF -> sink 101..105 */
enum { M_HELLO = 1, M_CAPS, M_DATA, M_FLUSH, M_EOS, M_STATE, M_RECT, M_VOLUME };
enum { T_WINDOW = 101, T_ACK, T_POS, T_EOSDONE, T_ERROR };
typedef struct {
    guint32 type, len;
} MsgHead;
#define NO_TIME G_MININT64
typedef struct {
    gint64 pts, dts, dur; /* ns, stream time (pts/dts may be negative); NO_TIME = none */
    guint32 flags, epoch; /* flags: 1 = key frame, 4 = encrypted (crypto block follows) */
} DataHead;
#define DATA_KEY 1
#define DATA_CRYPTO 4

typedef struct {
    GstBaseSink parent;
    gboolean video;
    int codec;          /* 264, 265 or 0 (audio) */
    int nal_len;        /* length prefix size of avc/hvc1 input, 0 = byte-stream */
    GByteArray *header; /* parameter sets from codec_data, as byte-stream */
    gboolean adts;      /* raw AAC: an ADTS header goes in front of each frame */
    int adts_profile, adts_sfi, adts_ch;
    GByteArray *out; /* converted buffer */
    gboolean bad_logged;
    /* EME (tvf-ocdm.c): encrypted samples stay encrypted, TVF's CDM gets them */
    gchar *protection; /* system id of an encrypted stream upstream ("" = unknown), NULL = clear */
    GArray *ins;       /* conversion: insertions (input offset, bytes) - for the subsample map */
    GArray *subs;      /* subsamples of the current sample (clear, encrypted) */
    GByteArray *pre;   /* DATA head + crypto block */
    guint64 crypted;
    gchar *rectangle;
    gdouble volume;
    gboolean mute;
    FILE *dump;
    guint64 count, bytes, keys;
    GstClockTime first_pts, last_pts;
    GstSegment seg;
    gboolean have_seg;
    /* TVF */
    int fd; /* -1 = no TVF (log only) */
    GThread *reader;
    GMutex wlock; /* one message at a time on the socket */
    GMutex lock;  /* everything below */
    GCond cond;
    gboolean hello_sent, dead, unlocked, eos_done;
    guint32 epoch;
    guint64 sent, consumed, window;
    gint64 pos; /* stream time of the picture on screen, -1 = unknown */
} GstTvfSink;

typedef struct {
    GstBaseSinkClass parent_class;
} GstTvfSinkClass;

/* volume/mute: playbin's stream volume goes to the sink itself (otherwise
 * playsink would put a volume element, which needs raw audio, before it) */
enum { PROP_0, PROP_RECTANGLE, PROP_VOLUME, PROP_MUTE };

GType gst_tvf_sink_get_type(void);
G_DEFINE_TYPE(GstTvfSink, gst_tvf_sink, GST_TYPE_BASE_SINK)

static int g_log_all = -1;
static const guint8 start_code[4] = {0, 0, 0, 1};

static const char *kind(GstTvfSink *s) { return s->video ? "video" : "audio"; }

static int nal_type(GstTvfSink *s, guint8 b) { return s->codec == 265 ? (b >> 1) & 0x3f : b & 0x1f; }
static int nal_is_sps(GstTvfSink *s, int t) { return s->codec == 265 ? t == 33 : t == 7; }
static int nal_is_aud(GstTvfSink *s, int t) { return s->codec == 265 ? t == 35 : t == 9; }

/* NAL unit types of a byte-stream access unit, e.g. "9,7,8,6,5" */
static void nal_types(GstTvfSink *s, const guint8 *d, gsize n, char *out, size_t on) {
    size_t o = 0;
    out[0] = 0;
    for (gsize i = 0; i + 3 < n && o + 6 < on; i++) {
        if (d[i] || d[i + 1] || d[i + 2] != 1) continue;
        o += (size_t) g_snprintf(out + o, on - o, "%s%d", o ? "," : "", nal_type(s, d[i + 3]));
        i += 3;
    }
}

static void add_nal(GByteArray *a, const guint8 *d, gsize n) {
    g_byte_array_append(a, start_code, 4);
    g_byte_array_append(a, d, (guint) n);
}

/* avcC (ISO 14496-15 5.3.3): length size, then SPS and PPS lists */
static gboolean parse_avcc(GstTvfSink *s, const guint8 *d, gsize n) {
    if (n < 7) return FALSE;
    s->nal_len = (d[4] & 3) + 1;
    gsize p = 5;
    for (int list = 0; list < 2; list++) {
        if (p >= n) return FALSE;
        int cnt = list ? d[p] : d[p] & 0x1f;
        p++;
        for (int i = 0; i < cnt; i++) {
            if (p + 2 > n) return FALSE;
            gsize l = (gsize) d[p] << 8 | d[p + 1];
            p += 2;
            if (p + l > n) return FALSE;
            add_nal(s->header, d + p, l);
            p += l;
        }
    }
    return TRUE;
}

/* hvcC (ISO 14496-15 8.3.3): length size at byte 21, then NAL unit arrays (VPS, SPS, PPS, SEI) */
static gboolean parse_hvcc(GstTvfSink *s, const guint8 *d, gsize n) {
    if (n < 23) return FALSE;
    s->nal_len = (d[21] & 3) + 1;
    gsize p = 23;
    for (int a = 0; a < d[22]; a++) {
        if (p + 3 > n) return FALSE;
        int cnt = d[p + 1] << 8 | d[p + 2];
        p += 3;
        for (int i = 0; i < cnt; i++) {
            if (p + 2 > n) return FALSE;
            gsize l = (gsize) d[p] << 8 | d[p + 1];
            p += 2;
            if (p + l > n) return FALSE;
            add_nal(s->header, d + p, l);
            p += l;
        }
    }
    return TRUE;
}

typedef struct {
    const guint8 *d;
    gsize n, pos; /* in bits */
} Bits;

static guint bits(Bits *b, int cnt) {
    guint v = 0;
    while (cnt--) {
        guint bit = b->pos < b->n * 8 ? (b->d[b->pos / 8] >> (7 - b->pos % 8)) & 1 : 0;
        v = v << 1 | bit;
        b->pos++;
    }
    return v;
}

static int aac_object_type(Bits *b) {
    int t = (int) bits(b, 5);
    return t == 31 ? 32 + (int) bits(b, 6) : t;
}

static const guint aac_rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};

static int aac_rate_index(guint rate) {
    for (int i = 0; i < 13; i++)
        if (aac_rates[i] == rate) return i;
    return -1;
}

/* AudioSpecificConfig (ISO 14496-3 1.6.2.1) -> ADTS profile, sampling index,
 * channels. With explicit SBR/PS signalling the first rate is the core rate. */
static gboolean parse_asc(GstTvfSink *s, const guint8 *d, gsize n) {
    Bits b = {d, n, 0};
    if (n < 2) return FALSE;
    int aot = aac_object_type(&b);
    int sfi = (int) bits(&b, 4);
    if (sfi == 15) sfi = aac_rate_index(bits(&b, 24));
    int ch = (int) bits(&b, 4);
    if (aot == 5 || aot == 29) {
        if (bits(&b, 4) == 15) bits(&b, 24);
        aot = aac_object_type(&b);
    }
    /* ADTS carries object types 1..4 and channel configurations 1..7 only */
    if (aot < 1 || aot > 4 || sfi < 0 || sfi > 12 || ch < 1 || ch > 7) return FALSE;
    s->adts_profile = aot - 1;
    s->adts_sfi = sfi;
    s->adts_ch = ch;
    return TRUE;
}

static void adts_header(GstTvfSink *s, gsize payload, guint8 h[7]) {
    gsize len = payload + 7;
    h[0] = 0xff;
    h[1] = 0xf1; /* MPEG-4, layer 0, no CRC */
    h[2] = (guint8) (s->adts_profile << 6 | s->adts_sfi << 2 | s->adts_ch >> 2);
    h[3] = (guint8) ((s->adts_ch & 3) << 6 | (len >> 11 & 3));
    h[4] = (guint8) (len >> 3 & 0xff);
    h[5] = (guint8) ((len & 7) << 5 | 0x1f);
    h[6] = 0xfc;
}

/* bytes added at input offset off (for the subsample map of encrypted samples) */
static void note_insert(GstTvfSink *s, gsize off, gsize bytes) {
    if (!bytes) return;
    guint32 e[2] = {(guint32) off, (guint32) bytes};
    g_array_append_vals(s->ins, e, 2);
}

/* length-prefixed access unit -> byte-stream; the parameter sets from
 * codec_data go in front of a key frame that has none (after an AUD) */
static gboolean to_byte_stream(GstTvfSink *s, const guint8 *d, gsize n, gboolean key) {
    gboolean has_ps = FALSE, ok = TRUE;
    gsize p = 0;
    while (p + (gsize) s->nal_len <= n) {
        gsize l = 0;
        for (int i = 0; i < s->nal_len; i++) l = l << 8 | d[p + i];
        p += (gsize) s->nal_len;
        if (!l || l > n - p) break;
        if (nal_is_sps(s, nal_type(s, d[p]))) has_ps = TRUE;
        p += l;
    }
    gboolean need = key && !has_ps && s->header->len;
    g_byte_array_set_size(s->out, 0);
    p = 0;
    while (p + (gsize) s->nal_len <= n) {
        gsize l = 0;
        for (int i = 0; i < s->nal_len; i++) l = l << 8 | d[p + i];
        p += (gsize) s->nal_len;
        if (!l || l > n - p) {
            ok = FALSE;
            break;
        }
        if (need && !nal_is_aud(s, nal_type(s, d[p]))) {
            g_byte_array_append(s->out, s->header->data, s->header->len);
            note_insert(s, p - (gsize) s->nal_len, s->header->len);
            need = FALSE;
        }
        /* start code instead of the length field */
        note_insert(s, p - (gsize) s->nal_len, 4 - (gsize) s->nal_len);
        add_nal(s->out, d + p, l);
        p += l;
    }
    return ok && p == n;
}

/* ---------------- EME ---------------- */

/* caps of an encrypted stream: application/x-cenc (also cbcs) with the system id */
static gchar *caps_protection(GstPad *pad) {
    GstCaps *c = gst_pad_get_current_caps(pad);
    gchar *r = NULL;
    for (guint i = 0; c && i < gst_caps_get_size(c) && !r; i++) {
        const GstStructure *st = gst_caps_get_structure(c, i);
        const char *n = gst_structure_get_name(st);
        if (g_str_has_prefix(n, "application/x-cenc") || g_str_has_prefix(n, "application/x-cbcs") ||
            g_str_has_prefix(n, "application/x-webm-enc")) {
            const char *sys = gst_structure_get_string(st, "protection-system");
            r = g_strdup(sys ? sys : "");
        }
    }
    if (c) gst_caps_unref(c);
    return r;
}

/* the sink pad of e that feeds its src pad (only one, or src_N <-> sink_N) */
static GstPad *matching_sink_pad(GstElement *e, GstPad *src) {
    GstPad *r = NULL;
    gchar *want = NULL;
    const gchar *sn = GST_PAD_NAME(src);
    if (g_str_has_prefix(sn, "src")) want = g_strconcat("sink", sn + 3, NULL);
    GST_OBJECT_LOCK(e);
    for (GList *l = e->sinkpads; l; l = l->next)
        if (e->numsinkpads == 1 || (want && !strcmp(GST_PAD_NAME(l->data), want))) {
            r = gst_object_ref(l->data);
            break;
        }
    GST_OBJECT_UNLOCK(e);
    g_free(want);
    return r;
}

/* WebKit's decryptor sits upstream (and only tags the samples): its input
 * caps say the stream is encrypted and with which system - TVF needs that
 * before the first encrypted sample (clear lead: the first segments are
 * plain, the pipeline has to be loaded for DRM anyway) */
static gchar *find_protection(GstTvfSink *s) {
    GstPad *pad = gst_pad_get_peer(GST_BASE_SINK_PAD(s));
    gchar *r = NULL;
    for (int hops = 0; pad && hops < 64 && !r; hops++) {
        GstPad *next = NULL;
        if ((r = caps_protection(pad))) break;
        if (GST_IS_GHOST_PAD(pad)) {
            next = gst_ghost_pad_get_target(GST_GHOST_PAD(pad));
        } else if (GST_IS_PROXY_PAD(pad)) {
            /* inside a bin: the ghost sink pad and what feeds it */
            GstProxyPad *g = gst_proxy_pad_get_internal(GST_PROXY_PAD(pad));
            if (g) {
                if (!(r = caps_protection(GST_PAD(g)))) next = gst_pad_get_peer(GST_PAD(g));
                gst_object_unref(g);
            }
        } else {
            GstElement *e = gst_pad_get_parent_element(pad);
            if (e) {
                GstPad *sp = matching_sink_pad(e, pad);
                if (sp) {
                    if (!(r = caps_protection(sp))) next = gst_pad_get_peer(sp);
                    gst_object_unref(sp);
                }
                gst_object_unref(e);
            }
        }
        gst_object_unref(pad);
        pad = next;
    }
    if (pad) gst_object_unref(pad);
    return r;
}

static gboolean buffer_bytes(const GstStructure *st, const char *field, guint8 *out, guint max, guint *len) {
    const GValue *v = gst_structure_get_value(st, field);
    GstBuffer *b = v && G_VALUE_HOLDS(v, GST_TYPE_BUFFER) ? gst_value_get_buffer(v) : NULL;
    if (!b) return FALSE;
    gsize n = gst_buffer_get_size(b);
    if (n > max) return FALSE;
    *len = (guint) gst_buffer_extract(b, 0, out, n);
    return TRUE;
}

/* crypto block of an encrypted sample (tagged by tvf-ocdm.c), its subsample
 * map moved by the insertions of the conversion; FALSE = clear sample */
static gboolean crypto_block(GstTvfSink *s, GstBuffer *buf, gsize insize, gsize outsize) {
    GstProtectionMeta *pm = gst_buffer_get_protection_meta(buf);
    if (!pm || !pm->info || !gst_structure_has_name(pm->info, "application/x-tvf-cenc")) return FALSE;
    const GstStructure *st = pm->info;
    gboolean enc = TRUE;
    gst_structure_get_boolean(st, "encrypted", &enc);
    const char *sid = gst_structure_get_string(st, "tvf-session");
    const char *mode = gst_structure_get_string(st, "cipher-mode");
    guint8 scheme = mode && (!strcmp(mode, "cbcs") || !strcmp(mode, "cbc1")) ? 2 : 1;
    guint8 iv[16] = {0}, kid[16] = {0};
    guint ivl = 0, kidl = 0, ivsize = 0, crypt = 0, skip = 0, count = 0;
    gst_structure_get_uint(st, "iv_size", &ivsize);
    if (!enc || !sid) return FALSE;
    if (!(ivsize && buffer_bytes(st, "iv", iv, sizeof(iv), &ivl))) buffer_bytes(st, "constant_iv", iv, sizeof(iv), &ivl);
    buffer_bytes(st, "kid", kid, sizeof(kid), &kidl);
    gst_structure_get_uint(st, "crypt_byte_block", &crypt);
    gst_structure_get_uint(st, "skip_byte_block", &skip);
    gst_structure_get_uint(st, "subsample_count", &count);
    /* subsamples: big endian (clear u16, encrypted u32) per entry */
    g_array_set_size(s->subs, 0);
    const GValue *v = gst_structure_get_value(st, "subsamples");
    GstBuffer *sb = count && v && G_VALUE_HOLDS(v, GST_TYPE_BUFFER) ? gst_value_get_buffer(v) : NULL;
    GstMapInfo m;
    if (sb && gst_buffer_map(sb, &m, GST_MAP_READ)) {
        for (guint i = 0; i < count && (i + 1) * 6 <= m.size; i++) {
            const guint8 *e = m.data + i * 6;
            guint32 pair[2] = {(guint32) (e[0] << 8 | e[1]), (guint32) e[2] << 24 | (guint32) e[3] << 16 | e[4] << 8 | e[5]};
            g_array_append_vals(s->subs, pair, 2);
        }
        gst_buffer_unmap(sb, &m);
    }
    if (!s->subs->len) { /* whole sample encrypted */
        guint32 pair[2] = {0, (guint32) insize};
        g_array_append_vals(s->subs, pair, 2);
    }
    /* insertions go to the clear part of the subsample they fall into (length
     * fields and NAL headers are always clear); located on the input map */
    guint32 *sub = (guint32 *) s->subs->data;
    guint nsub = s->subs->len / 2;
    guint32 *add = g_newa(guint32, nsub);
    memset(add, 0, nsub * sizeof(guint32));
    gboolean placed = TRUE;
    for (guint k = 0; k + 1 < s->ins->len; k += 2) {
        guint32 off = g_array_index(s->ins, guint32, k), bytes = g_array_index(s->ins, guint32, k + 1);
        guint32 start = 0;
        guint j = 0;
        for (; j < nsub; j++) {
            if (off >= start && off <= start + sub[2 * j]) break;
            start += sub[2 * j] + sub[2 * j + 1];
        }
        if (j < nsub) add[j] += bytes;
        else placed = FALSE;
    }
    guint64 total = 0;
    for (guint j = 0; j < nsub; j++) {
        sub[2 * j] += add[j];
        total += (guint64) sub[2 * j] + sub[2 * j + 1];
    }
    if (!placed || total != outsize) {
        if (!s->bad_logged)
            g_printerr("[tvfsink] %s FEHLER: Subsamples passen nicht (%" G_GUINT64_FORMAT " statt %" G_GSIZE_FORMAT
                       " B)\n", kind(s), total, outsize);
        s->bad_logged = TRUE;
    }
    guint8 sidl = (guint8) MIN(strlen(sid), 255);
    guint32 head[4] = {0, crypt, skip, nsub};
    guint8 *h8 = (guint8 *) head;
    h8[0] = scheme, h8[1] = (guint8) ivl, h8[2] = (guint8) kidl, h8[3] = sidl;
    guint32 clen = (guint32) (sizeof(head) + ivl + kidl + sidl + nsub * 8);
    g_byte_array_append(s->pre, (const guint8 *) &clen, 4);
    g_byte_array_append(s->pre, (const guint8 *) head, sizeof(head));
    g_byte_array_append(s->pre, iv, ivl);
    g_byte_array_append(s->pre, kid, kidl);
    g_byte_array_append(s->pre, (const guint8 *) sid, sidl);
    g_byte_array_append(s->pre, (const guint8 *) sub, nsub * 8);
    if (!s->crypted++)
        g_printerr("[tvfsink] %s verschluesselt (%s, Sitzung %s, %u Subsamples, Muster %u:%u)\n", kind(s),
                   scheme == 2 ? "cbcs" : "cenc", sid, nsub, crypt, skip);
    return TRUE;
}

static const char *dump_ext(GstTvfSink *s, const GstStructure *st) {
    const char *name = gst_structure_get_name(st);
    int ver = 0;
    if (s->codec) return s->codec == 265 ? "h265" : "h264";
    if (!strcmp(name, "audio/x-ac3")) return "ac3";
    if (!strcmp(name, "audio/x-eac3")) return "eac3";
    gst_structure_get_int(st, "mpegversion", &ver);
    return ver == 1 ? "mpa" : "aac";
}

/* ------------------------------------------------------------------ */
/* TVF connection                                                      */
/* ------------------------------------------------------------------ */

static int tvf_connect(const char *name) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    socklen_t len;
    if (name[0] == '@') { /* abstract socket */
        g_strlcpy(a.sun_path + 1, name + 1, sizeof(a.sun_path) - 1);
        len = (socklen_t) (offsetof(struct sockaddr_un, sun_path) + 1 + strlen(a.sun_path + 1));
    } else {
        g_strlcpy(a.sun_path, name, sizeof(a.sun_path));
        len = sizeof(a);
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *) &a, len) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static gboolean send_all(int fd, struct iovec *iov, int n) {
    while (n > 0) {
        struct msghdr m;
        memset(&m, 0, sizeof(m));
        m.msg_iov = iov;
        m.msg_iovlen = (size_t) n;
        ssize_t r = sendmsg(fd, &m, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            return FALSE;
        }
        while (n > 0 && (size_t) r >= iov->iov_len) {
            r -= (ssize_t) iov->iov_len;
            iov++;
            n--;
        }
        if (n > 0) {
            iov->iov_base = (char *) iov->iov_base + r;
            iov->iov_len -= (size_t) r;
        }
    }
    return TRUE;
}

static gboolean send_msg(GstTvfSink *s, guint32 type, const void *a, gsize an, const void *b, gsize bn) {
    if (s->fd < 0) return FALSE;
    MsgHead h = {type, (guint32) (an + bn)};
    struct iovec iov[3] = {{&h, sizeof(h)}, {(void *) a, an}, {(void *) b, bn}};
    g_mutex_lock(&s->wlock);
    gboolean ok = send_all(s->fd, iov, 3);
    g_mutex_unlock(&s->wlock);
    if (!ok) {
        g_mutex_lock(&s->lock);
        s->dead = TRUE;
        g_cond_broadcast(&s->cond);
        g_mutex_unlock(&s->lock);
    }
    return ok;
}

static void send_json(GstTvfSink *s, guint32 type, GString *j) {
    send_msg(s, type, j->str, j->len, NULL, 0);
    g_string_free(j, TRUE);
}

static void json_str(GString *j, const char *v) {
    g_string_append_c(j, '"');
    for (const char *p = v ? v : ""; *p; p++) {
        if (*p == '"' || *p == '\\') g_string_append_c(j, '\\');
        if ((guchar) *p < 0x20) g_string_append_printf(j, "\\u%04x", *p);
        else g_string_append_c(j, *p);
    }
    g_string_append_c(j, '"');
}

static gboolean read_all(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return FALSE;
        p += r;
        n -= (size_t) r;
    }
    return TRUE;
}

/* messages from TVF: credit, position, end, error */
static gpointer reader_main(gpointer data) {
    GstTvfSink *s = data;
    for (;;) {
        MsgHead h;
        if (!read_all(s->fd, &h, sizeof(h)) || h.len > (1u << 20)) break;
        guint8 *b = g_malloc(h.len + 1);
        if (h.len && !read_all(s->fd, b, h.len)) {
            g_free(b);
            break;
        }
        b[h.len] = 0;
        gchar *err = NULL;
        g_mutex_lock(&s->lock);
        if (h.type == T_WINDOW) {
            const char *w = strstr((char *) b, "\"window\"");
            if (w && (w = strchr(w, ':'))) s->window = g_ascii_strtoull(w + 1, NULL, 10);
        } else if (h.type == T_ACK && h.len >= 16) {
            guint32 epoch;
            guint64 consumed;
            memcpy(&epoch, b, 4);
            memcpy(&consumed, b + 8, 8);
            if (epoch == s->epoch) s->consumed = consumed;
        } else if (h.type == T_POS && h.len >= 8) {
            memcpy(&s->pos, b, 8);
        } else if (h.type == T_EOSDONE && h.len >= 4) {
            guint32 epoch;
            memcpy(&epoch, b, 4);
            if (epoch == s->epoch) s->eos_done = TRUE;
        } else if (h.type == T_ERROR) {
            err = g_strdup((char *) b);
        }
        g_cond_broadcast(&s->cond);
        g_mutex_unlock(&s->lock);
        g_free(b);
        if (err) {
            g_printerr("[tvfsink] %s TVF-Fehler: %s\n", kind(s), err);
            GST_ELEMENT_ERROR(s, RESOURCE, FAILED, ("TVF: %s", err), (NULL));
            g_free(err);
        }
    }
    g_mutex_lock(&s->lock);
    s->dead = TRUE;
    g_cond_broadcast(&s->cond);
    g_mutex_unlock(&s->lock);
    return NULL;
}

/* both sinks of a pipeline form one session at TVF */
static gchar *session_key(GstTvfSink *s) {
    GstObject *o = GST_OBJECT(s);
    while (GST_OBJECT_PARENT(o)) o = GST_OBJECT_PARENT(o);
    return g_strdup_printf("%d-%p", (int) getpid(), (void *) o);
}

static GString *caps_json(GstTvfSink *s, GstCaps *caps) {
    const GstStructure *st = gst_caps_get_structure(caps, 0);
    const char *name = gst_structure_get_name(st);
    int ver = 0, w = 0, h = 0, rate = 0, ch = 0, layer = 0, fn = 0, fd = 1;
    gst_structure_get_int(st, "mpegversion", &ver);
    gst_structure_get_int(st, "width", &w);
    gst_structure_get_int(st, "height", &h);
    gst_structure_get_int(st, "rate", &rate);
    gst_structure_get_int(st, "channels", &ch);
    gst_structure_get_int(st, "layer", &layer);
    gst_structure_get_fraction(st, "framerate", &fn, &fd);
    const char *codec = s->codec == 264   ? "H264"
                        : s->codec == 265 ? "H265"
                        : !strcmp(name, "audio/x-ac3")  ? "AC3"
                        : !strcmp(name, "audio/x-eac3") ? "EAC3"
                        : ver == 1                      ? "MPA"
                                                        : "AAC";
    GString *j = g_string_new("{\"codec\":");
    json_str(j, codec);
    if (w > 0) g_string_append_printf(j, ",\"width\":%d,\"height\":%d", w, h);
    if (fn > 0 && fd > 0) g_string_append_printf(j, ",\"fps\":%.3f", (double) fn / fd);
    if (s->adts) { /* raw AAC turned into ADTS: the values of the ADTS header */
        rate = (int) aac_rates[s->adts_sfi];
        if (!ch) ch = s->adts_ch == 7 ? 8 : s->adts_ch;
        g_string_append_printf(j, ",\"aot\":%d", s->adts_profile + 1);
    }
    if (rate > 0) g_string_append_printf(j, ",\"rate\":%d", rate);
    if (ch > 0) g_string_append_printf(j, ",\"channels\":%d", ch);
    if (layer > 0) g_string_append_printf(j, ",\"layer\":%d", layer);
    if (s->protection) {
        g_string_append(j, ",\"protection\":");
        json_str(j, s->protection);
    }
    gchar *c = gst_caps_to_string(caps);
    g_string_append(j, ",\"caps\":");
    json_str(j, c);
    g_free(c);
    g_string_append_c(j, '}');
    return j;
}

/* waits for credit (or EOS end), with the preroll lock (render/wait_event).
 * unlock (pause): gst_base_sink_wait_preroll - it marks the sink prerolled
 * (have_preroll), so the PLAYING -> PAUSED that is waiting for the preroll
 * lock completes at once, and waits for PLAYING; flush -> FLUSHING. (Not
 * gst_base_sink_do_preroll: need_preroll is set only after the state change
 * got the lock, so it returns at once and the loop would spin holding it.) */
static GstFlowReturn wait_until(GstTvfSink *s, gboolean (*ready)(GstTvfSink *, gsize), gsize n) {
    g_mutex_lock(&s->lock);
    while (!s->dead && !ready(s, n)) {
        if (s->unlocked) {
            g_mutex_unlock(&s->lock);
            GstFlowReturn r = gst_base_sink_wait_preroll(GST_BASE_SINK(s));
            if (r != GST_FLOW_OK) return r;
            g_mutex_lock(&s->lock);
            continue;
        }
        g_cond_wait(&s->cond, &s->lock);
    }
    gboolean dead = s->dead;
    g_mutex_unlock(&s->lock);
    if (dead) {
        GST_ELEMENT_ERROR(s, RESOURCE, WRITE, ("Verbindung zu TVF verloren"), (NULL));
        return GST_FLOW_ERROR;
    }
    return GST_FLOW_OK;
}

/* room in the window (always for one message when nothing is outstanding) */
static gboolean has_credit(GstTvfSink *s, gsize n) {
    return s->sent <= s->consumed || s->sent - s->consumed + n <= s->window;
}

static gboolean is_eos_done(GstTvfSink *s, gsize n) {
    (void) n;
    return s->eos_done;
}

/* buffer time -> stream time, also before the segment start (after a seek
 * the first audio frame and the DTS of the first pictures lie before it) */
static gint64 stream_time(GstTvfSink *s, GstClockTime t) {
    if (!GST_CLOCK_TIME_IS_VALID(t)) return NO_TIME;
    if (!s->have_seg) return (gint64) t;
    guint64 v;
    int sign = gst_segment_to_stream_time_full(&s->seg, GST_FORMAT_TIME, t, &v);
    return sign > 0 ? (gint64) v : sign < 0 ? -(gint64) v : NO_TIME;
}

/* ------------------------------------------------------------------ */
/* GstBaseSink                                                         */
/* ------------------------------------------------------------------ */

static void send_rect_msg(GstTvfSink *s) {
    int x, y, w, h;
    if (!s->hello_sent || !s->video || !s->rectangle || sscanf(s->rectangle, "%d,%d,%d,%d", &x, &y, &w, &h) != 4) return;
    GString *j = g_string_new(NULL);
    g_string_printf(j, "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}", x, y, w, h);
    send_json(s, M_RECT, j);
}

static void send_volume_msg(GstTvfSink *s) {
    if (!s->hello_sent || s->video) return;
    GString *j = g_string_new(NULL);
    g_string_printf(j, "{\"volume\":%.3f,\"mute\":%s}", s->volume, s->mute ? "true" : "false");
    send_json(s, M_VOLUME, j);
}

static gboolean tvf_set_caps(GstBaseSink *bs, GstCaps *caps) {
    GstTvfSink *s = (GstTvfSink *) bs;
    const GstStructure *st = gst_caps_get_structure(caps, 0);
    const char *name = gst_structure_get_name(st);
    const char *fmt = gst_structure_get_string(st, "stream-format");
    const GValue *cv = gst_structure_get_value(st, "codec_data");
    GstBuffer *cd = cv && G_VALUE_HOLDS(cv, GST_TYPE_BUFFER) ? gst_value_get_buffer(cv) : NULL;
    GstMapInfo m = {0};
    gboolean ok = TRUE;
    s->codec = !strcmp(name, "video/x-h264") ? 264 : !strcmp(name, "video/x-h265") ? 265 : 0;
    s->nal_len = 0;
    s->adts = FALSE;
    g_byte_array_set_size(s->header, 0);
    if (cd && !gst_buffer_map(cd, &m, GST_MAP_READ)) cd = NULL;
    if (s->codec && g_strcmp0(fmt, "byte-stream")) {
        if (cd) ok = s->codec == 265 ? parse_hvcc(s, m.data, m.size) : parse_avcc(s, m.data, m.size);
        else s->nal_len = 4; /* avc3/hev1 without codec_data: parameter sets in-band */
    } else if (!strcmp(name, "audio/mpeg") && !g_strcmp0(fmt, "raw")) {
        ok = cd && parse_asc(s, m.data, m.size);
        s->adts = ok;
    }
    if (cd) gst_buffer_unmap(cd, &m);
    g_free(s->protection);
    s->protection = find_protection(s);
    gchar *c = gst_caps_to_string(caps);
    g_printerr("[tvfsink] %s caps %s%s%s\n", kind(s), c, s->protection ? " - verschluesselt, System " : "",
               s->protection ? s->protection : "");
    g_free(c);
    if (!ok) {
        g_printerr("[tvfsink] %s FEHLER: codec_data nicht verwertbar\n", kind(s));
    } else if (s->nal_len) {
        g_printerr("[tvfsink] %s -> byte-stream (Laengenfeld %d B, Parametersaetze %u B)\n", kind(s), s->nal_len,
                   s->header->len);
    } else if (s->adts) {
        g_printerr("[tvfsink] %s -> ADTS (Profil %d, Ratenindex %d, Kanaele %d)\n", kind(s), s->adts_profile,
                   s->adts_sfi, s->adts_ch);
    }
    if (s->fd >= 0) {
        /* HELLO only now: at start the sinks still hang in playsink's chain
         * bins, with caps both are in the pipeline (one session at TVF) */
        if (!s->hello_sent) {
            gchar *key = session_key(s);
            GString *j = g_string_new("{\"v\":1,\"session\":");
            json_str(j, key);
            g_string_append_printf(j, ",\"stream\":\"%s\"}", kind(s));
            g_free(key);
            send_json(s, M_HELLO, j);
            s->hello_sent = TRUE;
            send_rect_msg(s);
            send_volume_msg(s);
        }
        send_json(s, M_CAPS, caps_json(s, caps));
    }
    const char *dir = g_getenv("TVF_SINK_DUMP");
    if (dir && *dir && !s->dump) {
        /* one file per start (a seek after the end starts the sinks again) */
        static int runs[2];
        gchar *f = g_strdup_printf("%s/tvf-%s-%d.%s", dir, kind(s), ++runs[s->video], dump_ext(s, st));
        s->dump = fopen(f, "wb");
        g_printerr("[tvfsink] %s dump %s%s\n", kind(s), f, s->dump ? "" : " (geht nicht)");
        g_free(f);
    }
    return TRUE;
}

static gboolean tvf_event(GstBaseSink *bs, GstEvent *ev) {
    GstTvfSink *s = (GstTvfSink *) bs;
    switch (GST_EVENT_TYPE(ev)) {
    case GST_EVENT_SEGMENT: {
        const GstSegment *seg;
        gst_event_parse_segment(ev, &seg);
        if (seg->format == GST_FORMAT_TIME) {
            gst_segment_copy_into(seg, &s->seg);
            s->have_seg = TRUE;
        }
        g_printerr("[tvfsink] %s segment start %" GST_TIME_FORMAT " stop %" GST_TIME_FORMAT " time %" GST_TIME_FORMAT
                   " base %" GST_TIME_FORMAT " rate %.2f\n",
                   kind(s), GST_TIME_ARGS(seg->start), GST_TIME_ARGS(seg->stop), GST_TIME_ARGS(seg->time),
                   GST_TIME_ARGS(seg->base), seg->rate);
        break;
    }
    case GST_EVENT_FLUSH_START:
        g_printerr("[tvfsink] %s flush start\n", kind(s));
        break;
    case GST_EVENT_FLUSH_STOP:
        g_printerr("[tvfsink] %s flush stop\n", kind(s));
        if (s->fd >= 0) {
            /* new epoch: TVF drops what it has of the old one; credit starts over */
            g_mutex_lock(&s->lock);
            guint32 epoch = ++s->epoch;
            s->sent = s->consumed = 0;
            s->eos_done = FALSE;
            s->pos = -1;
            g_mutex_unlock(&s->lock);
            send_msg(s, M_FLUSH, &epoch, sizeof(epoch), NULL, 0);
        }
        break;
    case GST_EVENT_EOS:
        g_printerr("[tvfsink] %s EOS nach %" G_GUINT64_FORMAT " Puffern, %" G_GUINT64_FORMAT " Bytes, %" G_GUINT64_FORMAT
                   " Keyframes, pts %" GST_TIME_FORMAT " .. %" GST_TIME_FORMAT "\n",
                   kind(s), s->count, s->bytes, s->keys, GST_TIME_ARGS(s->first_pts), GST_TIME_ARGS(s->last_pts));
        if (s->dump) fflush(s->dump);
        break;
    case GST_EVENT_STREAM_START: {
        const gchar *id = NULL;
        gst_event_parse_stream_start(ev, &id);
        g_printerr("[tvfsink] %s stream-start %s\n", kind(s), id ? id : "-");
        break;
    }
    default:
        break;
    }
    return GST_BASE_SINK_CLASS(gst_tvf_sink_parent_class)->event(bs, ev);
}

/* EOS: posted only when the TV has played everything (TVF's EOSDONE) */
static GstFlowReturn tvf_wait_event(GstBaseSink *bs, GstEvent *ev) {
    GstTvfSink *s = (GstTvfSink *) bs;
    GstFlowReturn r = GST_BASE_SINK_CLASS(gst_tvf_sink_parent_class)->wait_event(bs, ev);
    if (r != GST_FLOW_OK || GST_EVENT_TYPE(ev) != GST_EVENT_EOS || s->fd < 0) return r;
    g_mutex_lock(&s->lock);
    guint32 epoch = s->epoch;
    s->eos_done = FALSE;
    g_mutex_unlock(&s->lock);
    send_msg(s, M_EOS, &epoch, sizeof(epoch), NULL, 0);
    r = wait_until(s, is_eos_done, 0);
    g_printerr("[tvfsink] %s Ende am TV%s\n", kind(s), r == GST_FLOW_OK ? "" : " (abgebrochen)");
    return r;
}

static GstFlowReturn tvf_render(GstBaseSink *bs, GstBuffer *buf) {
    GstTvfSink *s = (GstTvfSink *) bs;
    GstClockTime pts = GST_BUFFER_PTS(buf);
    gboolean key = !GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);
    GstMapInfo m;
    if (!gst_buffer_map(buf, &m, GST_MAP_READ)) return GST_FLOW_ERROR;
    const guint8 *data = m.data;
    gsize n = m.size;
    g_array_set_size(s->ins, 0);
    if (s->codec && s->nal_len) {
        if (!to_byte_stream(s, m.data, m.size, key) && !s->bad_logged) {
            g_printerr("[tvfsink] %s FEHLER: Zugriffseinheit mit kaputten Laengenfeldern (%" G_GSIZE_FORMAT " B)\n",
                       kind(s), m.size);
            s->bad_logged = TRUE;
        }
        data = s->out->data;
        n = s->out->len;
    } else if (s->adts) {
        guint8 h[7];
        adts_header(s, m.size, h);
        note_insert(s, 0, 7);
        g_byte_array_set_size(s->out, 0);
        g_byte_array_append(s->out, h, 7);
        g_byte_array_append(s->out, m.data, (guint) m.size);
        data = s->out->data;
        n = s->out->len;
    }
    s->count++;
    s->bytes += n;
    if (key && s->video) s->keys++;
    if (GST_CLOCK_TIME_IS_VALID(pts)) {
        if (!GST_CLOCK_TIME_IS_VALID(s->first_pts)) s->first_pts = pts;
        s->last_pts = pts;
    }
    if (s->dump) {
        /* written at once: the WebProcess may be killed without a chance to flush */
        fwrite(data, 1, n, s->dump);
        fflush(s->dump);
    }
    if (g_log_all < 0) g_log_all = g_strcmp0(g_getenv("TVF_SINK_LOG"), "all") == 0;
    gboolean log = g_log_all || (s->fd >= 0 ? s->count <= 3
                                            : s->count <= 30 || (s->video && key) || s->count % 200 == 0);
    if (log) {
        char extra[96] = "";
        if (s->codec) {
            char t[80];
            nal_types(s, data, n, t, sizeof(t));
            g_snprintf(extra, sizeof(extra), " NAL %s", t);
        } else if (n >= 2) {
            g_snprintf(extra, sizeof(extra), " %02x%02x", data[0], data[1]);
        }
        g_printerr("[tvfsink] %s #%" G_GUINT64_FORMAT " pts %" GST_TIME_FORMAT " dts %" GST_TIME_FORMAT
                   " dur %" GST_TIME_FORMAT " %" G_GSIZE_FORMAT " B%s%s\n",
                   kind(s), s->count, GST_TIME_ARGS(pts), GST_TIME_ARGS(GST_BUFFER_DTS(buf)),
                   GST_TIME_ARGS(GST_BUFFER_DURATION(buf)), n, key ? " key" : "", extra);
    }
    GstFlowReturn r = GST_FLOW_OK;
    if (s->fd >= 0) {
        r = wait_until(s, has_credit, n);
        if (r == GST_FLOW_OK) {
            DataHead d;
            d.pts = stream_time(s, pts);
            d.dts = stream_time(s, GST_BUFFER_DTS(buf));
            d.dur = GST_BUFFER_DURATION_IS_VALID(buf) ? (gint64) GST_BUFFER_DURATION(buf) : NO_TIME;
            /* head, then (encrypted) the crypto block, then the data */
            g_byte_array_set_size(s->pre, sizeof(d));
            gboolean crypted = crypto_block(s, buf, m.size, n);
            d.flags = (key ? DATA_KEY : 0) | (crypted ? DATA_CRYPTO : 0);
            g_mutex_lock(&s->lock);
            d.epoch = s->epoch;
            s->sent += n;
            g_mutex_unlock(&s->lock);
            memcpy(s->pre->data, &d, sizeof(d));
            if (!send_msg(s, M_DATA, s->pre->data, s->pre->len, data, n)) {
                GST_ELEMENT_ERROR(s, RESOURCE, WRITE, ("Verbindung zu TVF verloren"), (NULL));
                r = GST_FLOW_ERROR;
            }
        }
    }
    gst_buffer_unmap(buf, &m);
    return r;
}

static gboolean tvf_unlock(GstBaseSink *bs) {
    GstTvfSink *s = (GstTvfSink *) bs;
    g_mutex_lock(&s->lock);
    s->unlocked = TRUE;
    g_cond_broadcast(&s->cond);
    g_mutex_unlock(&s->lock);
    return TRUE;
}

static gboolean tvf_unlock_stop(GstBaseSink *bs) {
    GstTvfSink *s = (GstTvfSink *) bs;
    g_mutex_lock(&s->lock);
    s->unlocked = FALSE;
    g_mutex_unlock(&s->lock);
    return TRUE;
}

static gboolean tvf_start(GstBaseSink *bs) {
    GstTvfSink *s = (GstTvfSink *) bs;
    /* never drop: every access unit has to reach the TV's decoder (WebKit
     * sets max-lateness 100 ms with QoS on video sinks) */
    gst_base_sink_set_max_lateness(bs, -1);
    gst_base_sink_set_qos_enabled(bs, FALSE);
    s->count = s->bytes = s->keys = 0;
    s->first_pts = s->last_pts = GST_CLOCK_TIME_NONE;
    s->bad_logged = FALSE;
    s->have_seg = FALSE;
    s->hello_sent = s->dead = s->unlocked = s->eos_done = FALSE;
    s->epoch = 0;
    s->sent = s->consumed = 0;
    s->window = s->video ? 4u << 20 : 512u << 10; /* until TVF sends its own */
    s->pos = -1;
    const char *sock = g_getenv("TVF_MEDIA_SOCK");
    if (sock && *sock) {
        s->fd = tvf_connect(sock);
        if (s->fd < 0) {
            g_printerr("[tvfsink] %s TVF nicht erreichbar (%s): %s - nur Mitschrift\n", kind(s), sock,
                       g_strerror(errno));
        } else {
            /* the TV keeps the time */
            gst_base_sink_set_sync(bs, FALSE);
            s->reader = g_thread_new("tvfsink", reader_main, s);
        }
    }
    g_printerr("[tvfsink] %s start%s\n", kind(s), s->fd >= 0 ? " (an TVF)" : "");
    return TRUE;
}

static gboolean tvf_stop(GstBaseSink *bs) {
    GstTvfSink *s = (GstTvfSink *) bs;
    g_printerr("[tvfsink] %s stop nach %" G_GUINT64_FORMAT " Puffern, %" G_GUINT64_FORMAT " Bytes\n", kind(s), s->count,
               s->bytes);
    if (s->fd >= 0) {
        shutdown(s->fd, SHUT_RDWR);
        if (s->reader) g_thread_join(s->reader);
        s->reader = NULL;
        close(s->fd);
        s->fd = -1;
    }
    if (s->dump) {
        fclose(s->dump);
        s->dump = NULL;
    }
    return TRUE;
}

static gboolean tvf_unlock(GstBaseSink *bs);

/* state for TVF: play/pause of the TV's pipeline follows WebKit's. With TVF
 * the picture is on the TV, so pausing needs no new buffer in the sink - and
 * waiting for one (async) can deadlock: the sink that has passed on
 * everything waits for data stuck in the demuxer behind the other stream,
 * whose sink sits paused. So PLAYING -> PAUSED is synchronous; async stays
 * for the first preroll, for flushes (WebKit's loading and seeked wait for
 * those) and for PAUSED -> PLAYING (data flows again then). */
static GstStateChangeReturn tvf_change_state(GstElement *e, GstStateChange t) {
    GstTvfSink *s = (GstTvfSink *) e;
    GstBaseSink *bs = GST_BASE_SINK(e);
    gboolean pause = t == GST_STATE_CHANGE_PLAYING_TO_PAUSED && s->fd >= 0;
    if (t == GST_STATE_CHANGE_PAUSED_TO_PLAYING && s->fd >= 0) send_msg(s, M_STATE, "{\"playing\":true}", 16, NULL, 0);
    if (pause) {
        /* set_async_enabled takes the preroll lock, which a render waiting
         * for credit holds: wake it first (it goes into wait_preroll and lets
         * go of the lock) */
        tvf_unlock(bs);
        gst_base_sink_set_async_enabled(bs, FALSE);
    }
    GstStateChangeReturn r = GST_ELEMENT_CLASS(gst_tvf_sink_parent_class)->change_state(e, t);
    if (pause) {
        gst_base_sink_set_async_enabled(bs, TRUE);
        send_msg(s, M_STATE, "{\"playing\":false}", 17, NULL, 0);
    }
    return r;
}

/* position (WebKit's currentTime): the picture on the TV, before the first
 * report the start of the segment */
static gboolean tvf_query(GstElement *e, GstQuery *q) {
    GstTvfSink *s = (GstTvfSink *) e;
    if (GST_QUERY_TYPE(q) == GST_QUERY_POSITION && s->fd >= 0) {
        GstFormat f;
        gst_query_parse_position(q, &f, NULL);
        g_mutex_lock(&s->lock);
        gint64 p = s->pos;
        g_mutex_unlock(&s->lock);
        if (p < 0 && s->have_seg) p = (gint64) s->seg.time;
        if (f == GST_FORMAT_TIME && p >= 0) {
            gst_query_set_position(q, GST_FORMAT_TIME, p);
            return TRUE;
        }
    }
    return GST_ELEMENT_CLASS(gst_tvf_sink_parent_class)->query(e, q);
}

static void tvf_set_property(GObject *o, guint id, const GValue *v, GParamSpec *ps) {
    GstTvfSink *s = (GstTvfSink *) o;
    if (id == PROP_RECTANGLE) {
        /* WebKit sets it again on every repaint of the video (~10/s): only changes go on */
        const char *r = g_value_get_string(v);
        if (!g_strcmp0(r, s->rectangle)) return;
        g_free(s->rectangle);
        s->rectangle = g_strdup(r);
        g_printerr("[tvfsink] %s rectangle %s\n", kind(s), s->rectangle ? s->rectangle : "-");
        send_rect_msg(s);
    } else if (id == PROP_VOLUME) {
        s->volume = g_value_get_double(v);
        g_printerr("[tvfsink] %s volume %.2f\n", kind(s), s->volume);
        send_volume_msg(s);
    } else if (id == PROP_MUTE) {
        s->mute = g_value_get_boolean(v);
        g_printerr("[tvfsink] %s mute %d\n", kind(s), s->mute);
        send_volume_msg(s);
    } else {
        G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, ps);
    }
}

static void tvf_get_property(GObject *o, guint id, GValue *v, GParamSpec *ps) {
    GstTvfSink *s = (GstTvfSink *) o;
    if (id == PROP_RECTANGLE) g_value_set_string(v, s->rectangle);
    else if (id == PROP_VOLUME) g_value_set_double(v, s->volume);
    else if (id == PROP_MUTE) g_value_set_boolean(v, s->mute);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, ps);
}

static void tvf_finalize(GObject *o) {
    GstTvfSink *s = (GstTvfSink *) o;
    g_free(s->rectangle);
    g_byte_array_unref(s->header);
    g_byte_array_unref(s->out);
    g_byte_array_unref(s->pre);
    g_array_unref(s->ins);
    g_array_unref(s->subs);
    g_free(s->protection);
    if (s->dump) fclose(s->dump);
    g_mutex_clear(&s->wlock);
    g_mutex_clear(&s->lock);
    g_cond_clear(&s->cond);
    G_OBJECT_CLASS(gst_tvf_sink_parent_class)->finalize(o);
}

static void gst_tvf_sink_class_init(GstTvfSinkClass *k) {
    GObjectClass *oc = G_OBJECT_CLASS(k);
    GstElementClass *ec = GST_ELEMENT_CLASS(k);
    GstBaseSinkClass *bc = GST_BASE_SINK_CLASS(k);
    oc->set_property = tvf_set_property;
    oc->get_property = tvf_get_property;
    oc->finalize = tvf_finalize;
    g_object_class_install_property(oc, PROP_RECTANGLE,
                                    g_param_spec_string("rectangle", "Rectangle",
                                                        "Video window x,y,w,h (from WebKit's hole punch quirk)", NULL,
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(oc, PROP_VOLUME,
                                    g_param_spec_double("volume", "Volume", "Volume (1.0 = 100 %)", 0.0, 10.0, 1.0,
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(oc, PROP_MUTE,
                                    g_param_spec_boolean("mute", "Mute", "Mute", FALSE,
                                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    ec->change_state = tvf_change_state;
    ec->query = tvf_query;
    bc->set_caps = tvf_set_caps;
    bc->event = tvf_event;
    bc->wait_event = tvf_wait_event;
    bc->render = tvf_render;
    bc->unlock = tvf_unlock;
    bc->unlock_stop = tvf_unlock_stop;
    bc->start = tvf_start;
    bc->stop = tvf_stop;
}

static void gst_tvf_sink_init(GstTvfSink *s) {
    s->first_pts = s->last_pts = GST_CLOCK_TIME_NONE;
    s->volume = 1.0;
    s->header = g_byte_array_new();
    s->out = g_byte_array_new();
    s->pre = g_byte_array_new();
    s->ins = g_array_new(FALSE, FALSE, sizeof(guint32));
    s->subs = g_array_new(FALSE, FALSE, sizeof(guint32));
    s->fd = -1;
    s->pos = -1;
    g_mutex_init(&s->wlock);
    g_mutex_init(&s->lock);
    g_cond_init(&s->cond);
    gst_segment_init(&s->seg, GST_FORMAT_TIME);
}

/* video and audio: the same sink with its own caps and klass */
typedef GstTvfSink GstTvfVideoSink;
typedef GstTvfSinkClass GstTvfVideoSinkClass;
typedef GstTvfSink GstTvfAudioSink;
typedef GstTvfSinkClass GstTvfAudioSinkClass;
GType gst_tvf_video_sink_get_type(void);
GType gst_tvf_audio_sink_get_type(void);
G_DEFINE_TYPE(GstTvfVideoSink, gst_tvf_video_sink, gst_tvf_sink_get_type())
/* GstStreamVolume (only marks the volume/mute properties): playsink then
 * leaves volume and mute to the sink instead of looking for a volume element */
G_DEFINE_TYPE_WITH_CODE(GstTvfAudioSink, gst_tvf_audio_sink, gst_tvf_sink_get_type(),
                        G_IMPLEMENT_INTERFACE(GST_TYPE_STREAM_VOLUME, NULL))

static GstStaticPadTemplate video_template =
    GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS(VIDEO_CAPS));
static GstStaticPadTemplate audio_template =
    GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS(AUDIO_CAPS));

static void gst_tvf_video_sink_class_init(GstTvfVideoSinkClass *k) {
    GstElementClass *ec = GST_ELEMENT_CLASS(k);
    gst_element_class_add_static_pad_template(ec, &video_template);
    gst_element_class_set_static_metadata(ec, "TVF video sink", "Decoder/Video/Sink/Video",
                                          "Encoded video to TVF (TV hardware decoder)", "L-S-D");
}
static void gst_tvf_video_sink_init(GstTvfVideoSink *s) { s->video = TRUE; }

static void gst_tvf_audio_sink_class_init(GstTvfAudioSinkClass *k) {
    GstElementClass *ec = GST_ELEMENT_CLASS(k);
    gst_element_class_add_static_pad_template(ec, &audio_template);
    gst_element_class_set_static_metadata(ec, "TVF audio sink", "Decoder/Audio/Sink/Audio",
                                          "Encoded audio to TVF (TV hardware decoder)", "L-S-D");
}
static void gst_tvf_audio_sink_init(GstTvfAudioSink *s) { s->video = FALSE; }

static gboolean plugin_init(GstPlugin *p) {
    /* rank just above MARGINAL: WebKit's quirk and registry scanner need
     * more than MARGINAL, nothing else should pick the sinks first */
    return gst_element_register(p, "tvfvideosink", GST_RANK_MARGINAL + 1, gst_tvf_video_sink_get_type()) &&
           gst_element_register(p, "tvfaudiosink", GST_RANK_MARGINAL + 1, gst_tvf_audio_sink_get_type());
}

#define PACKAGE "tvf"
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, tvf, "TVF sinks (encoded streams to the TV's decoder)",
                  plugin_init, "0.1", "LGPL", "tvf", "https://github.com/L-S-D/wpe-webos")

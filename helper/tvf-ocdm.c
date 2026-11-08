/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * libocdm for WPE WebKit's Thunder CDM (ENABLE_THUNDER): an OpenCDM client
 * of TVF.
 *
 * TVF hosts the TV's CDMs (Chromium library CDMs: Widevine, PlayReady) and
 * feeds the decoder; this library only forwards. One connection per process
 * to TVF_MEDIA_SOCK (the media socket, "@name" = abstract; the first message
 * says "drm"), requests with an id and a synchronous reply, events (license
 * request, key status, close) from TVF on a reader thread that calls the
 * session callbacks - WebKit's callbacks only post to its main thread.
 * Protocol: ../PROTOCOL-MEDIA.md, "DRM".
 *
 * Decryption: opencdm_gstreamer_session_decrypt_buffer leaves the sample
 * encrypted and adds a GstProtectionMeta "application/x-tvf-cenc" (the
 * demuxer's fields plus "tvf-session"); WebKit's decryptor then drops the
 * demuxer's meta, the TVF sinks send ours along with the sample, and TVF's
 * CDM turns it into the TV's secure-video-path input. Same as rialto-ocdm.
 *
 * Without TVF (no TVF_MEDIA_SOCK, or not reachable) no key system is
 * supported - WebKit then has no EME, as before.
 */
#define _GNU_SOURCE
#include "ocdm/open_cdm.h"
#include "ocdm/open_cdm_adapter.h"

#include <errno.h>
#ifndef TVF_OCDM_NO_GST /* host test without GStreamer (sdk/tests/eme) */
#include <gst/gst.h>
#include <gst/gstprotection.h>
#else
#include <glib.h>
#endif
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum { M_HELLO = 1 };
enum { D_OPEN = 20, D_CLOSE, D_CERT, D_CREATE, D_LOAD, D_UPDATE, D_REMOVE, D_SCLOSE, D_RELEASE, D_QUERY };
enum { R_REPLY = 120, R_MESSAGE, R_KEYS, R_ERROR, R_CLOSED };

#define MAX_KEYS 64
#define MAX_KID 32

struct OpenCDMSystem {
    uint32_t id;
    char ks[64];
};

struct OpenCDMSession {
    struct OpenCDMSession *next;
    int refs;
    uint32_t id, sys;
    char ks[64];
    char sid[256];
    OpenCDMSessionCallbacks cb; /* copied: WebKit's struct lives in its session object */
    void *user;
    int silent;   /* closed: no callbacks any more */
    int activity; /* a message or key change since the last update/load */
    int nkeys;
    struct {
        uint8_t len;
        uint8_t id[MAX_KID];
        KeyStatus st;
    } keys[MAX_KEYS];
};

typedef struct pending {
    struct pending *next;
    uint32_t req;
    int done;
    uint32_t status;
    char text[256];
} pending;

static struct {
    pthread_mutex_t lock; /* everything below */
    pthread_cond_t cond;  /* replies, key changes */
    pthread_mutex_t wlock; /* socket writes */
    int fd, tried;
    uint32_t next_id, next_req;
    struct OpenCDMSession *sessions;
    pending *pend;
} G = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, -1, 0, 1, 1, NULL, NULL};

static void lg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void lg(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[ocdm] %s\n", b);
}

/* ---------------- wire format ---------------- */

static void put_u32(GByteArray *a, uint32_t v) { g_byte_array_append(a, (const guint8 *) &v, 4); }
static void put_bytes(GByteArray *a, const void *d, uint32_t n) {
    put_u32(a, n);
    if (n) g_byte_array_append(a, d, n);
}
static void put_str(GByteArray *a, const char *s) { put_bytes(a, s ? s : "", s ? (uint32_t) strlen(s) : 0); }

typedef struct {
    const uint8_t *p, *end;
    int bad;
} rd;
static uint32_t get_u32(rd *r) {
    uint32_t v = 0;
    if (r->end - r->p < 4) {
        r->bad = 1;
        return 0;
    }
    memcpy(&v, r->p, 4);
    r->p += 4;
    return v;
}
static const uint8_t *get_bytes(rd *r, uint32_t *n) {
    *n = get_u32(r);
    if (r->bad || (uint32_t) (r->end - r->p) < *n) {
        r->bad = 1;
        *n = 0;
        return NULL;
    }
    const uint8_t *d = r->p;
    r->p += *n;
    return d;
}

static int send_msg(uint32_t type, GByteArray *a) {
    uint32_t h[2] = {type, a ? a->len : 0};
    struct iovec iov[2] = {{h, sizeof(h)}, {a ? a->data : NULL, a ? a->len : 0}};
    struct msghdr m = {0};
    m.msg_iov = iov;
    m.msg_iovlen = a && a->len ? 2 : 1;
    size_t total = sizeof(h) + (a ? a->len : 0), done = 0;
    pthread_mutex_lock(&G.wlock);
    int fd = G.fd, ok = fd >= 0;
    while (ok && done < total) {
        ssize_t r = sendmsg(fd, &m, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            ok = 0;
            break;
        }
        done += (size_t) r;
        while (m.msg_iovlen && (size_t) r >= m.msg_iov->iov_len) {
            r -= (ssize_t) m.msg_iov->iov_len;
            m.msg_iov++;
            m.msg_iovlen--;
        }
        if (m.msg_iovlen && r) {
            m.msg_iov->iov_base = (char *) m.msg_iov->iov_base + r;
            m.msg_iov->iov_len -= (size_t) r;
        }
    }
    pthread_mutex_unlock(&G.wlock);
    return ok;
}

static int read_all(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        p += r;
        n -= (size_t) r;
    }
    return 1;
}

/* ---------------- sessions ---------------- */

static struct OpenCDMSession *find_session(uint32_t id) {
    for (struct OpenCDMSession *s = G.sessions; s; s = s->next)
        if (s->id == id) return s;
    return NULL;
}

static void session_unref_locked(struct OpenCDMSession *s) {
    if (--s->refs > 0) return;
    for (struct OpenCDMSession **p = &G.sessions; *p; p = &(*p)->next)
        if (*p == s) {
            *p = s->next;
            break;
        }
    free(s);
}

/* PlayReady: the 4-2-2 GUID parts of a key id in the other byte order */
static void swap_guid(const uint8_t *in, uint8_t *out) {
    memcpy(out, in, 16);
    out[0] = in[3], out[1] = in[2], out[2] = in[1], out[3] = in[0];
    out[4] = in[5], out[5] = in[4];
    out[6] = in[7], out[7] = in[6];
}

static int key_index(const struct OpenCDMSession *s, const uint8_t *kid, uint8_t len) {
    for (int i = 0; i < s->nkeys; i++)
        if (s->keys[i].len == len && !memcmp(s->keys[i].id, kid, len)) return i;
    if (len == 16) {
        uint8_t sw[16];
        swap_guid(kid, sw);
        for (int i = 0; i < s->nkeys; i++)
            if (s->keys[i].len == 16 && !memcmp(s->keys[i].id, sw, 16)) return i;
    }
    return -1;
}

/* ---------------- connection ---------------- */

static void fail_all_locked(void) {
    for (pending *p = G.pend; p; p = p->next)
        if (!p->done) {
            p->done = 1;
            p->status = 1;
            snprintf(p->text, sizeof(p->text), "TVF getrennt");
        }
    pthread_cond_broadcast(&G.cond);
}

static void on_event(uint32_t type, rd *r) {
    uint32_t sess = get_u32(r);
    pthread_mutex_lock(&G.lock);
    struct OpenCDMSession *s = find_session(sess);
    if (!s || s->silent) {
        pthread_mutex_unlock(&G.lock);
        return;
    }
    s->refs++;
    OpenCDMSessionCallbacks cb = s->cb;
    void *user = s->user;
    if (type == R_MESSAGE) {
        uint32_t mtype = get_u32(r), ul, ml;
        const uint8_t *url = get_bytes(r, &ul);
        const uint8_t *msg = get_bytes(r, &ml);
        s->activity = 1;
        pthread_mutex_unlock(&G.lock);
        if (!r->bad && cb.process_challenge_callback) {
            /* WebKit: "<type>:Type:" in front tells it the message type */
            size_t n = ml + 7;
            if (n > 0xffff) {
                lg("%s: Nachricht zu gross (%u Bytes)", s->sid, ml);
            } else {
                uint8_t *b = malloc(n);
                char url0[512];
                snprintf(url0, sizeof(url0), "%.*s", (int) ul, url ? (const char *) url : "");
                if (b) {
                    snprintf((char *) b, 8, "%u:Type:", mtype > 9 ? 0 : mtype);
                    memcpy(b + 7, msg, ml);
                    lg("%s: Nachricht Typ %u, %u Bytes", s->sid, mtype, ml);
                    cb.process_challenge_callback(s, user, url0, b, (uint16_t) n);
                    free(b);
                }
            }
        }
        pthread_mutex_lock(&G.lock);
    } else if (type == R_KEYS) {
        uint32_t count = get_u32(r);
        uint8_t ids[MAX_KEYS][MAX_KID], lens[MAX_KEYS];
        int n = 0;
        for (uint32_t i = 0; i < count && !r->bad; i++) {
            uint32_t kl;
            const uint8_t *kid = get_bytes(r, &kl);
            KeyStatus st = (KeyStatus) get_u32(r);
            if (r->bad || !kl || kl > MAX_KID) continue;
            int k = key_index(s, kid, (uint8_t) kl);
            if (k < 0 && s->nkeys < MAX_KEYS) {
                k = s->nkeys++;
                s->keys[k].len = (uint8_t) kl;
                memcpy(s->keys[k].id, kid, kl);
            }
            if (k >= 0) s->keys[k].st = st;
            if (n < MAX_KEYS) {
                memcpy(ids[n], kid, kl);
                lens[n++] = (uint8_t) kl;
            }
        }
        s->activity = 1;
        pthread_cond_broadcast(&G.cond); /* opencdm_get_system_session */
        pthread_mutex_unlock(&G.lock);
        lg("%s: %d Schluessel", s->sid, n);
        for (int i = 0; i < n; i++)
            if (cb.key_update_callback) cb.key_update_callback(s, user, ids[i], lens[i]);
        if (cb.keys_updated_callback) cb.keys_updated_callback(s, user);
        pthread_mutex_lock(&G.lock);
    } else if (type == R_ERROR) {
        uint32_t tl;
        const uint8_t *t = get_bytes(r, &tl);
        char text[512];
        snprintf(text, sizeof(text), "%.*s", (int) tl, t ? (const char *) t : "");
        pthread_mutex_unlock(&G.lock);
        lg("%s: Fehler %s", s->sid, text);
        if (cb.error_message_callback) cb.error_message_callback(s, user, text);
        pthread_mutex_lock(&G.lock);
    } else if (type == R_CLOSED) {
        /* closed by the CDM (e.g. expired): keys are gone */
        uint8_t ids[MAX_KEYS][MAX_KID], lens[MAX_KEYS];
        int n = s->nkeys;
        for (int i = 0; i < n; i++) {
            s->keys[i].st = Released;
            memcpy(ids[i], s->keys[i].id, s->keys[i].len);
            lens[i] = s->keys[i].len;
        }
        pthread_mutex_unlock(&G.lock);
        lg("%s: vom CDM geschlossen", s->sid);
        for (int i = 0; i < n; i++)
            if (cb.key_update_callback) cb.key_update_callback(s, user, ids[i], lens[i]);
        if (n && cb.keys_updated_callback) cb.keys_updated_callback(s, user);
        pthread_mutex_lock(&G.lock);
    }
    session_unref_locked(s);
    pthread_mutex_unlock(&G.lock);
}

static void *reader_main(void *arg) {
    int fd = (int) (intptr_t) arg;
    for (;;) {
        uint32_t h[2];
        if (!read_all(fd, h, sizeof(h)) || h[1] > (4u << 20)) break;
        uint8_t *b = malloc(h[1] ? h[1] : 1);
        if (!b || (h[1] && !read_all(fd, b, h[1]))) {
            free(b);
            break;
        }
        rd r = {b, b + h[1], 0};
        if (h[0] == R_REPLY) {
            uint32_t req = get_u32(&r), status = get_u32(&r), tl;
            const uint8_t *t = get_bytes(&r, &tl);
            pthread_mutex_lock(&G.lock);
            for (pending *p = G.pend; p; p = p->next)
                if (p->req == req) {
                    p->done = 1;
                    p->status = r.bad ? 1 : status;
                    snprintf(p->text, sizeof(p->text), "%.*s", (int) tl, t ? (const char *) t : "");
                }
            pthread_cond_broadcast(&G.cond);
            pthread_mutex_unlock(&G.lock);
        } else if (h[0] >= R_MESSAGE && h[0] <= R_CLOSED) {
            on_event(h[0], &r);
        }
        free(b);
    }
    lg("Verbindung zu TVF beendet");
    pthread_mutex_lock(&G.wlock);
    pthread_mutex_lock(&G.lock);
    if (G.fd == fd) G.fd = -1;
    fail_all_locked();
    pthread_mutex_unlock(&G.lock);
    pthread_mutex_unlock(&G.wlock);
    close(fd);
    return NULL;
}

/* connected (once; under G.lock) */
static int ensure_conn_locked(void) {
    if (G.fd >= 0) return 1;
    if (G.tried) return 0;
    G.tried = 1;
    const char *name = getenv("TVF_MEDIA_SOCK");
    if (!name || !*name) return 0;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    socklen_t al;
    if (name[0] == '@') {
        size_t n = strnlen(name + 1, sizeof(a.sun_path) - 1);
        memcpy(a.sun_path + 1, name + 1, n);
        al = (socklen_t) (offsetof(struct sockaddr_un, sun_path) + 1 + n);
    } else {
        snprintf(a.sun_path, sizeof(a.sun_path), "%s", name);
        al = sizeof(a);
    }
    if (connect(fd, (struct sockaddr *) &a, al) != 0) {
        lg("TVF nicht erreichbar (%s): %s", name, strerror(errno));
        close(fd);
        return 0;
    }
    /* HELLO before anything else can be sent (G.fd is not set yet) */
    const char *hello = "{\"drm\":1}";
    uint32_t hh[2] = {M_HELLO, (uint32_t) strlen(hello)};
    struct iovec iov[2] = {{hh, sizeof(hh)}, {(void *) hello, strlen(hello)}};
    int ok = writev(fd, iov, 2) == (ssize_t) (sizeof(hh) + strlen(hello));
    if (ok) G.fd = fd;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (!ok || pthread_create(&t, &at, reader_main, (void *) (intptr_t) fd) != 0) {
        G.fd = -1;
        close(fd);
        ok = 0;
    }
    pthread_attr_destroy(&at);
    if (ok) lg("mit TVF verbunden");
    return ok;
}

static int ensure_conn(void) {
    pthread_mutex_lock(&G.lock);
    int ok = ensure_conn_locked();
    pthread_mutex_unlock(&G.lock);
    return ok;
}

/* request with reply; the payload starts with the request id (filled in here) */
static uint32_t request(uint32_t type, GByteArray *a, int timeout_ms, char *text, size_t tn) {
    pending p = {0};
    pthread_mutex_lock(&G.lock);
    if (!ensure_conn_locked()) {
        pthread_mutex_unlock(&G.lock);
        if (text) snprintf(text, tn, "TVF nicht verbunden");
        return 1;
    }
    p.req = G.next_req++;
    memcpy(a->data, &p.req, 4);
    p.next = G.pend;
    G.pend = &p;
    pthread_mutex_unlock(&G.lock);
    int sent = send_msg(type, a);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000;
    until.tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&G.lock);
    while (sent && !p.done)
        if (pthread_cond_timedwait(&G.cond, &G.lock, &until) == ETIMEDOUT) break;
    for (pending **q = &G.pend; *q; q = &(*q)->next)
        if (*q == &p) {
            *q = p.next;
            break;
        }
    pthread_mutex_unlock(&G.lock);
    if (!p.done) {
        if (text) snprintf(text, tn, "%s", sent ? "keine Antwort von TVF" : "TVF getrennt");
        return 1;
    }
    if (text) snprintf(text, tn, "%s", p.text);
    return p.status;
}

static GByteArray *req_new(void) {
    GByteArray *a = g_byte_array_new();
    put_u32(a, 0); /* request id */
    return a;
}

/* ---------------- API ---------------- */

OpenCDMError opencdm_is_type_supported(const char keySystem[], const char mimeType[]) {
    if (!keySystem || !ensure_conn()) return ERROR_KEYSYSTEM_NOT_SUPPORTED;
    GByteArray *a = req_new();
    put_str(a, keySystem);
    put_str(a, mimeType);
    char t[128];
    uint32_t st = request(D_QUERY, a, 5000, t, sizeof(t));
    g_byte_array_unref(a);
    lg("%s \"%s\": %s%s%s", keySystem, mimeType ? mimeType : "", st == 0 ? "ja" : "nein", t[0] ? " - " : "", t);
    return st == 0 ? ERROR_NONE : ERROR_KEYSYSTEM_NOT_SUPPORTED;
}

struct OpenCDMSystem *opencdm_create_system(const char keySystem[]) {
    /* never NULL: WebKit dereferences it; without TVF the calls fail */
    struct OpenCDMSystem *sys = calloc(1, sizeof(*sys));
    if (!sys) return NULL;
    snprintf(sys->ks, sizeof(sys->ks), "%s", keySystem ? keySystem : "");
    pthread_mutex_lock(&G.lock);
    sys->id = G.next_id++;
    pthread_mutex_unlock(&G.lock);
    if (ensure_conn()) {
        GByteArray *a = g_byte_array_new();
        put_u32(a, sys->id);
        put_str(a, sys->ks);
        send_msg(D_OPEN, a);
        g_byte_array_unref(a);
    }
    return sys;
}

OpenCDMError opencdm_destruct_system(struct OpenCDMSystem *system) {
    if (!system) return ERROR_INVALID_ACCESSOR;
    GByteArray *a = g_byte_array_new();
    put_u32(a, system->id);
    send_msg(D_CLOSE, a);
    g_byte_array_unref(a);
    free(system);
    return ERROR_NONE;
}

OpenCDMBool opencdm_system_supports_server_certificate(struct OpenCDMSystem *system) {
    return system && !strcmp(system->ks, "com.widevine.alpha") ? OPENCDM_BOOL_TRUE : OPENCDM_BOOL_FALSE;
}

OpenCDMError opencdm_system_set_server_certificate(struct OpenCDMSystem *system, const uint8_t serverCertificate[],
                                                   const uint16_t serverCertificateLength) {
    if (!system) return ERROR_INVALID_ACCESSOR;
    GByteArray *a = req_new();
    put_u32(a, system->id);
    put_str(a, system->ks);
    put_bytes(a, serverCertificate, serverCertificateLength);
    char t[256];
    uint32_t st = request(D_CERT, a, 15000, t, sizeof(t));
    g_byte_array_unref(a);
    if (st) lg("Serverzertifikat abgelehnt: %s", t);
    return st ? ERROR_FAIL : ERROR_NONE;
}

OpenCDMError opencdm_construct_session(struct OpenCDMSystem *system, const LicenseType licenseType,
                                       const char initDataType[], const uint8_t initData[],
                                       const uint16_t initDataLength, const uint8_t CDMData[],
                                       const uint16_t CDMDataLength, OpenCDMSessionCallbacks *callbacks,
                                       void *userData, struct OpenCDMSession **session) {
    *session = NULL;
    if (!system) return ERROR_INVALID_ACCESSOR;
    struct OpenCDMSession *s = calloc(1, sizeof(*s));
    if (!s) return ERROR_OUT_OF_MEMORY;
    s->refs = 1;
    s->sys = system->id;
    snprintf(s->ks, sizeof(s->ks), "%s", system->ks);
    if (callbacks) s->cb = *callbacks;
    s->user = userData;
    pthread_mutex_lock(&G.lock);
    s->id = G.next_id++;
    s->next = G.sessions;
    G.sessions = s;
    pthread_mutex_unlock(&G.lock);
    if (!initDataLength && CDMDataLength) {
        /* persistent session to load: WebKit passes its id here, opencdm_session_load loads it */
        snprintf(s->sid, sizeof(s->sid), "%.*s", (int) CDMDataLength, (const char *) CDMData);
        *session = s;
        return ERROR_NONE;
    }
    GByteArray *a = req_new();
    put_u32(a, system->id);
    put_str(a, system->ks);
    put_u32(a, s->id);
    put_u32(a, (uint32_t) licenseType);
    put_str(a, initDataType);
    put_bytes(a, initData, initDataLength);
    char t[256];
    uint32_t st = request(D_CREATE, a, 25000, t, sizeof(t));
    g_byte_array_unref(a);
    pthread_mutex_lock(&G.lock);
    if (st) {
        s->silent = 1;
        session_unref_locked(s);
        pthread_mutex_unlock(&G.lock);
        lg("Sitzung (%s, %s) abgelehnt: %s", system->ks, initDataType ? initDataType : "-", t);
        return st == ERROR_KEYSYSTEM_NOT_SUPPORTED ? ERROR_KEYSYSTEM_NOT_SUPPORTED : ERROR_FAIL;
    }
    snprintf(s->sid, sizeof(s->sid), "%s", t);
    pthread_mutex_unlock(&G.lock);
    lg("Sitzung %s (%s)", s->sid, system->ks);
    *session = s;
    return ERROR_NONE;
}

OpenCDMError opencdm_destruct_session(struct OpenCDMSession *session) {
    if (!session) return ERROR_INVALID_ACCESSOR;
    pthread_mutex_lock(&G.lock);
    int last = session->refs == 1;
    uint32_t id = session->id;
    session_unref_locked(session);
    pthread_mutex_unlock(&G.lock);
    if (last) {
        GByteArray *a = g_byte_array_new();
        put_u32(a, id);
        send_msg(D_RELEASE, a);
        g_byte_array_unref(a);
    }
    return ERROR_NONE;
}

/* update/load: WebKit waits for keys_updated (or a message); a CDM that
 * answers without key changes still has to get there */
static void settle_quiet(struct OpenCDMSession *s) {
    pthread_mutex_lock(&G.lock);
    int quiet = !s->activity && !s->silent;
    OpenCDMSessionCallbacks cb = s->cb;
    void *user = s->user;
    pthread_mutex_unlock(&G.lock);
    if (quiet && cb.keys_updated_callback) cb.keys_updated_callback(s, user);
}

OpenCDMError opencdm_session_load(struct OpenCDMSession *session) {
    if (!session) return ERROR_INVALID_SESSION;
    pthread_mutex_lock(&G.lock);
    session->activity = 0;
    pthread_mutex_unlock(&G.lock);
    GByteArray *a = req_new();
    put_u32(a, session->sys);
    put_str(a, session->ks);
    put_u32(a, session->id);
    put_str(a, session->sid);
    char t[256];
    uint32_t st = request(D_LOAD, a, 25000, t, sizeof(t));
    g_byte_array_unref(a);
    if (st) {
        lg("%s: Laden fehlgeschlagen: %s", session->sid, t);
        return ERROR_INVALID_SESSION;
    }
    settle_quiet(session);
    return ERROR_NONE;
}

OpenCDMError opencdm_session_update(struct OpenCDMSession *session, const uint8_t keyMessage[],
                                    const uint16_t keyLength) {
    if (!session) return ERROR_INVALID_SESSION;
    pthread_mutex_lock(&G.lock);
    session->activity = 0;
    pthread_mutex_unlock(&G.lock);
    GByteArray *a = req_new();
    put_u32(a, session->id);
    put_bytes(a, keyMessage, keyLength);
    char t[256];
    uint32_t st = request(D_UPDATE, a, 25000, t, sizeof(t));
    g_byte_array_unref(a);
    if (st) {
        lg("%s: Lizenz abgelehnt: %s", session->sid, t);
        return ERROR_FAIL;
    }
    settle_quiet(session);
    return ERROR_NONE;
}

OpenCDMError opencdm_session_remove(struct OpenCDMSession *session) {
    if (!session) return ERROR_INVALID_SESSION;
    GByteArray *a = req_new();
    put_u32(a, session->id);
    uint32_t st = request(D_REMOVE, a, 15000, NULL, 0);
    g_byte_array_unref(a);
    return st ? ERROR_FAIL : ERROR_NONE;
}

OpenCDMError opencdm_session_close(struct OpenCDMSession *session) {
    if (!session) return ERROR_INVALID_SESSION;
    GByteArray *a = req_new();
    put_u32(a, session->id);
    uint32_t st = request(D_SCLOSE, a, 10000, NULL, 0);
    g_byte_array_unref(a);
    pthread_mutex_lock(&G.lock);
    session->silent = 1;
    pthread_mutex_unlock(&G.lock);
    return st ? ERROR_FAIL : ERROR_NONE;
}

const char *opencdm_session_id(const struct OpenCDMSession *session) { return session ? session->sid : ""; }

uint32_t opencdm_session_has_key_id(struct OpenCDMSession *session, const uint8_t length, const uint8_t keyId[]) {
    if (!session) return 0;
    pthread_mutex_lock(&G.lock);
    int k = key_index(session, keyId, length);
    pthread_mutex_unlock(&G.lock);
    return k >= 0;
}

KeyStatus opencdm_session_status(const struct OpenCDMSession *session, const uint8_t keyId[], const uint8_t length) {
    if (!session) return InternalError;
    pthread_mutex_lock(&G.lock);
    int k = key_index(session, keyId, length);
    KeyStatus st = k >= 0 ? session->keys[k].st : StatusPending;
    pthread_mutex_unlock(&G.lock);
    return st;
}

struct OpenCDMSession *opencdm_get_system_session(struct OpenCDMSystem *system, const uint8_t keyId[],
                                                  const uint8_t length, const uint32_t waitTime) {
    if (!system) return NULL;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += waitTime / 1000;
    until.tv_nsec += (long) (waitTime % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&G.lock);
    for (;;) {
        struct OpenCDMSession *best = NULL;
        for (struct OpenCDMSession *s = G.sessions; s; s = s->next) {
            if (s->silent || s->sys != system->id) continue;
            int k = key_index(s, keyId, length);
            if (k < 0) continue;
            if (!best || s->keys[k].st == Usable) best = s;
            if (s->keys[k].st == Usable) break;
        }
        if (best) {
            best->refs++;
            pthread_mutex_unlock(&G.lock);
            return best;
        }
        if (pthread_cond_timedwait(&G.cond, &G.lock, &until) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&G.lock);
    return NULL;
}

/* ---------------- GStreamer ---------------- */
#ifndef TVF_OCDM_NO_GST

static OpenCDMError tag_buffer(struct OpenCDMSession *session, GstBuffer *buffer, GstStructure *info) {
    gst_structure_set_name(info, "application/x-tvf-cenc");
    gst_structure_set(info, "tvf-session", G_TYPE_STRING, session->sid, "tvf-key-system", G_TYPE_STRING,
                      session->ks, NULL);
    if (!gst_buffer_add_protection_meta(buffer, info)) return ERROR_INVALID_DECRYPT_BUFFER;
    return ERROR_NONE;
}

OpenCDMError opencdm_gstreamer_session_decrypt_buffer(struct OpenCDMSession *session, GstBuffer *buffer,
                                                      GstCaps *caps) {
    (void) caps;
    if (!session || !buffer) return ERROR_INVALID_ARG;
    GstProtectionMeta *pm = gst_buffer_get_protection_meta(buffer);
    if (!pm || !pm->info) return ERROR_INVALID_DECRYPT_BUFFER;
    return tag_buffer(session, buffer, gst_structure_copy(pm->info));
}

OpenCDMError opencdm_gstreamer_session_decrypt(struct OpenCDMSession *session, GstBuffer *buffer,
                                               GstBuffer *subSample, const uint32_t subSampleCount, GstBuffer *IV,
                                               GstBuffer *keyID, uint32_t initWithLast15) {
    (void) initWithLast15;
    if (!session || !buffer || !IV || !keyID) return ERROR_INVALID_ARG;
    GstStructure *info = gst_structure_new("application/x-cenc", "encrypted", G_TYPE_BOOLEAN, TRUE, "iv_size",
                                           G_TYPE_UINT, (guint) gst_buffer_get_size(IV), "iv", GST_TYPE_BUFFER, IV,
                                           "kid", GST_TYPE_BUFFER, keyID, "subsample_count", G_TYPE_UINT,
                                           subSampleCount, "cipher-mode", G_TYPE_STRING, "cenc", NULL);
    if (subSampleCount && subSample) gst_structure_set(info, "subsamples", GST_TYPE_BUFFER, subSample, NULL);
    return tag_buffer(session, buffer, info);
}
#endif /* TVF_OCDM_NO_GST */

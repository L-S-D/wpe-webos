/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host test of TVF's OpenCDM client (helper/tvf-ocdm.c) against TVF's media
 * server with a simulated CDM and decoder (TVF tools/webmedia-test: fake_cdm.c,
 * fake_smp.c). Calls the OpenCDM API the way WebKit's Thunder CDM does, then
 * acts as a video sink and sends encrypted access units (crypto block, see
 * PROTOCOL-MEDIA.md "EME") for the session it created.
 *
 *   TVF:   sh tools/webmedia-test/build.sh && build/webmedia-test/webmedia-test 60 > /tmp/wm.log &
 *   here:  sh sdk/tests/eme/run.sh
 */
#include "ocdm/open_cdm.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (c) printf("ok   ");                                                                                         \
        else {                                                                                                         \
            printf("FAIL ");                                                                                           \
            fails++;                                                                                                   \
        }                                                                                                              \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
    } while (0)

static pthread_mutex_t L = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t C = PTHREAD_COND_INITIALIZER;
static char challenge[1024];
static int challenges, key_updates, keys_done, errors;
static uint8_t last_kid[16];

static void on_challenge(struct OpenCDMSession *s, void *u, const char url[], const uint8_t ch[], const uint16_t n) {
    (void) s, (void) u, (void) url;
    pthread_mutex_lock(&L);
    snprintf(challenge, sizeof(challenge), "%.*s", (int) n, (const char *) ch);
    challenges++;
    pthread_cond_broadcast(&C);
    pthread_mutex_unlock(&L);
}
static void on_key(struct OpenCDMSession *s, void *u, const uint8_t kid[], const uint8_t n) {
    (void) s, (void) u;
    pthread_mutex_lock(&L);
    if (n == 16) memcpy(last_kid, kid, 16);
    key_updates++;
    pthread_mutex_unlock(&L);
}
static void on_error(struct OpenCDMSession *s, void *u, const char m[]) {
    (void) s, (void) u;
    printf("     Fehler-Rueckruf: %s\n", m);
    pthread_mutex_lock(&L);
    errors++;
    pthread_mutex_unlock(&L);
}
static void on_keys_done(const struct OpenCDMSession *s, void *u) {
    (void) s, (void) u;
    pthread_mutex_lock(&L);
    keys_done++;
    pthread_cond_broadcast(&C);
    pthread_mutex_unlock(&L);
}

static int wait_for(int *v, int want, int ms) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000;
    pthread_mutex_lock(&L);
    while (*v < want)
        if (pthread_cond_timedwait(&C, &L, &t)) break;
    int r = *v >= want;
    pthread_mutex_unlock(&L);
    return r;
}

/* ---- raw media connection (as tvfvideosink) ---- */

static int connect_tvf(void) {
    const char *name = getenv("TVF_MEDIA_SOCK");
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    size_t n = strlen(name + 1);
    memcpy(a.sun_path + 1, name + 1, n);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(fd, (struct sockaddr *) &a, (socklen_t) (offsetof(struct sockaddr_un, sun_path) + 1 + n))) return -1;
    return fd;
}
static void send_raw(int fd, uint32_t type, const void *a, size_t an, const void *b, size_t bn) {
    uint32_t h[2] = {type, (uint32_t) (an + bn)};
    if (write(fd, h, 8) != 8 || (an && write(fd, a, an) != (ssize_t) an) || (bn && write(fd, b, bn) != (ssize_t) bn))
        perror("write");
}
static uint64_t acked;
static int eos_done;
static void *media_reader(void *arg) {
    int fd = (int) (intptr_t) arg;
    for (;;) {
        uint32_t h[2];
        if (read(fd, h, 8) != 8) break;
        uint8_t b[256];
        if (h[1] > sizeof(b) || (h[1] && read(fd, b, h[1]) != (ssize_t) h[1])) break;
        pthread_mutex_lock(&L);
        if (h[0] == 102 && h[1] >= 16) memcpy(&acked, b + 8, 8);
        if (h[0] == 104) eos_done = 1;
        if (h[0] == 105) printf("     TVF-Fehler: %.*s\n", (int) h[1], b);
        pthread_cond_broadcast(&C);
        pthread_mutex_unlock(&L);
    }
    return NULL;
}

/* one access unit: start code + IDR/non-IDR NAL, with a crypto block when sid is set */
static void send_unit(int fd, int64_t pts, int key, const char *sid, const uint8_t *kid) {
    uint8_t data[1000];
    memset(data, 0xab, sizeof(data));
    data[0] = data[1] = data[2] = 0, data[3] = 1, data[4] = key ? 0x65 : 0x41;
    struct {
        int64_t pts, dts, dur;
        uint32_t flags, epoch;
    } h = {pts, pts, 40000000, (key ? 1u : 0u) | (sid ? 4u : 0u), 0};
    uint8_t blk[256];
    size_t bn = 0;
    if (sid) {
        uint8_t sidl = (uint8_t) strlen(sid);
        uint32_t clen = 16 + 8 + 16 + sidl + 8;
        memcpy(blk, &clen, 4);
        blk[4] = 1, blk[5] = 8, blk[6] = 16, blk[7] = sidl; /* cenc, 8-byte IV */
        uint32_t crypt = 0, skip = 0, nsubs = 1, sub[2] = {5, sizeof(data) - 5};
        memcpy(blk + 8, &crypt, 4);
        memcpy(blk + 12, &skip, 4);
        memcpy(blk + 16, &nsubs, 4);
        memset(blk + 20, 7, 8);
        memcpy(blk + 28, kid, 16);
        memcpy(blk + 44, sid, sidl);
        memcpy(blk + 44 + sidl, sub, 8);
        bn = 4 + clen;
    }
    uint8_t head[32 + 256];
    memcpy(head, &h, 32);
    memcpy(head + 32, blk, bn);
    send_raw(fd, 3, head, 32 + bn, data, sizeof(data));
}

int main(void) {
    if (!getenv("TVF_MEDIA_SOCK")) setenv("TVF_MEDIA_SOCK", "@tvf-media-test", 1);
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(opencdm_is_type_supported("com.widevine.alpha", "") == ERROR_NONE, "Widevine unterstuetzt");
    CHECK(opencdm_is_type_supported("com.widevine.alpha", "video/mp4; codecs=\"avc1.640028\"") == ERROR_NONE,
          "Widevine + avc1");
    CHECK(opencdm_is_type_supported("com.widevine.alpha", "video/webm; codecs=\"vp9\"") != ERROR_NONE, "vp9 nicht");
    CHECK(opencdm_is_type_supported("org.w3.clearkey", "") != ERROR_NONE, "ClearKey nicht");

    struct OpenCDMSystem *sys = opencdm_create_system("com.widevine.alpha");
    CHECK(sys != NULL, "System");
    CHECK(opencdm_system_supports_server_certificate(sys) == OPENCDM_BOOL_TRUE, "Zertifikat moeglich");
    uint8_t cert[300];
    memset(cert, 5, sizeof(cert));
    CHECK(opencdm_system_set_server_certificate(sys, cert, sizeof(cert)) == ERROR_NONE, "Zertifikat gesetzt");

    OpenCDMSessionCallbacks cb = {on_challenge, on_key, on_error, on_keys_done};
    static const uint8_t pssh[] = {0, 0, 0, 0x20, 'p', 's', 's', 'h', 1, 2, 3, 4};
    struct OpenCDMSession *s = NULL;
    OpenCDMError e = opencdm_construct_session(sys, Temporary, "cenc", pssh, sizeof(pssh), NULL, 0, &cb, (void *) 1, &s);
    /* WebKit's callbacks live in its session object: the client must have copied them */
    memset(&cb, 0, sizeof(cb));
    CHECK(e == ERROR_NONE && s, "Sitzung angelegt");
    const char *sid = s ? opencdm_session_id(s) : "";
    CHECK(sid[0] != 0, "Sitzungs-ID %s", sid);
    CHECK(wait_for(&challenges, 1, 3000) && !strncmp(challenge, "0:Type:CHALLENGE:000000207073", 29),
          "Challenge mit Typ (%.40s)", challenge);

    int kd0 = keys_done;
    const char *lic = "LICENSE:00112233445566778899aabbccddeeff";
    CHECK(opencdm_session_update(s, (const uint8_t *) lic, (uint16_t) strlen(lic)) == ERROR_NONE, "Lizenz eingespielt");
    CHECK(wait_for(&keys_done, kd0 + 1, 2000) && key_updates == 1, "Schluessel-Rueckrufe (%d/%d)", key_updates, keys_done);
    static const uint8_t kid[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    CHECK(!memcmp(last_kid, kid, 16), "Schluessel-ID stimmt");
    CHECK(opencdm_session_status(s, kid, 16) == Usable, "Status nutzbar");
    CHECK(opencdm_session_has_key_id(s, 16, kid) == 1, "has_key_id");
    uint8_t swapped[16];
    memcpy(swapped, kid, 16);
    swapped[0] = kid[3], swapped[1] = kid[2], swapped[2] = kid[1], swapped[3] = kid[0];
    swapped[4] = kid[5], swapped[5] = kid[4], swapped[6] = kid[7], swapped[7] = kid[6];
    CHECK(opencdm_session_status(s, swapped, 16) == Usable, "PlayReady-Byteordnung gefunden");

    struct OpenCDMSession *g = opencdm_get_system_session(sys, kid, 16, 500);
    CHECK(g == s, "get_system_session");
    if (g) opencdm_destruct_session(g); /* zweite Referenz */
    CHECK(!strcmp(opencdm_session_id(s), sid), "Sitzung lebt nach destruct der zweiten Referenz");

    kd0 = keys_done;
    CHECK(opencdm_session_update(s, (const uint8_t *) "NOKEYS", 6) == ERROR_NONE, "Antwort ohne Schluessel");
    CHECK(wait_for(&keys_done, kd0 + 1, 2000), "trotzdem keys_updated (WebKit wartet darauf)");
    CHECK(opencdm_session_update(s, (const uint8_t *) "MUELL", 5) != ERROR_NONE, "kaputte Lizenz abgelehnt");

    /* ---- media: encrypted units for this session ---- */
    int fd = connect_tvf();
    CHECK(fd >= 0, "Medienverbindung");
    pthread_t rt;
    pthread_create(&rt, NULL, media_reader, (void *) (intptr_t) fd);
    const char *hello = "{\"v\":1,\"session\":\"ocdm-test\",\"stream\":\"video\"}";
    send_raw(fd, 1, hello, strlen(hello), NULL, 0);
    const char *caps = "{\"codec\":\"H264\",\"width\":640,\"height\":360,\"fps\":25,"
                       "\"protection\":\"edef8ba9-79d6-4ace-a3c8-27dcd51d21ed\"}";
    send_raw(fd, 2, caps, strlen(caps), NULL, 0);
    const char *play = "{\"playing\":true}";
    send_raw(fd, 6, play, strlen(play), NULL, 0);
    /* 2 klare (Vorlauf), dann 48 verschluesselte Einheiten, alle 25 ein Keyframe */
    for (int i = 0; i < 50; i++) send_unit(fd, (int64_t) i * 40000000, i % 25 == 0, i < 2 ? NULL : sid, kid);
    uint32_t epoch = 0;
    send_raw(fd, 5, &epoch, 4, NULL, 0);
    int one = 1;
    CHECK(wait_for(&eos_done, one, 8000), "Ende vom (nachgebauten) TV, %llu Bytes genommen",
          (unsigned long long) acked);
    CHECK(acked == 50 * 1000, "alle Einheiten genommen");
    close(fd);

    /* ---- persistent session ---- */
    struct OpenCDMSession *p = NULL;
    OpenCDMSessionCallbacks cb2 = {on_challenge, on_key, on_error, on_keys_done};
    const char *pid = "gespeichert-1";
    kd0 = keys_done;
    e = opencdm_construct_session(sys, PersistentLicense, "", NULL, 0, (const uint8_t *) pid, (uint16_t) strlen(pid), &cb2,
                                  (void *) 2, &p);
    CHECK(e == ERROR_NONE && p && !strcmp(opencdm_session_id(p), pid), "Sitzung zum Laden");
    CHECK(opencdm_session_load(p) == ERROR_NONE && wait_for(&keys_done, kd0 + 1, 2000), "geladen, Schluessel da");

    CHECK(opencdm_session_close(s) == ERROR_NONE, "Sitzung geschlossen");
    opencdm_destruct_session(s);
    opencdm_session_close(p);
    opencdm_destruct_session(p);
    CHECK(opencdm_destruct_system(sys) == ERROR_NONE, "System zu");
    CHECK(errors == 0, "keine Fehler-Rueckrufe");
    usleep(300000);
    printf("%s (%d Fehler)\n", fails ? "FEHLGESCHLAGEN" : "BESTANDEN", fails);
    return fails != 0;
}

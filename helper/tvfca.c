/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * libtvfca.so - preloaded into the WebKit child processes (ld.so --preload).
 *
 * Debian's GnuTLS finds its "system trust" through p11-kit modules and paths
 * below /usr that do not exist on the TV. glib-networking asks for it with
 * gnutls_x509_trust_list_add_system_trust(); this replacement loads the CA
 * bundle shipped with tvf-web instead (TVF_WEB_CA_FILE, PEM). Certificates are
 * still fully verified - only the location of the trust anchors changes.
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct gnutls_x509_trust_list_st *gnutls_x509_trust_list_t;

int gnutls_x509_trust_list_add_trust_file(gnutls_x509_trust_list_t list, const char *ca_file, const char *crl_file,
                                          int type, unsigned int tl_flags, unsigned int tl_vflags);

int gnutls_x509_trust_list_add_system_trust(gnutls_x509_trust_list_t list, unsigned int tl_flags,
                                            unsigned int tl_vflags) {
    const char *f = getenv("TVF_WEB_CA_FILE");
    if (!f || !*f) f = "/etc/ssl/certs/ca-certificates.crt";
    int n = gnutls_x509_trust_list_add_trust_file(list, f, NULL, 1 /* GNUTLS_X509_FMT_PEM */, tl_flags, tl_vflags);
    if (getenv("TVF_WEB_DEBUG")) fprintf(stderr, "[tvfca] %d CA-Zertifikate aus %s\n", n, f);
    return n;
}

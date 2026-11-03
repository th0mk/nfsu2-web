/* n2n_tls_mbedtls.c -- n2n_io over TLS with mbedTLS (devkitPro's switch-mbedtls), for wss://.  Build n2n.c with
 * -DN2N_TLS and link this file.
 *
 * NOT TESTED: written against mbedTLS's API without a Switch or mbedTLS at hand.  It verifies the server's
 * certificate against the PEM in n2n_tls_set_ca() (the root of your site's chain, e.g. ISRG Root X1 for Let's
 * Encrypt), called once before n2n_connect.  Without a CA it refuses to connect: no unverified TLS. */
#include "n2n.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

static const char *g_ca;
void n2n_tls_set_ca(const char *pem) { g_ca = pem; }

#ifndef N2N_BUILD_EPOCH
#define N2N_BUILD_EPOCH 1735689600LL
#endif
typedef struct {
    mbedtls_net_context net; mbedtls_ssl_context ssl; mbedtls_ssl_config conf;
    mbedtls_entropy_context ent; mbedtls_ctr_drbg_context drbg; mbedtls_x509_crt ca;
} tls;

static int tls_send(void *ctx, const void *buf, size_t n) {
    tls *t = ctx; const unsigned char *p = buf;
    while (n) {
        int w = mbedtls_ssl_write(&t->ssl, p, n);
        if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int tls_recv(void *ctx, void *buf, size_t n, int timeout_ms) {
    tls *t = ctx;
    if (!mbedtls_ssl_get_bytes_avail(&t->ssl)) {        /* bytes already decrypted are not visible to poll */
        int r = mbedtls_net_poll(&t->net, MBEDTLS_NET_POLL_READ, (uint32_t)timeout_ms);
        if (r < 0) return -1;
        if (r == 0) return 0;
    }
    int g = mbedtls_ssl_read(&t->ssl, buf, n);
    if (g == MBEDTLS_ERR_SSL_WANT_READ || g == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    return g > 0 ? g : -1;
}

static void tls_close(void *ctx) {
    tls *t = ctx;
    mbedtls_ssl_close_notify(&t->ssl);
    mbedtls_ssl_free(&t->ssl); mbedtls_ssl_config_free(&t->conf); mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->ent); mbedtls_x509_crt_free(&t->ca); mbedtls_net_free(&t->net);
    free(t);
}

int n2n_tls_connect(const char *host, int port, int timeout_ms, n2n_io *out) {
    (void)timeout_ms;
    if (!g_ca) return N2N_ERR_TLS;
    tls *t = calloc(1, sizeof *t);
    if (!t) return N2N_ERR_TLS;
    mbedtls_net_init(&t->net); mbedtls_ssl_init(&t->ssl); mbedtls_ssl_config_init(&t->conf);
    mbedtls_entropy_init(&t->ent); mbedtls_ctr_drbg_init(&t->drbg); mbedtls_x509_crt_init(&t->ca);
    char ps[16]; snprintf(ps, sizeof ps, "%d", port);
    if (mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->ent, (const unsigned char *)"n2n", 3) ||
        mbedtls_x509_crt_parse(&t->ca, (const unsigned char *)g_ca, strlen(g_ca) + 1) ||
        mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) goto fail;
    /* the chain and the name are checked after the handshake (below): a console whose clock is wrong would fail on the dates alone */
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, NULL);
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
    if (mbedtls_ssl_setup(&t->ssl, &t->conf) || mbedtls_ssl_set_hostname(&t->ssl, host)) goto fail;   /* SNI and name check */
    { int r = mbedtls_net_connect(&t->net, host, ps, MBEDTLS_NET_PROTO_TCP);
      if (r){ fprintf(stderr, "[tls] connect to %s:%s failed: mbedtls -0x%04x (errno %d)\n", host, ps, -r, errno); goto fail; } }
    mbedtls_ssl_set_bio(&t->ssl, &t->net, mbedtls_net_send, mbedtls_net_recv, NULL);
    for (int r; (r = mbedtls_ssl_handshake(&t->ssl)); )
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE){
            fprintf(stderr, "[tls] handshake failed: mbedtls -0x%04x\n", -r); goto fail; }
    { uint32_t fl = mbedtls_ssl_get_verify_result(&t->ssl);
      const uint32_t dates = MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE;
      if (fl & ~dates){ fprintf(stderr, "[tls] certificate refused: flags 0x%x\n", fl); goto fail; }
      if (fl & dates){
          /* only the dates are wrong: believed when the console's clock is right (it is then the certificate), not when it is
           * before the day this program was built (N2N_BUILD_EPOCH: a clock nobody set or that stands months back; the chain and the
           * name still held) */
          if (time(NULL) >= N2N_BUILD_EPOCH){ fprintf(stderr, "[tls] certificate out of date (flags 0x%x)\n", fl); goto fail; }
          fprintf(stderr, "[tls] the console's clock is before this build (time %lld): the certificate's dates are not checked\n", (long long)time(NULL));
      } }
    out->ctx = t; out->send = tls_send; out->recv = tls_recv; out->close = tls_close;
    return 0;
fail:
    tls_close(t);
    return N2N_ERR_TLS;
}

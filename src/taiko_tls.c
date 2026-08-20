/* Host TLS transport + online redirect configuration. See taiko_tls.h. */
#include "taiko_tls.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <errno.h>
#include <time.h>
#endif

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

/* ---------------------------------------------------------------------------
 * Configuration
 *
 * The arcade endpoints are compiled into the game (naominet.jp, the MUCHA and
 * game-server names ALL.Net hands back), so the redirect has to be host-side.
 * A file next to the executable, overridable per run by the environment:
 *
 *     host=127.0.0.1     TAIKO_ONLINE_HOST
 *     port=443           TAIKO_ONLINE_PORT
 *     verify=0           TAIKO_ONLINE_VERIFY   (1 = check the server chain)
 *     cacert=ca.pem      TAIKO_ONLINE_CACERT   (needed when verify=1)
 *
 * With no host configured the title stays offline exactly as before.
 * -----------------------------------------------------------------------*/
#define TAIKO_ONLINE_CONFIG_FILE "taiko_online.cfg"

static struct {
    int  loaded;
    char host[256];
    int  port;
    int  verify;
    char cacert[512];
} g_cfg;

static pthread_mutex_t g_tls_lock = PTHREAD_MUTEX_INITIALIZER;

static void cfg_assign(const char* key, const char* value)
{
    if (!strcmp(key, "host"))        snprintf(g_cfg.host, sizeof(g_cfg.host), "%s", value);
    else if (!strcmp(key, "port"))   g_cfg.port = atoi(value);
    else if (!strcmp(key, "verify")) g_cfg.verify = atoi(value);
    else if (!strcmp(key, "cacert")) snprintf(g_cfg.cacert, sizeof(g_cfg.cacert), "%s", value);
}

static void cfg_load_file(const char* path)
{
    FILE* f = fopen(path, "r");
    char line[640];
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char* hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = line;
        char* value = eq + 1;
        while (*key == ' ' || *key == '\t') key++;
        for (char* end = key + strlen(key); end > key && (end[-1] == ' ' || end[-1] == '\t'); )
            *--end = '\0';
        while (*value == ' ' || *value == '\t') value++;
        for (char* end = value + strlen(value);
             end > value && (end[-1] == '\n' || end[-1] == '\r' ||
                             end[-1] == ' '  || end[-1] == '\t'); )
            *--end = '\0';
        cfg_assign(key, value);
    }
    fclose(f);
}

static void cfg_load(void)
{
    if (g_cfg.loaded) return;
    g_cfg.loaded = 1;
    g_cfg.port = 443;

    const char* path = getenv("TAIKO_ONLINE_CONFIG");
    cfg_load_file(path && path[0] ? path : TAIKO_ONLINE_CONFIG_FILE);

    const char* host = getenv("TAIKO_ONLINE_HOST");
    const char* port = getenv("TAIKO_ONLINE_PORT");
    const char* verify = getenv("TAIKO_ONLINE_VERIFY");
    const char* cacert = getenv("TAIKO_ONLINE_CACERT");
    if (host && host[0])     cfg_assign("host", host);
    if (port && port[0])     cfg_assign("port", port);
    if (verify && verify[0]) cfg_assign("verify", verify);
    if (cacert && cacert[0]) cfg_assign("cacert", cacert);

    if (g_cfg.host[0])
        fprintf(stderr, "[taiko_online] every arcade service -> https://%s:%d "
                        "(certificate verification %s)\n",
                g_cfg.host, g_cfg.port, g_cfg.verify ? "on" : "off");
}

int taiko_online_enabled(void)
{
    pthread_mutex_lock(&g_tls_lock);
    cfg_load();
    int enabled = g_cfg.host[0] != '\0';
    pthread_mutex_unlock(&g_tls_lock);
    return enabled;
}

const char* taiko_online_host(void)
{
    pthread_mutex_lock(&g_tls_lock);
    cfg_load();
    const char* host = g_cfg.host;
    pthread_mutex_unlock(&g_tls_lock);
    return host;
}

int taiko_online_port(void)
{
    pthread_mutex_lock(&g_tls_lock);
    cfg_load();
    int port = g_cfg.port;
    pthread_mutex_unlock(&g_tls_lock);
    return port;
}

/* ---------------------------------------------------------------------------
 * TLS
 * -----------------------------------------------------------------------*/

struct taiko_tls {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;
    mbedtls_x509_crt    cacert;
    int                 socket_fd;   /* -1 when the caller supplied callbacks */
};

/* One entropy source and DRBG for the process; mbedTLS documents both as
 * shareable across contexts, and seeding is the expensive part. */
static mbedtls_entropy_context  g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static int                      g_drbg_ready;

static int tls_seed_locked(void)
{
    if (g_drbg_ready) return 0;
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    const char* personal = "taiko_recomp";
    int rc = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                                   (const unsigned char*)personal, strlen(personal));
    if (rc != 0) {
        fprintf(stderr, "[taiko_online] ctr_drbg_seed failed: -0x%04X\n", -rc);
        return -1;
    }
    g_drbg_ready = 1;
    return 0;
}

static void tls_log_error(const char* what, int rc)
{
    char text[128];
    mbedtls_strerror(rc, text, sizeof(text));
    fprintf(stderr, "[taiko_online] %s failed: -0x%04X (%s)\n", what, -rc, text);
}

static void tls_yield(void)
{
#ifdef _WIN32
    Sleep(1);
#else
    struct timespec ms = { 0, 1000000 };
    nanosleep(&ms, NULL);
#endif
}

static int tls_socket_send(void* ctx, const unsigned char* buf, size_t len)
{
    int fd = (int)(intptr_t)ctx;
    int n = (int)send(fd, (const char*)buf, (int)len, 0);
    return n < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : n;
}

static int tls_socket_recv(void* ctx, unsigned char* buf, size_t len)
{
    int fd = (int)(intptr_t)ctx;
    int n = (int)recv(fd, (char*)buf, (int)len, 0);
    return n < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : n;
}

taiko_tls* taiko_tls_open(taiko_tls_send_fn send_fn, taiko_tls_recv_fn recv_fn,
                          void* ctx, const char* sni)
{
    if (!send_fn || !recv_fn) return NULL;

    pthread_mutex_lock(&g_tls_lock);
    cfg_load();
    int seeded = tls_seed_locked();
    int verify = g_cfg.verify;
    char cacert_path[512];
    snprintf(cacert_path, sizeof(cacert_path), "%s", g_cfg.cacert);
    pthread_mutex_unlock(&g_tls_lock);
    if (seeded != 0) return NULL;

    taiko_tls* tls = (taiko_tls*)calloc(1, sizeof(*tls));
    if (!tls) return NULL;
    tls->socket_fd = -1;
    mbedtls_ssl_init(&tls->ssl);
    mbedtls_ssl_config_init(&tls->conf);
    mbedtls_x509_crt_init(&tls->cacert);

    int rc = mbedtls_ssl_config_defaults(&tls->conf, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) { tls_log_error("ssl_config_defaults", rc); goto fail; }

    /* A private arcade server presents its own certificate, so verification is
     * off unless a CA is configured -- refusing it would just mean no online
     * at all, which is the state this replaces. */
    if (verify && cacert_path[0]) {
        rc = mbedtls_x509_crt_parse_file(&tls->cacert, cacert_path);
        if (rc != 0) { tls_log_error("x509_crt_parse_file", rc); goto fail; }
        mbedtls_ssl_conf_ca_chain(&tls->conf, &tls->cacert, NULL);
        mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        if (verify)
            fprintf(stderr, "[taiko_online] verify=1 but no cacert configured; "
                            "continuing without verification\n");
        mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    mbedtls_ssl_conf_rng(&tls->conf, mbedtls_ctr_drbg_random, &g_drbg);
    rc = mbedtls_ssl_setup(&tls->ssl, &tls->conf);
    if (rc != 0) { tls_log_error("ssl_setup", rc); goto fail; }

    if (sni && sni[0]) {
        rc = mbedtls_ssl_set_hostname(&tls->ssl, sni);
        if (rc != 0) { tls_log_error("ssl_set_hostname", rc); goto fail; }
    }

    mbedtls_ssl_set_bio(&tls->ssl, ctx, send_fn, recv_fn, NULL);

    while ((rc = mbedtls_ssl_handshake(&tls->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            tls_log_error("handshake", rc);
            goto fail;
        }
        tls_yield();   /* the transport may be non-blocking; do not spin hot */
    }
    return tls;

fail:
    mbedtls_ssl_free(&tls->ssl);
    mbedtls_ssl_config_free(&tls->conf);
    mbedtls_x509_crt_free(&tls->cacert);
    free(tls);
    return NULL;
}

taiko_tls* taiko_tls_open_socket(int fd, const char* sni)
{
    taiko_tls* tls = taiko_tls_open(tls_socket_send, tls_socket_recv,
                                    (void*)(intptr_t)fd, sni);
    if (tls) tls->socket_fd = fd;
    return tls;
}

int taiko_tls_send(taiko_tls* tls, const void* buf, size_t len)
{
    const unsigned char* p = (const unsigned char*)buf;
    size_t sent = 0;
    if (!tls) return -1;
    while (sent < len) {
        int rc = mbedtls_ssl_write(&tls->ssl, p + sent, len - sent);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            { tls_yield(); continue; }
        if (rc <= 0) { tls_log_error("ssl_write", rc); return -1; }
        sent += (size_t)rc;
    }
    return 0;
}

int taiko_tls_recv(taiko_tls* tls, void* buf, size_t len)
{
    if (!tls) return -1;
    for (;;) {
        int rc = mbedtls_ssl_read(&tls->ssl, (unsigned char*)buf, len);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            { tls_yield(); continue; }
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        if (rc < 0) { tls_log_error("ssl_read", rc); return -1; }
        return rc;
    }
}

void taiko_tls_close(taiko_tls* tls)
{
    if (!tls) return;
    mbedtls_ssl_close_notify(&tls->ssl);
    mbedtls_ssl_free(&tls->ssl);
    mbedtls_ssl_config_free(&tls->conf);
    mbedtls_x509_crt_free(&tls->cacert);
    free(tls);
}

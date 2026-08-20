/* Six-digit pairing: log in without a card reader.
 *
 * The cabinet asks the server for a pairing session, the server answers with a
 * six-digit code, and whoever types that code into the server's web UI picks a
 * card for it. The server then hands the cabinet that card's 20-digit access
 * code, which goes onto the virtual reader as if it had been swiped.
 *
 *   POST /api/zucchini/pairing        Authorization: Bearer <token>
 *   cabinet_id=<8 hex>&state=<scene>&accepting=1[&session=..][&ack=..]
 *   -> status=active   session=..  code=977086  expires_in=30
 *   -> status=claimed  command_id=..  access_code=<20 digits>
 *
 * `state` must be one of attract/shop/unknown; the recomp has no scene
 * classifier, and the server documents `unknown` for exactly that case.
 *
 * The protocol and its semantics come from TaikoZucchini's network/pairing.c
 * (MIT, same author); only the transport and the reader plumbing differ.
 */
#include "taiko_card.h"
#include "taiko_tls.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PAIRING_PATH      "/api/zucchini/pairing"
#define PAIRING_POLL_MS   2000
#define PAIRING_HTTP_CAP  8192

static pthread_t g_thread;
static int       g_running;
static char      g_session[96];
static char      g_ack[48];
static char      g_last_command[48];
static char      g_shown_code[8];

typedef struct {
    char status[16];
    char session[96];
    char code[8];
    char command_id[48];
    char access_code[24];
    int  expires_in;
} pairing_response;

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* One field out of the server's `key=value` lines. */
static int copy_field(const char* body, const char* key, char* out, size_t cap)
{
    const size_t key_length = strlen(key);
    for (const char* line = body; line && *line; ) {
        const char* end = strchr(line, '\n');
        const size_t length = end ? (size_t)(end - line) : strlen(line);
        if (length > key_length && !strncmp(line, key, key_length) &&
            line[key_length] == '=') {
            size_t value_length = length - key_length - 1;
            if (value_length && line[key_length + 1 + value_length - 1] == '\r')
                value_length--;
            if (value_length >= cap) value_length = cap - 1;
            memcpy(out, line + key_length + 1, value_length);
            out[value_length] = '\0';
            return 1;
        }
        line = end ? end + 1 : NULL;
    }
    out[0] = '\0';
    return 0;
}

static int exact_digits(const char* value, int digits)
{
    for (int i = 0; i < digits; i++)
        if (value[i] < '0' || value[i] > '9') return 0;
    return value[digits] == '\0';
}

/* One request/response over its own TLS connection. Returns 0 on success. */
static int pairing_request(int accepting, pairing_response* out)
{
    const char* host = taiko_online_host();
    const char* token = taiko_online_pairing_token();

    /* The token goes into a header verbatim; a newline in it would let a bad
     * config inject one. */
    if (strpbrk(token, "\r\n")) return -1;
    char body[512];
    char request[1024];
    char response[PAIRING_HTTP_CAP];

    const int body_length = snprintf(body, sizeof(body),
        "cabinet_id=%s&state=unknown&accepting=%d%s%s%s%s",
        taiko_online_cabinet_id(), accepting,
        g_session[0] ? "&session=" : "", g_session[0] ? g_session : "",
        g_ack[0] ? "&ack=" : "", g_ack[0] ? g_ack : "");
    if (body_length <= 0 || body_length >= (int)sizeof(body)) return -1;

    const int request_length = snprintf(request, sizeof(request),
        "POST %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\n"
        "Accept: text/plain\r\nContent-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
        PAIRING_PATH, host, token, body_length, body);
    if (request_length <= 0 || request_length >= (int)sizeof(request)) return -1;

    taiko_tls* tls = taiko_tls_connect(host, taiko_online_port());
    if (!tls) return -1;

    int rc = -1;
    if (taiko_tls_send(tls, request, (size_t)request_length) == 0) {
        size_t filled = 0;
        for (;;) {
            const int n = taiko_tls_recv(tls, response + filled,
                                         sizeof(response) - 1 - filled);
            if (n <= 0) break;
            filled += (size_t)n;
            if (filled >= sizeof(response) - 1) break;
        }
        response[filled] = '\0';
        const char* head = strstr(response, "\r\n\r\n");
        if (head && strstr(response, " 200 ")) {
            const char* payload = head + 4;
            memset(out, 0, sizeof(*out));
            if (copy_field(payload, "status", out->status, sizeof(out->status))) {
                copy_field(payload, "session", out->session, sizeof(out->session));
                copy_field(payload, "code", out->code, sizeof(out->code));
                copy_field(payload, "command_id", out->command_id, sizeof(out->command_id));
                copy_field(payload, "access_code", out->access_code, sizeof(out->access_code));
                char expires[16];
                copy_field(payload, "expires_in", expires, sizeof(expires));
                out->expires_in = atoi(expires);
                rc = 0;
            }
        }
    }
    taiko_tls_close(tls);
    return rc;
}

static void apply(const pairing_response* response)
{
    if (response->session[0])
        snprintf(g_session, sizeof(g_session), "%s", response->session);

    if (!strcmp(response->status, "closed")) {
        g_session[0] = g_ack[0] = g_last_command[0] = g_shown_code[0] = '\0';
        return;
    }

    /* A card was chosen for us: put it on the reader, then acknowledge it so
     * the server does not hand it out again. */
    if (response->command_id[0] && exact_digits(response->access_code, 20)) {
        if (strcmp(response->command_id, g_last_command) != 0) {
            const int rc = taiko_card_present(response->access_code);
            snprintf(g_last_command, sizeof(g_last_command), "%s", response->command_id);
            snprintf(g_ack, sizeof(g_ack), "%s", response->command_id);
            if (rc != TAIKO_CARD_OK)
                fprintf(stderr, "[taiko_pairing] card could not be presented (%d)\n", rc);
        }
        g_shown_code[0] = '\0';
        return;
    }

    if (!strcmp(response->status, "active") && exact_digits(response->code, 6) &&
        response->expires_in > 0) {
        if (strcmp(response->code, g_shown_code) != 0) {
            snprintf(g_shown_code, sizeof(g_shown_code), "%s", response->code);
            fprintf(stderr,
                    "[taiko_pairing] pairing code %s -- enter it on %s within %ds "
                    "to put a card on the reader\n",
                    response->code, taiko_online_host(), response->expires_in);
        }
        return;
    }

    g_shown_code[0] = '\0';
}

static void* pairing_thread(void* unused)
{
    (void)unused;
    while (g_running) {
        /* Only while the game is polling the reader and has no card yet. */
        if (taiko_card_reader_active() && !taiko_card_is_present()) {
            pairing_response response;
            if (pairing_request(1, &response) == 0)
                apply(&response);
        } else if (g_session[0]) {
            pairing_response response;
            (void)pairing_request(0, &response);   /* close the session */
            g_session[0] = g_ack[0] = g_last_command[0] = g_shown_code[0] = '\0';
        }
        for (int slept = 0; slept < PAIRING_POLL_MS && g_running; slept += 100)
            sleep_ms(100);
    }
    return NULL;
}

__attribute__((constructor))
static void taiko_pairing_start(void)
{
    const char* token = taiko_online_pairing_token();
    if (!taiko_online_enabled() || !token || !token[0])
        return;

    g_running = 1;
    if (pthread_create(&g_thread, NULL, pairing_thread, NULL) != 0) {
        g_running = 0;
        fprintf(stderr, "[taiko_pairing] could not start the polling thread\n");
        return;
    }
    fprintf(stderr, "[taiko_pairing] polling %s as cabinet %s\n",
            taiko_online_host(), taiko_online_cabinet_id());
}

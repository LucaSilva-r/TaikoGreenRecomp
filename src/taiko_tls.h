/* Host TLS + the online redirect, shared by the two transports the title uses
 * to reach its arcade services: cellHttp's own native sockets (MUCHA, the game
 * server) and the raw sys_net socket the ALL.Net PowerOn POST speaks.
 *
 * The title's own cellSsl is a lifecycle shell and its stack is pinned to TLS
 * 1.0, but that only mattered on real PS3 hardware. Here the library is host C,
 * so the handshake is ours and modern TLS is free.
 */
#ifndef TAIKO_TLS_H
#define TAIKO_TLS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Redirect configuration, from taiko_online.cfg and/or the environment. */
int         taiko_online_enabled(void);   /* 0 when no server is configured */
const char* taiko_online_host(void);
int         taiko_online_port(void);

/* Transport callbacks. Signatures match mbedTLS's BIO so a socket transport
 * can be handed over unwrapped: >0 bytes, 0 for EOF, <0 for error. */
typedef int (*taiko_tls_send_fn)(void* ctx, const unsigned char* buf, size_t len);
typedef int (*taiko_tls_recv_fn)(void* ctx, unsigned char* buf, size_t len);

typedef struct taiko_tls taiko_tls;

/* Handshake as a client over an already-connected transport. `sni` is the
 * name presented in SNI (and verified against, when verification is on);
 * NULL skips SNI. Returns NULL if the handshake fails. */
taiko_tls* taiko_tls_open(taiko_tls_send_fn send_fn, taiko_tls_recv_fn recv_fn,
                          void* ctx, const char* sni);

/* Same, over a connected native socket. */
taiko_tls* taiko_tls_open_socket(int fd, const char* sni);

/* Resolve, connect and handshake in one step; closes the socket on failure.
 * The socket is closed by taiko_tls_close. */
taiko_tls* taiko_tls_connect(const char* host, int port);

/* Pairing configuration, alongside the redirect. */
const char* taiko_online_pairing_token(void);
const char* taiko_online_cabinet_id(void);

/* Sends every byte or fails. Returns 0 on success, -1 on error. */
int  taiko_tls_send(taiko_tls* tls, const void* buf, size_t len);
/* Bytes read, 0 at end of stream, -1 on error. */
int  taiko_tls_recv(taiko_tls* tls, void* buf, size_t len);
void taiko_tls_close(taiko_tls* tls);

#ifdef __cplusplus
}
#endif

#endif /* TAIKO_TLS_H */

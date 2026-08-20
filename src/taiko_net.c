/* cellHttp NID aliases for Taiko's MUCHA client.
 *
 * ps3recomp implements the HTTP transport for real (native sockets, HTTP/1.1)
 * but exports it under SDK names that differ from the ones this title imports
 * -- e.g. it defines cellHttpGetStatusCode while Taiko imports
 * cellHttpResponseGetStatusCode. NIDs are hashes of the name, so nothing binds
 * and the client retries forever ("RouterName :NETDB_SUCCESS" x3842).
 *
 * These are aliases, not reimplementations: same code, second NID. The handful
 * with no counterpart return CELL_OK, which is the honest answer for a tuning
 * knob (keep-alive, pool sizes) on a host stack that manages its own sockets.
 *
 * ponytail: no TLS here. cellSsl/cellHttps are still lifecycle-only, so this
 * gets plain HTTP working -- enough for an offline cabinet and for a private
 * MUCHA server over http://. Real HTTPS wants a host TLS backend behind
 * cellHttps (mingw64-openssl, or Schannel on the Windows target); do it when
 * something actually needs to reach an https:// endpoint.
 */
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef int32_t s32;
typedef uint32_t u32;
typedef uint64_t u64;

extern void ps3_hle_register(uint32_t nid, const char* name, void* handler);

/* Implemented in ps3recomp/libs/network/cellHttp.c */
s32 cellHttpGetStatusCode(u32 transId, s32* code);
s32 cellHttpGetResponseContentLength(u32 transId, u64* length);
s32 cellHttpSetRequestContentLength(u32 transId, u64 length);
s32 cellHttpSetConnectTimeOut(u32 transId, u32 usec);
s32 cellHttpAbortTransaction(u32 transId);
s32 cellHttpCloseConnection(u32 transId);

/* The socket layer, implemented in ps3recomp/libs/network/sysNet.c under
 * RPCS3's sys_net_bnet_* names. Taiko imports the plain BSD names, which hash
 * to different NIDs, so none of it was reachable -- gethostbyname in
 * particular, which is what the MUCHA router lookup spins on. */
#include "sysNet.h"
#include "taiko_tls.h"
#include <mbedtls/ssl.h>          /* the BIO callbacks below answer in mbedTLS codes */
#include <mbedtls/net_sockets.h>

extern unsigned char* vm_base;

#define NET_HOSTENT_EA   0x0C000100u
#define NET_HOSTNAME_EA  0x0C000140u
#define NET_ALIASES_EA   0x0C0001C0u
#define NET_ADDR_LIST_EA 0x0C0001C8u
#define NET_ADDR_EA      0x0C0001D0u

static void net_write_be32(u32 ea, u32 value)
{
    vm_base[ea + 0] = (unsigned char)(value >> 24);
    vm_base[ea + 1] = (unsigned char)(value >> 16);
    vm_base[ea + 2] = (unsigned char)(value >> 8);
    vm_base[ea + 3] = (unsigned char)value;
}

static u32 net_read_be32(u32 ea)
{
    return ((u32)vm_base[ea + 0] << 24) |
           ((u32)vm_base[ea + 1] << 16) |
           ((u32)vm_base[ea + 2] << 8) |
           (u32)vm_base[ea + 3];
}

static uint16_t net_read_be16(u32 ea)
{
    return (uint16_t)(((u32)vm_base[ea] << 8) | vm_base[ea + 1]);
}

static void net_write_be16(u32 ea, uint16_t value)
{
    vm_base[ea + 0] = (unsigned char)(value >> 8);
    vm_base[ea + 1] = (unsigned char)value;
}

/* Bounded socket tracing; TAIKO_NET_TRACE=1. */
static int net_trace_enabled(void)
{
    static int state = -1;
    static unsigned budget = 40;
    if (state < 0) state = getenv("TAIKO_NET_TRACE") ? 1 : 0;
    if (!state || !budget) return 0;
    budget--;
    return 1;
}

static void* net_guest_ptr(u32 ea)
{
    return ea && vm_base ? vm_base + ea : NULL;
}

/* --------------------------------------------------------------------------
 * Online redirect for the raw socket path.
 *
 * The ALL.Net PowerOn POST does not go through cellHttp: it opens a socket to
 * naominet.jp:80 and writes HTTP itself. Zucchini catches that with a loopback
 * proxy because it runs inside the game; we own connect(), so the connection
 * is retargeted in place -- and, since the configured server is HTTPS, wrapped
 * in TLS here. The guest keeps writing and reading plain HTTP, unaware.
 *
 * SNI needs the name the guest asked for, which connect() does not carry, so
 * gethostbyname hands out a distinct 127.1.x.y for each name and this table
 * maps it back.
 * -----------------------------------------------------------------------*/
#define NET_CONNECT_WAIT_MS    5000
#define NET_MAX_REDIRECT_NAMES 32
#define NET_MAX_TLS_SOCKETS    64

static struct { u32 addr; char name[128]; } g_redirect_names[NET_MAX_REDIRECT_NAMES];
static u32 g_redirect_name_count;
/* One entry per redirected guest socket. `pending` means the TCP connect is
 * still in progress (the guest uses non-blocking sockets), so the handshake is
 * deferred to the first send or recv. */
static struct {
    int        used;
    s32        fd;
    taiko_tls* tls;
    int        pending;
    char       sni[128];
} g_tls_sockets[NET_MAX_TLS_SOCKETS];

/* Several service threads resolve and connect at once, so both tables are
 * locked. ponytail: one lock for both, held across the handshake -- that
 * serializes concurrent connects for a few milliseconds each. Split it if a
 * slow server ever makes that visible. */
static pthread_mutex_t g_online_lock = PTHREAD_MUTEX_INITIALIZER;

/* Synthetic loopback address for `name`, stable for the life of the process. */
static u32 net_redirect_addr_for(const char* name)
{
    u32 i;
    pthread_mutex_lock(&g_online_lock);
    for (i = 0; i < g_redirect_name_count; i++)
        if (!strcmp(g_redirect_names[i].name, name)) {
            u32 addr = g_redirect_names[i].addr;
            pthread_mutex_unlock(&g_online_lock);
            return addr;
        }
    if (g_redirect_name_count >= NET_MAX_REDIRECT_NAMES) {
        pthread_mutex_unlock(&g_online_lock);
        return 0x7F000001u;                       /* 127.0.0.1, no SNI */
    }
    i = g_redirect_name_count++;
    snprintf(g_redirect_names[i].name, sizeof(g_redirect_names[i].name), "%s", name);
    g_redirect_names[i].addr = 0x7F010000u | (i + 1);   /* 127.1.0.N */
    pthread_mutex_unlock(&g_online_lock);
    return g_redirect_names[i].addr;
}

/* The entry is never removed, so the returned pointer stays valid. */
static const char* net_redirect_name_for(u32 addr)
{
    const char* name = NULL;
    pthread_mutex_lock(&g_online_lock);
    for (u32 i = 0; i < g_redirect_name_count; i++)
        if (g_redirect_names[i].addr == addr) {
            name = g_redirect_names[i].name;
            break;
        }
    pthread_mutex_unlock(&g_online_lock);
    return name;
}

/* All four take g_online_lock; the _locked helpers assume it is held. */
static int net_tls_slot_locked(s32 fd)
{
    for (u32 i = 0; i < NET_MAX_TLS_SOCKETS; i++)
        if (g_tls_sockets[i].used && g_tls_sockets[i].fd == fd)
            return (int)i;
    return -1;
}

static void net_tls_drop_locked(s32 fd)
{
    int slot = net_tls_slot_locked(fd);
    if (slot < 0) return;
    if (g_tls_sockets[slot].tls) taiko_tls_close(g_tls_sockets[slot].tls);
    memset(&g_tls_sockets[slot], 0, sizeof(g_tls_sockets[slot]));
}

static void net_tls_drop(s32 fd)
{
    pthread_mutex_lock(&g_online_lock);
    net_tls_drop_locked(fd);
    pthread_mutex_unlock(&g_online_lock);
}

static void net_tls_claim(s32 fd, const char* sni, int pending)
{
    pthread_mutex_lock(&g_online_lock);
    net_tls_drop_locked(fd);
    for (u32 i = 0; i < NET_MAX_TLS_SOCKETS; i++) {
        if (!g_tls_sockets[i].used) {
            g_tls_sockets[i].used = 1;
            g_tls_sockets[i].fd = fd;
            g_tls_sockets[i].tls = NULL;
            g_tls_sockets[i].pending = pending;
            snprintf(g_tls_sockets[i].sni, sizeof(g_tls_sockets[i].sni), "%s",
                     sni ? sni : "");
            pthread_mutex_unlock(&g_online_lock);
            return;
        }
    }
    pthread_mutex_unlock(&g_online_lock);
    fprintf(stderr, "[taiko_online] TLS socket table full; socket %d stays plain\n", fd);
}

/* mbedTLS BIO over one guest socket. The title sets SO_NBIO, so a short read
 * or write is the normal case and has to come back as WANT_READ/WANT_WRITE --
 * any other negative value aborts the handshake outright. */
static int net_tls_would_block(void)
{
    const int32_t* e = sys_net_errno_loc();
    return e && (*e == (int32_t)SYS_NET_EWOULDBLOCK ||
                 *e == (int32_t)SYS_NET_EINPROGRESS ||
                 *e == (int32_t)SYS_NET_EALREADY);
}

static int net_tls_bio_send(void* ctx, const unsigned char* buf, size_t len)
{
    s32 n = sys_net_bnet_send((s32)(intptr_t)ctx, buf, (u32)len, 0);
    if (n >= 0) return (int)n;
    return net_tls_would_block() ? MBEDTLS_ERR_SSL_WANT_WRITE
                                 : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int net_tls_bio_recv(void* ctx, unsigned char* buf, size_t len)
{
    s32 n = sys_net_bnet_recv((s32)(intptr_t)ctx, buf, (u32)len, 0);
    if (n >= 0) return (int)n;
    return net_tls_would_block() ? MBEDTLS_ERR_SSL_WANT_READ
                                 : MBEDTLS_ERR_NET_RECV_FAILED;
}

/* The session for a redirected socket, completing a deferred handshake the
 * first time the guest actually uses the connection. */
static taiko_tls* net_tls_session(s32 fd)
{
    pthread_mutex_lock(&g_online_lock);
    int slot = net_tls_slot_locked(fd);
    if (slot < 0 || (!g_tls_sockets[slot].tls && !g_tls_sockets[slot].pending)) {
        pthread_mutex_unlock(&g_online_lock);
        return NULL;
    }
    if (g_tls_sockets[slot].tls) {
        taiko_tls* tls = g_tls_sockets[slot].tls;
        pthread_mutex_unlock(&g_online_lock);
        return tls;
    }

    const char* sni = g_tls_sockets[slot].sni[0] ? g_tls_sockets[slot].sni : NULL;
    taiko_tls* tls = taiko_tls_open(net_tls_bio_send, net_tls_bio_recv,
                                    (void*)(intptr_t)fd, sni);
    g_tls_sockets[slot].pending = 0;
    g_tls_sockets[slot].tls = tls;
    pthread_mutex_unlock(&g_online_lock);
    fprintf(stderr, "[taiko_online] socket %d TLS session %s (sni=%s)\n",
            fd, tls ? "established" : "FAILED", sni ? sni : "none");
    return tls;
}

/* The configured server, resolved on the host once. */
static u32 net_resolve_online_host(void)
{
    static u32 cached;
    if (cached) return cached;

    struct addrinfo hints;
    struct addrinfo* result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(taiko_online_host(), NULL, &hints, &result) != 0 || !result) {
        fprintf(stderr, "[taiko_online] cannot resolve configured host '%s'\n",
                taiko_online_host());
        if (result) freeaddrinfo(result);
        return 0;
    }
    const struct sockaddr_in* in = (const struct sockaddr_in*)result->ai_addr;
    cached = ntohl(in->sin_addr.s_addr);
    freeaddrinfo(result);
    return cached;
}


/* The generic HLE bridge passes pointer arguments as guest effective
 * addresses. Adapt Taiko's plain BSD imports to the host-pointer sysNet API;
 * registering sys_net_bnet_* directly worked by accident only on mappings
 * where a low guest address happened to be readable. */
static s32 net_bind(s32 s, u32 addr_ea, u32 addrlen)
{
    return sys_net_bnet_bind(s, (const sys_net_sockaddr*)net_guest_ptr(addr_ea), addrlen);
}

static s32 net_accept(s32 s, u32 addr_ea, u32 addrlen_ea)
{
    unsigned char address[16] = {0};
    u32 length = addrlen_ea ? net_read_be32(addrlen_ea) : sizeof(address);
    if (length > sizeof(address)) length = sizeof(address);
    s32 result = sys_net_bnet_accept(s, addr_ea ? (sys_net_sockaddr*)address : NULL,
                                     addrlen_ea ? &length : NULL);
    if (result >= 0 && addr_ea) memcpy(vm_base + addr_ea, address, length);
    if (addrlen_ea) net_write_be32(addrlen_ea, length);
    return result;
}

static s32 net_connect(s32 s, u32 addr_ea, u32 addrlen)
{
    if (!taiko_online_enabled() || !addr_ea || !vm_base)
        return sys_net_bnet_connect(s, (const sys_net_sockaddr*)net_guest_ptr(addr_ea), addrlen);

    /* sys_net_sockaddr_in: u8 len, u8 family, be16 port, be32 addr. */
    const u32 wanted_addr = net_read_be32(addr_ea + 4);
    const uint16_t wanted_port = net_read_be16(addr_ea + 2);
    const char* sni = net_redirect_name_for(wanted_addr);

    unsigned char target[16] = {0};
    u32 host_addr = net_resolve_online_host();
    if (!host_addr) return -1;
    target[0] = 16;
    target[1] = 2;                                  /* AF_INET */
    target[2] = (unsigned char)(taiko_online_port() >> 8);
    target[3] = (unsigned char)taiko_online_port();
    target[4] = (unsigned char)(host_addr >> 24);
    target[5] = (unsigned char)(host_addr >> 16);
    target[6] = (unsigned char)(host_addr >> 8);
    target[7] = (unsigned char)host_addr;

    s32 result = sys_net_bnet_connect(s, (const sys_net_sockaddr*)target, 16);
    const int32_t* net_errno = sys_net_errno_loc();
    fprintf(stderr, "[taiko_online] socket %d connect %u.%u.%u.%u:%u (%s) -> %s:%d "
                    "over TLS: rc=%d errno=%d\n",
            s, (wanted_addr >> 24) & 0xFF, (wanted_addr >> 16) & 0xFF,
            (wanted_addr >> 8) & 0xFF, wanted_addr & 0xFF, wanted_port,
            sni ? sni : "unnamed", taiko_online_host(), taiko_online_port(),
            result, net_errno ? *net_errno : 0);

    /* The title sets SO_NBIO, so the connect reports EINPROGRESS and the guest
     * decides when to look again. Finish it here instead: wait for writability
     * and complete the handshake, then report a connected socket. Otherwise the
     * TLS session would have to be built inside whatever the guest happens to
     * call next, and a service thread that waits on a long retry timer would
     * hold the whole online path there. Bounded, on a background thread. */
    if (result != 0 && net_errno && *net_errno == (int32_t)SYS_NET_EINPROGRESS) {
        sys_net_pollfd wait = { s, SYS_NET_POLLOUT, 0 };
        if (sys_net_bnet_poll(&wait, 1, NET_CONNECT_WAIT_MS) > 0 &&
            (wait.revents & SYS_NET_POLLOUT))
            result = 0;
    }
    if (result != 0) {
        fprintf(stderr, "[taiko_online] socket %d never connected to %s:%d\n",
                s, taiko_online_host(), taiko_online_port());
        return result;
    }

    /* SNI is the configured server's own name: a real server terminates TLS
     * for itself and answers an unknown name with a fatal alert (measured:
     * sni=naominet.jp -> -0x7780). The service the guest wanted is still
     * visible to it in the `Host:` header the guest writes. */
    net_tls_claim(s, taiko_online_host(), 1);
    if (!net_tls_session(s)) {
        net_tls_drop(s);
        sys_net_bnet_shutdown(s, 2);
        return -1;
    }
    return 0;
}


/* The raw ALL.Net path writes its own HTTP, so its `Host:` header names
 * naominet.jp. A server reached through a proxy or CDN rejects that outright
 * (measured: Cloudflare 403; the same request with the server's own name
 * returns stat=1), so rewrite that one header on the way out. Path, method,
 * body and every other header are left alone -- those are what the server
 * routes on. Returns the rewritten length, or 0 to send the original bytes. */
static u32 net_rewrite_host_header(const char* in, u32 len, char* out, u32 out_size)
{
    if (len < 16 || len > out_size) return 0;
    if (memcmp(in, "GET ", 4) && memcmp(in, "POST ", 5) &&
        memcmp(in, "HEAD ", 5) && memcmp(in, "PUT ", 4))
        return 0;

    /* Search the header block only. */
    u32 header_len = len;
    for (u32 i = 0; i + 3 < len; i++)
        if (!memcmp(in + i, "\r\n\r\n", 4)) { header_len = i + 2; break; }

    for (u32 i = 0; i + 6 < header_len; i++) {
        if (memcmp(in + i, "\r\n", 2) ||
            (in[i + 2] != 'H' && in[i + 2] != 'h') ||
            strncasecmp(in + i + 2, "Host:", 5))
            continue;

        u32 value_end = i + 2;
        while (value_end + 1 < header_len && memcmp(in + value_end, "\r\n", 2))
            value_end++;

        const char* host = taiko_online_host();
        u32 head = i + 2;
        u32 tail = len - value_end;
        u32 total = head + 6 + (u32)strlen(host) + tail;
        if (total > out_size) return 0;

        memcpy(out, in, head);
        int written = snprintf(out + head, out_size - head, "Host: %s", host);
        if (written < 0) return 0;
        memcpy(out + head + written, in + value_end, tail);
        return head + (u32)written + tail;
    }
    return 0;
}

static s32 net_send(s32 s, u32 buf_ea, u32 len, s32 flags)
{
    taiko_tls* tls = net_tls_session(s);
    if (net_trace_enabled())
        fprintf(stderr, "[taiko_net] send fd=%d len=%u %s\n", s, len,
                tls ? "over TLS" : "plain");
    if (!tls)
        return sys_net_bnet_send(s, net_guest_ptr(buf_ea), len, flags);

    const char* payload = (const char*)net_guest_ptr(buf_ea);
    char rewritten[8192];
    u32 rewritten_len = net_rewrite_host_header(payload, len, rewritten,
                                                sizeof(rewritten));
    if (rewritten_len) {
        if (net_trace_enabled())
            fprintf(stderr, "[taiko_net] send fd=%d Host: -> %s\n", s,
                    taiko_online_host());
        payload = rewritten;
    }
    /* The guest counts the bytes it handed us, not the rewritten ones. */
    return taiko_tls_send(tls, payload, rewritten_len ? rewritten_len : len) == 0
               ? (s32)len : -1;
}

static s32 net_sendto(s32 s, u32 buf_ea, u32 len, s32 flags,
                      u32 to_ea, u32 tolen)
{
    return sys_net_bnet_sendto(s, net_guest_ptr(buf_ea), len, flags,
                               (const sys_net_sockaddr*)net_guest_ptr(to_ea), tolen);
}

static s32 net_recv(s32 s, u32 buf_ea, u32 len, s32 flags)
{
    taiko_tls* tls = net_tls_session(s);
    if (net_trace_enabled())
        fprintf(stderr, "[taiko_net] recv fd=%d len=%u %s\n", s, len,
                tls ? "over TLS" : "plain");
    if (!tls)
        return sys_net_bnet_recv(s, net_guest_ptr(buf_ea), len, flags);

    s32 got = taiko_tls_recv(tls, net_guest_ptr(buf_ea), len);
    if (got > 0 && net_trace_enabled()) {
        /* The server's answer is what decides whether the cabinet comes
         * online, so show the head of it rather than just a byte count. */
        const char* text = (const char*)net_guest_ptr(buf_ea);
        int show = got < 220 ? got : 220;
        fprintf(stderr, "[taiko_net] recv fd=%d %d bytes: ", s, got);
        for (int i = 0; i < show; i++)
            fputc((text[i] >= 32 && text[i] < 127) ? text[i] : '.', stderr);
        fputc('\n', stderr);
    }
    return got;
}

static s32 net_recvfrom(s32 s, u32 buf_ea, u32 len, s32 flags,
                        u32 from_ea, u32 fromlen_ea)
{
    unsigned char address[16] = {0};
    u32 fromlen = fromlen_ea ? net_read_be32(fromlen_ea) : sizeof(address);
    if (fromlen > sizeof(address)) fromlen = sizeof(address);
    s32 result = sys_net_bnet_recvfrom(
        s, net_guest_ptr(buf_ea), len, flags,
        from_ea ? (sys_net_sockaddr*)address : NULL,
        fromlen_ea ? &fromlen : NULL);
    if (result >= 0 && from_ea) memcpy(vm_base + from_ea, address, fromlen);
    if (fromlen_ea) net_write_be32(fromlen_ea, fromlen);
    return result;
}

static s32 net_setsockopt(s32 s, s32 level, s32 opt, u32 val_ea, u32 len)
{
    unsigned char value[32] = {0};
    if (len > sizeof(value) || (len && !val_ea)) return -1;
    if (len == 4) {
        u32 host_value = net_read_be32(val_ea);
        memcpy(value, &host_value, sizeof(host_value));
    } else if (len) {
        memcpy(value, vm_base + val_ea, len);
        /* sys_net_linger is two guest-endian s32 values. */
        if (len == 8 && opt == 0x0080) {
            u32 first = net_read_be32(val_ea);
            u32 second = net_read_be32(val_ea + 4);
            memcpy(value + 0, &first, 4);
            memcpy(value + 4, &second, 4);
        }
    }
    return sys_net_bnet_setsockopt(s, level, opt, len ? value : NULL, len);
}

static s32 net_getsockopt(s32 s, s32 level, s32 opt, u32 val_ea, u32 len_ea)
{
    unsigned char value[32] = {0};
    if (!len_ea) return -1;
    u32 length = net_read_be32(len_ea);
    if (length > sizeof(value) || (length && !val_ea)) return -1;
    s32 result = sys_net_bnet_getsockopt(s, level, opt,
                                         length ? value : NULL, &length);
    if (result == 0 && val_ea) {
        if (length == 4) {
            u32 host_value;
            memcpy(&host_value, value, sizeof(host_value));
            net_write_be32(val_ea, host_value);
        } else {
            memcpy(vm_base + val_ea, value, length);
        }
    }
    net_write_be32(len_ea, length);
    return result;
}

/* PS3's hostent contains guest pointers and big-endian integers, so a native
 * hostent (or the ps3recomp shim's old success sentinel, 1) cannot be returned
 * directly. Keep one title-local result in unused guest RAM. This mirrors the
 * loopback DNS hook used by Taiko Zucchini and is opt-in until the matching
 * ALL.Net transport is implemented. */
static u32 net_hostent(const char* name, u32 address)
{
    size_t name_len = strlen(name);
    if (name_len > 126)
        name_len = 126;

    memcpy(vm_base + NET_HOSTNAME_EA, name, name_len);
    vm_base[NET_HOSTNAME_EA + name_len] = 0;

    net_write_be32(NET_HOSTENT_EA + 0x00, NET_HOSTNAME_EA);  /* h_name */
    net_write_be32(NET_HOSTENT_EA + 0x04, NET_ALIASES_EA);   /* h_aliases */
    net_write_be32(NET_HOSTENT_EA + 0x08, 2);                /* AF_INET */
    net_write_be32(NET_HOSTENT_EA + 0x0C, 4);                /* h_length */
    net_write_be32(NET_HOSTENT_EA + 0x10, NET_ADDR_LIST_EA); /* h_addr_list */
    net_write_be32(NET_ALIASES_EA, 0);
    net_write_be32(NET_ADDR_LIST_EA + 0, NET_ADDR_EA);
    net_write_be32(NET_ADDR_LIST_EA + 4, 0);
    net_write_be32(NET_ADDR_EA, address);
    return NET_HOSTENT_EA;
}

static u32 net_loopback_hostent(const char* name)
{
    return net_hostent(name, 0x7F000001u);
}

/* The generic HLE bridge passes PS3 effective addresses verbatim. They are
 * offsets into vm_base, not native pointers. Passing name_ea directly made
 * Winsock read unrelated low memory: tenporouter.loc / bbrouter.loc appeared
 * as empty strings and resolved to the host PC. Translate before resolving. */
static unsigned net_gethostbyname(unsigned name_ea)
{
    const char* name = (name_ea && vm_base) ? (const char*)(vm_base + name_ea) : NULL;
    unsigned char first = name ? (unsigned char)name[0] : 0;
    const char* loopback = getenv("TAIKO_DNS_LOOPBACK");
    static unsigned trace_count;

    if (!name || first == 0) {
        if (trace_count++ < 16)
            fprintf(stderr, "[taiko_net] gethostbyname ea=0x%08X -> HOST_NOT_FOUND\n",
                    name_ea);
        return 0;                       /* NULL hostent -> h_errno HOST_NOT_FOUND */
    }

    /* Redirecting: each name gets its own synthetic address so the connect
     * hook can recover it for SNI. */
    if (taiko_online_enabled()) {
        u32 address = net_redirect_addr_for(name);
        if (trace_count++ < 16)
            fprintf(stderr, "[taiko_net] gethostbyname '%.96s' -> %u.%u.%u.%u "
                            "(redirect placeholder)\n",
                    name, (address >> 24) & 0xFF, (address >> 16) & 0xFF,
                    (address >> 8) & 0xFF, address & 0xFF);
        return net_hostent(name, address);
    }

    if (trace_count++ < 16)
        fprintf(stderr, "[taiko_net] gethostbyname '%.96s' -> %s\n", name,
                (loopback && loopback[0] != '0') ? "127.0.0.1 guest hostent"
                                                 : "host resolver");
    if (loopback && loopback[0] != '0')
        return net_loopback_hostent(name);
    return sys_net_bnet_gethostbyname(name);
}

/* inet_addr is inet_aton with the result in the return value (INADDR_NONE on
 * failure) rather than an out-param. */
static u32 net_inet_addr(u32 cp_ea)
{
    u32 out = 0xFFFFFFFFu;
    const char* cp = (cp_ea && vm_base) ? (const char*)(vm_base + cp_ea) : NULL;
    if (!cp)
        return out;
    return sys_net_bnet_inet_aton(cp, &out) ? out : 0xFFFFFFFFu;
}

/* _sys_net_errno_loc / _sys_net_h_errno_loc return the guest address of this
 * thread's errno. ponytail: one shared word at a fixed guest EA, not per
 * thread -- guest RAM between the image (.bss ends ~0x149C000) and the SPU/TLS
 * scratch arenas at 0x0D000000 is unused. It reads 0 (success) unless a
 * handler writes it, so a caller that inspects errno after a failure sees
 * "no error". Give it a real per-thread slot the first time that misleads. */
#define NET_ERRNO_EA 0x0C000000u
static u32 net_errno_loc(void)
{
    /* Publish the socket layer's errno instead of a word nothing ever writes:
     * a guest that sees connect() == -1 with errno 0 reads a hard failure
     * where the truth is EINPROGRESS. ponytail: sysNet keeps one global errno,
     * so this slot is process-wide too. */
    const int32_t* host = sys_net_errno_loc();
    net_write_be32(NET_ERRNO_EA, host ? (u32)*host : 0);
    return NET_ERRNO_EA;
}
static u32 net_h_errno_loc(void) { return NET_ERRNO_EA + 4; }

static s32 net_close(s32 s)
{
    net_tls_drop(s);
    return sys_net_bnet_close(s);
}

static s32 net_shutdown(s32 s, s32 how)
{
    if (how == 2) net_tls_drop(s);
    return sys_net_bnet_shutdown(s, how);
}

/* --------------------------------------------------------------------------
 * I/O multiplexing.
 *
 * socketpoll/socketselect were registered to a stub that returned 0 (timeout)
 * without waiting, so every guest loop above them span at full speed instead
 * of blocking for its timeout. sysNet.c already has a real poll(); these are
 * the guest<->host marshalling it was missing. Layouts match RPCS3's
 * Emu/Cell/lv2/sys_net.h: pollfd is {be s32 fd; be s16 events; be s16
 * revents} (8 bytes), fd_set is 32 big-endian words with bit (fd&31) of word
 * (fd>>5), timeval is two big-endian s64.
 * -----------------------------------------------------------------------*/
#define SYS_NET_POLLPRI   0x0002
#define NET_MAX_POLLFDS   64
#define NET_FD_SETSIZE    256
/* ponytail: a blocking wait is capped rather than infinite so a guest thread
 * parked in select cannot outlive the process. Callers re-arm on timeout. */
#define NET_WAIT_CAP_MS   30000

static s32 net_socketpoll(u32 fds_ea, u32 nfds, s32 timeout_ms)
{
    sys_net_pollfd fds[NET_MAX_POLLFDS];
    u32 i;

    if (!fds_ea || !vm_base || nfds == 0) return 0;
    if (nfds > NET_MAX_POLLFDS) nfds = NET_MAX_POLLFDS;
    if (timeout_ms < 0 || timeout_ms > NET_WAIT_CAP_MS) timeout_ms = NET_WAIT_CAP_MS;

    for (i = 0; i < nfds; i++) {
        fds[i].fd      = (s32)net_read_be32(fds_ea + i * 8);
        fds[i].events  = (int16_t)net_read_be16(fds_ea + i * 8 + 4);
        fds[i].revents = 0;
    }
    s32 result = sys_net_bnet_poll(fds, nfds, timeout_ms);
    for (i = 0; i < nfds; i++)
        net_write_be16(fds_ea + i * 8 + 6, (uint16_t)fds[i].revents);
    if (net_trace_enabled())
        fprintf(stderr, "[taiko_net] poll nfds=%u timeout=%d -> %d (fd %d ev %04X rev %04X)\n",
                nfds, timeout_ms, result, fds[0].fd,
                (unsigned)fds[0].events & 0xFFFFu, (unsigned)fds[0].revents & 0xFFFFu);
    return result;
}

static int net_fd_isset(u32 set_ea, s32 fd)
{
    if (!set_ea) return 0;
    return (net_read_be32(set_ea + ((fd >> 5) & 31) * 4) >> (fd & 31)) & 1;
}

static void net_fd_zero(u32 set_ea)
{
    int word;
    if (!set_ea) return;
    for (word = 0; word < 32; word++) net_write_be32(set_ea + word * 4, 0);
}

static void net_fd_set(u32 set_ea, s32 fd)
{
    u32 ea = set_ea + ((fd >> 5) & 31) * 4;
    net_write_be32(ea, net_read_be32(ea) | (1u << (fd & 31)));
}

static s32 net_select_timeout_ms(u32 tv_ea)
{
    if (!tv_ea) return NET_WAIT_CAP_MS;              /* NULL == block */
    uint64_t sec  = ((uint64_t)net_read_be32(tv_ea) << 32) | net_read_be32(tv_ea + 4);
    uint64_t usec = ((uint64_t)net_read_be32(tv_ea + 8) << 32) | net_read_be32(tv_ea + 12);
    uint64_t ms = sec * 1000ull + usec / 1000ull;
    return ms > NET_WAIT_CAP_MS ? NET_WAIT_CAP_MS : (s32)ms;
}

static s32 net_socketselect(s32 nfds, u32 read_ea, u32 write_ea, u32 except_ea,
                            u32 timeout_ea)
{
    sys_net_pollfd fds[NET_MAX_POLLFDS];
    s32 guest_fd[NET_MAX_POLLFDS];
    u32 count = 0;
    s32 fd;

    if (!vm_base) return -1;
    if (nfds > NET_FD_SETSIZE) nfds = NET_FD_SETSIZE;

    for (fd = 0; fd < nfds && count < NET_MAX_POLLFDS; fd++) {
        int16_t events = 0;
        if (net_fd_isset(read_ea, fd))   events |= SYS_NET_POLLIN;
        if (net_fd_isset(write_ea, fd))  events |= SYS_NET_POLLOUT;
        if (net_fd_isset(except_ea, fd)) events |= SYS_NET_POLLPRI;
        if (!events) continue;
        fds[count].fd      = fd;
        fds[count].events  = events;
        fds[count].revents = 0;
        guest_fd[count++]  = fd;
    }

    s32 timeout_ms = net_select_timeout_ms(timeout_ea);
    s32 result = count ? sys_net_bnet_poll(fds, count, timeout_ms) : 0;
    if (net_trace_enabled())
        fprintf(stderr, "[taiko_net] select nfds=%d watching=%u timeout=%d -> %d\n",
                nfds, count, timeout_ms, result);

    net_fd_zero(read_ea);
    net_fd_zero(write_ea);
    net_fd_zero(except_ea);
    if (result <= 0) return result;

    s32 ready = 0;
    for (u32 i = 0; i < count; i++) {
        int16_t revents = fds[i].revents;
        /* select reports an error/hangup as readable and writable, which is
         * what a caller that only watches one set needs to make progress. */
        int failed = (revents & (SYS_NET_POLLERR | SYS_NET_POLLHUP |
                                 SYS_NET_POLLNVAL)) != 0;
        if (read_ea && ((revents & SYS_NET_POLLIN) || failed)) {
            net_fd_set(read_ea, guest_fd[i]); ready++;
        }
        if (write_ea && ((revents & SYS_NET_POLLOUT) || failed)) {
            net_fd_set(write_ea, guest_fd[i]); ready++;
        }
        if (except_ea && (revents & (SYS_NET_POLLPRI | SYS_NET_POLLNVAL))) {
            net_fd_set(except_ea, guest_fd[i]); ready++;
        }
    }
    return ready;
}

/* inet_ntop(af, src, dst, size) -> dst, or 0 on failure. */
static u32 net_inet_ntop(s32 af, u32 src_ea, u32 dst_ea, u32 size)
{
    char text[64];
    if (af != 2 || !src_ea || !dst_ea || !vm_base) return 0;   /* AF_INET only */
    int n = snprintf(text, sizeof(text), "%u.%u.%u.%u",
                     vm_base[src_ea], vm_base[src_ea + 1],
                     vm_base[src_ea + 2], vm_base[src_ea + 3]);
    if (n < 0 || (u32)n >= size) return 0;
    memcpy(vm_base + dst_ea, text, (size_t)n + 1);
    return dst_ea;
}

/* cellHttpRequestSetHeader(transId, const CellHttpHeader* {name, value}).
 * Registered as a no-op before, so every header the client set -- including
 * the MUCHA Content-Type -- was dropped. */
s32 cellHttpAddRequestHeader(u32 transId, const char* name, const char* value);

static s32 net_http_request_set_header(u32 transId, u32 header_ea)
{
    if (!header_ea || !vm_base) return 0;
    u32 name_ea  = net_read_be32(header_ea);
    u32 value_ea = net_read_be32(header_ea + 4);
    if (!name_ea || !value_ea) return 0;
    /* cellHttp.c takes guest effective addresses, not host pointers. */
    return cellHttpAddRequestHeader(transId, (const char*)(uintptr_t)name_ea,
                                    (const char*)(uintptr_t)value_ea);
}

/* sys_net_abort_socket(s, flags): unblock everyone waiting on the socket. */
static s32 net_abort_socket(s32 s, s32 flags)
{
    (void)flags;
    return sys_net_bnet_shutdown(s, 2 /* SHUT_RDWR */);
}

/* --------------------------------------------------------------------------
 * cellHttp transport hooks (declared in ps3recomp/libs/network/cellHttp.c).
 * Installed only when a server is configured, so the default build keeps the
 * plain-HTTP path it had.
 * -----------------------------------------------------------------------*/
extern int   (*g_http_redirect_target)(char* host, u32 host_size, u32* port);
extern void* (*g_http_tls_open)(int fd, const char* sni);
extern int   (*g_http_tls_send)(void* session, const void* buf, u32 len);
extern int   (*g_http_tls_recv)(void* session, void* buf, u32 len);
extern void  (*g_http_tls_close)(void* session);

static int http_redirect_target(char* host, u32 host_size, u32* port)
{
    snprintf(host, host_size, "%s", taiko_online_host());
    *port = (u32)taiko_online_port();
    return 1;
}

static void* http_tls_open(int fd, const char* sni)
{
    return taiko_tls_open_socket(fd, sni);
}

static int http_tls_send(void* session, const void* buf, u32 len)
{
    return taiko_tls_send((taiko_tls*)session, buf, len);
}

static int http_tls_recv(void* session, void* buf, u32 len)
{
    return taiko_tls_recv((taiko_tls*)session, buf, len);
}

static void http_tls_close(void* session)
{
    taiko_tls_close((taiko_tls*)session);
}

/* cellHttpRequestGetAllHeaders(trans, headers**, items*, pool, poolSize,
 * required*). Returning CELL_OK without touching the out-parameters left the
 * caller iterating whatever was on its stack. No request header is set through
 * this build's path, so the honest answer is "none". */
static s32 net_http_request_get_all_headers(u32 transId, u32 headers_ea,
                                            u32 items_ea, u32 pool_ea,
                                            u32 poolSize, u32 required_ea)
{
    (void)transId; (void)pool_ea; (void)poolSize;
    if (!vm_base) return 0;
    if (headers_ea)  net_write_be32(headers_ea, 0);
    if (items_ea)    net_write_be32(items_ea, 0);
    if (required_ea) net_write_be32(required_ea, 0);
    return 0;
}

/* Knobs with no host-side equivalent. */
static s32 http_ok(void) { return 0; }

__attribute__((constructor))
static void taiko_net_register(void)
{
    if (taiko_online_enabled()) {
        g_http_redirect_target = http_redirect_target;
        g_http_tls_open        = http_tls_open;
        g_http_tls_send        = http_tls_send;
        g_http_tls_recv        = http_tls_recv;
        g_http_tls_close       = http_tls_close;
    }

    /* Same function, the name this title's SDK used. */
    ps3_hle_register(0x10D0D7FCu, "cellHttpResponseGetStatusCode",     (void*)cellHttpGetStatusCode);
    ps3_hle_register(0x464FF889u, "cellHttpResponseGetContentLength",  (void*)cellHttpGetResponseContentLength);
    ps3_hle_register(0xAF73A64Eu, "cellHttpRequestSetContentLength",   (void*)cellHttpSetRequestContentLength);
    ps3_hle_register(0xD7471088u, "cellHttpClientSetConnTimeout",      (void*)cellHttpSetConnectTimeOut);
    ps3_hle_register(0x2D52848Bu, "cellHttpTransactionAbortConnection",(void*)cellHttpAbortTransaction);
    ps3_hle_register(0xA0D9223Cu, "cellHttpTransactionCloseConnection",(void*)cellHttpCloseConnection);

    /* Connection tuning: the host stack opens and closes its own sockets. */
    ps3_hle_register(0x5D473170u, "cellHttpClientSetKeepAlive",        (void*)http_ok);
    ps3_hle_register(0x1395D8D1u, "cellHttpClientSetSslCallback",      (void*)http_ok);

    ps3_hle_register(0x54F2A4DEu, "cellHttpRequestSetHeader",         (void*)net_http_request_set_header);

    /* Named by NID (SHA-1(name + PS3 suffix)); no host-side equivalent, and
     * none of their results are read before the first request. */
    ps3_hle_register(0x2033B878u, "cellHttpClientCloseAllConnections", (void*)http_ok);
    ps3_hle_register(0x42205FE0u, "cellHttpRequestGetAllHeaders",      (void*)net_http_request_get_all_headers);

    /* BSD socket names -> the sys_net_bnet_* implementations. */
    ps3_hle_register(0x9C056962u, "socket",        (void*)sys_net_bnet_socket);
    ps3_hle_register(0xB0A59804u, "bind",          (void*)net_bind);
    ps3_hle_register(0x28E208BBu, "listen",        (void*)sys_net_bnet_listen);
    ps3_hle_register(0xC94F6939u, "accept",        (void*)net_accept);
    ps3_hle_register(0x64F66D35u, "connect",       (void*)net_connect);
    ps3_hle_register(0xDC751B40u, "send",          (void*)net_send);
    ps3_hle_register(0x9647570Bu, "sendto",        (void*)net_sendto);
    ps3_hle_register(0xFBA04F37u, "recv",          (void*)net_recv);
    ps3_hle_register(0x1F953B9Fu, "recvfrom",      (void*)net_recvfrom);
    ps3_hle_register(0xA50777C6u, "shutdown",      (void*)net_shutdown);
    ps3_hle_register(0x6DB6E8CDu, "socketclose",   (void*)net_close);
    ps3_hle_register(0x88F03575u, "setsockopt",    (void*)net_setsockopt);
    ps3_hle_register(0x5A045BD1u, "getsockopt",    (void*)net_getsockopt);
    ps3_hle_register(0x71F4C717u, "gethostbyname", (void*)net_gethostbyname);
    ps3_hle_register(0x051EE3EEu, "socketpoll",    (void*)net_socketpoll);
    ps3_hle_register(0x3F09E20Au, "socketselect",  (void*)net_socketselect);
    ps3_hle_register(0xC98A3146u, "inet_ntop",     (void*)net_inet_ntop);
    ps3_hle_register(0x8D1B77FBu, "sys_net_abort_socket",        (void*)net_abort_socket);
    ps3_hle_register(0xFDB8F926u, "sys_net_free_thread_context", (void*)http_ok);
    ps3_hle_register(0x3B27C780u, "sys_net_get_sockinfo",        (void*)http_ok);
    ps3_hle_register(0xA765D029u, "sys_net_get_sockinfo_ex",     (void*)http_ok);
    ps3_hle_register(0xB48636C4u, "sys_net_show_ifconfig",       (void*)http_ok);
    ps3_hle_register(0xDABBC2C0u, "inet_addr",     (void*)net_inet_addr);
    ps3_hle_register(0x6005CDE1u, "_sys_net_errno_loc",   (void*)net_errno_loc);
    ps3_hle_register(0xC9157D30u, "_sys_net_h_errno_loc", (void*)net_h_errno_loc);
}

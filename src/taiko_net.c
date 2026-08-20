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
#include <stdlib.h>
#include <string.h>

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

/* The socket layer, implemented in ps3recomp/libs/network/sysNet.c under
 * RPCS3's sys_net_bnet_* names. Taiko imports the plain BSD names, which hash
 * to different NIDs, so none of it was reachable -- gethostbyname in
 * particular, which is what the MUCHA router lookup spins on. */
s32 sys_net_bnet_socket(s32 domain, s32 type, s32 protocol);
s32 sys_net_bnet_bind(s32 s, const void* addr, u32 addrlen);
s32 sys_net_bnet_listen(s32 s, s32 backlog);
s32 sys_net_bnet_accept(s32 s, void* addr, u32* addrlen);
s32 sys_net_bnet_connect(s32 s, const void* addr, u32 addrlen);
s32 sys_net_bnet_send(s32 s, const void* buf, u32 len, s32 flags);
s32 sys_net_bnet_sendto(s32 s, const void* buf, u32 len, s32 flags, const void* to, u32 tolen);
s32 sys_net_bnet_recv(s32 s, void* buf, u32 len, s32 flags);
s32 sys_net_bnet_recvfrom(s32 s, void* buf, u32 len, s32 flags, void* from, u32* fromlen);
s32 sys_net_bnet_shutdown(s32 s, s32 how);
s32 sys_net_bnet_close(s32 s);
s32 sys_net_bnet_setsockopt(s32 s, s32 level, s32 opt, const void* val, u32 len);
s32 sys_net_bnet_getsockopt(s32 s, s32 level, s32 opt, void* val, u32* len);
s32 sys_net_bnet_inet_aton(const char* cp, u32* inp);
u32 sys_net_bnet_gethostbyname(const char* name);

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

static void* net_guest_ptr(u32 ea)
{
    return ea && vm_base ? vm_base + ea : NULL;
}

/* The generic HLE bridge passes pointer arguments as guest effective
 * addresses. Adapt Taiko's plain BSD imports to the host-pointer sysNet API;
 * registering sys_net_bnet_* directly worked by accident only on mappings
 * where a low guest address happened to be readable. */
static s32 net_bind(s32 s, u32 addr_ea, u32 addrlen)
{
    return sys_net_bnet_bind(s, net_guest_ptr(addr_ea), addrlen);
}

static s32 net_accept(s32 s, u32 addr_ea, u32 addrlen_ea)
{
    unsigned char address[16] = {0};
    u32 length = addrlen_ea ? net_read_be32(addrlen_ea) : sizeof(address);
    if (length > sizeof(address)) length = sizeof(address);
    s32 result = sys_net_bnet_accept(s, addr_ea ? address : NULL,
                                     addrlen_ea ? &length : NULL);
    if (result >= 0 && addr_ea) memcpy(vm_base + addr_ea, address, length);
    if (addrlen_ea) net_write_be32(addrlen_ea, length);
    return result;
}

static s32 net_connect(s32 s, u32 addr_ea, u32 addrlen)
{
    return sys_net_bnet_connect(s, net_guest_ptr(addr_ea), addrlen);
}

static s32 net_send(s32 s, u32 buf_ea, u32 len, s32 flags)
{
    return sys_net_bnet_send(s, net_guest_ptr(buf_ea), len, flags);
}

static s32 net_sendto(s32 s, u32 buf_ea, u32 len, s32 flags,
                      u32 to_ea, u32 tolen)
{
    return sys_net_bnet_sendto(s, net_guest_ptr(buf_ea), len, flags,
                               net_guest_ptr(to_ea), tolen);
}

static s32 net_recv(s32 s, u32 buf_ea, u32 len, s32 flags)
{
    return sys_net_bnet_recv(s, net_guest_ptr(buf_ea), len, flags);
}

static s32 net_recvfrom(s32 s, u32 buf_ea, u32 len, s32 flags,
                        u32 from_ea, u32 fromlen_ea)
{
    unsigned char address[16] = {0};
    u32 fromlen = fromlen_ea ? net_read_be32(fromlen_ea) : sizeof(address);
    if (fromlen > sizeof(address)) fromlen = sizeof(address);
    s32 result = sys_net_bnet_recvfrom(
        s, net_guest_ptr(buf_ea), len, flags,
        from_ea ? address : NULL, fromlen_ea ? &fromlen : NULL);
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
static u32 net_loopback_hostent(const char* name)
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
    vm_base[NET_ADDR_EA + 0] = 127;
    vm_base[NET_ADDR_EA + 1] = 0;
    vm_base[NET_ADDR_EA + 2] = 0;
    vm_base[NET_ADDR_EA + 3] = 1;
    return NET_HOSTENT_EA;
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
    if (trace_count++ < 16)
        fprintf(stderr, "[taiko_net] gethostbyname ea=0x%08X name='%.*s' -> %s\n",
                name_ea, 96, name ? name : "",
                (!name || first == 0) ? "HOST_NOT_FOUND" :
                (loopback && loopback[0] != '0' ? "127.0.0.1 guest hostent" : "host resolver"));
    if (!name || first == 0)
        return 0;                       /* NULL hostent -> h_errno HOST_NOT_FOUND */
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
static u32 net_errno_loc(void) { return NET_ERRNO_EA; }
static u32 net_h_errno_loc(void) { return NET_ERRNO_EA + 4; }

/* Knobs with no host-side equivalent. */
static s32 http_ok(void) { return 0; }

__attribute__((constructor))
static void taiko_net_register(void)
{
    /* Same function, the name this title's SDK used. */
    ps3_hle_register(0x10D0D7FCu, "cellHttpResponseGetStatusCode",     (void*)cellHttpGetStatusCode);
    ps3_hle_register(0x464FF889u, "cellHttpResponseGetContentLength",  (void*)cellHttpGetResponseContentLength);
    ps3_hle_register(0xAF73A64Eu, "cellHttpRequestSetContentLength",   (void*)cellHttpSetRequestContentLength);
    ps3_hle_register(0xD7471088u, "cellHttpClientSetConnTimeout",      (void*)cellHttpSetConnectTimeOut);
    ps3_hle_register(0x2D52848Bu, "cellHttpTransactionAbortConnection",(void*)cellHttpAbortTransaction);
    ps3_hle_register(0xA0D9223Cu, "cellHttpTransactionCloseConnection",(void*)cellHttpAbortTransaction);

    /* Connection tuning: the host stack opens and closes its own sockets. */
    ps3_hle_register(0x5D473170u, "cellHttpClientSetKeepAlive",        (void*)http_ok);
    ps3_hle_register(0x1395D8D1u, "cellHttpClientSetSslCallback",      (void*)http_ok);

    /* Still unidentified -- no name in the SDK surface hashes to these. They
     * are all called during client setup and none of their results are read
     * before the first request, so CELL_OK keeps the client moving. */
    ps3_hle_register(0x2033B878u, "cellHttp_0x2033B878", (void*)http_ok);
    ps3_hle_register(0x42205FE0u, "cellHttp_0x42205FE0", (void*)http_ok);
    ps3_hle_register(0x54F2A4DEu, "cellHttp_0x54F2A4DE", (void*)http_ok);
    ps3_hle_register(0x051EE3EEu, "sys_net_0x051EE3EE", (void*)http_ok);
    ps3_hle_register(0x3F09E20Au, "sys_net_0x3F09E20A", (void*)http_ok);
    ps3_hle_register(0xC98A3146u, "sys_net_0xC98A3146", (void*)http_ok);

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
    ps3_hle_register(0xA50777C6u, "shutdown",      (void*)sys_net_bnet_shutdown);
    ps3_hle_register(0x6DB6E8CDu, "socketclose",   (void*)sys_net_bnet_close);
    ps3_hle_register(0x88F03575u, "setsockopt",    (void*)net_setsockopt);
    ps3_hle_register(0x5A045BD1u, "getsockopt",    (void*)net_getsockopt);
    ps3_hle_register(0x71F4C717u, "gethostbyname", (void*)net_gethostbyname);
    ps3_hle_register(0xDABBC2C0u, "inet_addr",     (void*)net_inet_addr);
    ps3_hle_register(0x6005CDE1u, "_sys_net_errno_loc",   (void*)net_errno_loc);
    ps3_hle_register(0xC9157D30u, "_sys_net_h_errno_loc", (void*)net_h_errno_loc);
}

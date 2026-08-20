/* Guest<->host marshalling for sys_net socketpoll / socketselect / inet_ntop.
 *
 * These are big-endian guest structures (RPCS3 Emu/Cell/lv2/sys_net.h):
 * pollfd {s32 fd; s16 events; s16 revents}, fd_set = 32 BE words with bit
 * (fd & 31) of word (fd >> 5), timeval = two BE s64. Getting one wrong makes
 * the shim wait on the wrong descriptor, which no log would show. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Not assert(): this builds with -DNDEBUG, which would compile the checks --
 * and the calls inside them -- away entirely. */
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        abort(); \
    } \
} while (0)

#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>

#include "sysNet.h"
#include "cellHttpUtil.h"
#include "taiko_tls.h"

/* taiko_net.c's constructor reads the online configuration once, before main,
 * so the test server has to be in the environment before it runs. */
__attribute__((constructor(101)))
static void configure_test_server(void)
{
    setenv("TAIKO_ONLINE_HOST", "taiko.example.net", 1);
}

static unsigned char g_guest_memory[0x0C001000];
unsigned char* vm_base = g_guest_memory;

/* Fake host poll: records the request, replays a canned answer. */
static sys_net_pollfd g_seen[8];
static uint32_t       g_seen_count;
static int32_t        g_seen_timeout_ms;
static int16_t        g_reply_revents[8];
static int32_t        g_reply_result;

int32_t sys_net_bnet_poll(sys_net_pollfd* fds, uint32_t nfds, int32_t timeout_ms)
{
    g_seen_count = nfds < 8 ? nfds : 8;
    g_seen_timeout_ms = timeout_ms;
    memcpy(g_seen, fds, g_seen_count * sizeof(*fds));
    for (uint32_t i = 0; i < g_seen_count; i++)
        fds[i].revents = g_reply_revents[i];
    return g_reply_result;
}

/* vm_write* in ppu_memory.h break any outstanding reservation; the runtime
 * owns those, and this test links neither. */
int g_resv_store_active = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }

/* cellHttp's transport hooks live in the runtime library, which this test does
 * not link; taiko_net.c installs them when a server is configured. */
int   (*g_http_redirect_target)(char* host, uint32_t host_size, uint32_t* port);
void* (*g_http_tls_open)(int fd, const char* sni);
int   (*g_http_tls_send)(void* session, const void* buf, uint32_t len);
int   (*g_http_tls_recv)(void* session, void* buf, uint32_t len);
void  (*g_http_tls_close)(void* session);

/* Everything else taiko_net.c references, unused by these tests. */
void ps3_hle_register(uint32_t nid, const char* name, void* handler)
{ (void)nid; (void)name; (void)handler; }
int32_t sys_net_bnet_socket(int32_t a, int32_t b, int32_t c) { (void)a;(void)b;(void)c; return -1; }
int32_t sys_net_bnet_bind(int32_t a, const sys_net_sockaddr* b, uint32_t c) { (void)a;(void)b;(void)c; return -1; }
int32_t sys_net_bnet_listen(int32_t a, int32_t b) { (void)a;(void)b; return -1; }
int32_t sys_net_bnet_accept(int32_t a, sys_net_sockaddr* b, uint32_t* c) { (void)a;(void)b;(void)c; return -1; }
int32_t sys_net_bnet_connect(int32_t a, const sys_net_sockaddr* b, uint32_t c) { (void)a;(void)b;(void)c; return -1; }
int32_t sys_net_bnet_send(int32_t a, const void* b, uint32_t c, int32_t d) { (void)a;(void)b;(void)c;(void)d; return -1; }
int32_t sys_net_bnet_sendto(int32_t a, const void* b, uint32_t c, int32_t d, const sys_net_sockaddr* e, uint32_t f)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; return -1; }
int32_t sys_net_bnet_recv(int32_t a, void* b, uint32_t c, int32_t d) { (void)a;(void)b;(void)c;(void)d; return -1; }
int32_t sys_net_bnet_recvfrom(int32_t a, void* b, uint32_t c, int32_t d, sys_net_sockaddr* e, uint32_t* f)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; return -1; }
int32_t sys_net_bnet_shutdown(int32_t a, int32_t b) { (void)a; (void)b; return 0; }
int32_t sys_net_bnet_close(int32_t a) { (void)a; return -1; }
int32_t sys_net_bnet_setsockopt(int32_t a, int32_t b, int32_t c, const void* d, uint32_t e)
{ (void)a;(void)b;(void)c;(void)d;(void)e; return -1; }
int32_t sys_net_bnet_getsockopt(int32_t a, int32_t b, int32_t c, void* d, uint32_t* e)
{ (void)a;(void)b;(void)c;(void)d;(void)e; return -1; }
int32_t sys_net_bnet_inet_aton(const char* a, uint32_t* b) { (void)a; (void)b; return 0; }
uint32_t sys_net_bnet_gethostbyname(const char* a) { (void)a; return 0; }
int32_t cellHttpGetStatusCode(uint32_t a, int32_t* b) { (void)a; (void)b; return 0; }
int32_t cellHttpGetResponseContentLength(uint32_t a, uint64_t* b) { (void)a; (void)b; return 0; }
int32_t cellHttpSetRequestContentLength(uint32_t a, uint64_t b) { (void)a; (void)b; return 0; }
int32_t cellHttpSetConnectTimeOut(uint32_t a, uint32_t b) { (void)a; (void)b; return 0; }
int32_t cellHttpAbortTransaction(uint32_t a) { (void)a; return 0; }
static uint32_t g_header_trans, g_header_name_ea, g_header_value_ea;
int32_t cellHttpAddRequestHeader(uint32_t trans, const char* name, const char* value)
{
    g_header_trans = trans;
    g_header_name_ea = (uint32_t)(uintptr_t)name;
    g_header_value_ea = (uint32_t)(uintptr_t)value;
    return 0;
}

#include "../src/taiko_net.c"

#define POLLFDS_EA 0x00100000u
#define READ_EA    0x00101000u
#define WRITE_EA   0x00102000u
#define TIMEVAL_EA 0x00103000u
#define SCRATCH_EA 0x00104000u

static void test_socketpoll(void)
{
    memset(vm_base + POLLFDS_EA, 0, 32);
    net_write_be32(POLLFDS_EA + 0, 7);
    net_write_be16(POLLFDS_EA + 4, SYS_NET_POLLIN);
    net_write_be32(POLLFDS_EA + 8, 9);
    net_write_be16(POLLFDS_EA + 12, SYS_NET_POLLOUT);

    g_reply_result = 1;
    g_reply_revents[0] = SYS_NET_POLLIN;
    g_reply_revents[1] = 0;

    CHECK(net_socketpoll(POLLFDS_EA, 2, 250) == 1);
    CHECK(g_seen_count == 2);
    CHECK(g_seen[0].fd == 7 && g_seen[0].events == SYS_NET_POLLIN);
    CHECK(g_seen[1].fd == 9 && g_seen[1].events == SYS_NET_POLLOUT);
    CHECK(g_seen_timeout_ms == 250);
    CHECK(net_read_be16(POLLFDS_EA + 6) == SYS_NET_POLLIN);
    CHECK(net_read_be16(POLLFDS_EA + 14) == 0);

    /* A negative (infinite) timeout must still wait, not return instantly. */
    net_socketpoll(POLLFDS_EA, 2, -1);
    CHECK(g_seen_timeout_ms == NET_WAIT_CAP_MS);
}

static void test_socketselect(void)
{
    memset(vm_base + READ_EA, 0, 128);
    memset(vm_base + WRITE_EA, 0, 128);
    net_fd_set(READ_EA, 3);
    net_fd_set(READ_EA, 40);      /* second fd_set word: exercises fd >> 5 */
    net_fd_set(WRITE_EA, 40);
    CHECK(net_fd_isset(READ_EA, 3) && net_fd_isset(READ_EA, 40));
    CHECK(!net_fd_isset(READ_EA, 4));

    /* 1.5 s, as two big-endian 64-bit fields. */
    memset(vm_base + TIMEVAL_EA, 0, 16);
    net_write_be32(TIMEVAL_EA + 4, 1);
    net_write_be32(TIMEVAL_EA + 12, 500000);

    g_reply_result = 1;
    g_reply_revents[0] = 0;                  /* fd 3 not ready */
    g_reply_revents[1] = SYS_NET_POLLOUT;    /* fd 40 writable */

    int32_t ready = net_socketselect(64, READ_EA, WRITE_EA, 0, TIMEVAL_EA);
    CHECK(g_seen_count == 2);
    CHECK(g_seen[0].fd == 3 && g_seen[0].events == SYS_NET_POLLIN);
    CHECK(g_seen[1].fd == 40 &&
           g_seen[1].events == (SYS_NET_POLLIN | SYS_NET_POLLOUT));
    CHECK(g_seen_timeout_ms == 1500);
    CHECK(ready == 1);
    CHECK(!net_fd_isset(READ_EA, 3) && !net_fd_isset(READ_EA, 40));
    CHECK(net_fd_isset(WRITE_EA, 40));

    /* An error is reported on both sets so a one-set caller makes progress. */
    memset(vm_base + READ_EA, 0, 128);
    memset(vm_base + WRITE_EA, 0, 128);
    net_fd_set(READ_EA, 5);
    g_reply_result = 1;
    g_reply_revents[0] = SYS_NET_POLLHUP;
    CHECK(net_socketselect(8, READ_EA, 0, 0, TIMEVAL_EA) == 1);
    CHECK(net_fd_isset(READ_EA, 5));

    /* Empty sets are a plain sleep, not a poll on nothing. */
    memset(vm_base + READ_EA, 0, 128);
    g_seen_count = 0;
    CHECK(net_socketselect(8, READ_EA, 0, 0, TIMEVAL_EA) == 0);
    CHECK(g_seen_count == 0);
}

static void test_inet_ntop_and_header(void)
{
    vm_base[SCRATCH_EA + 0] = 192; vm_base[SCRATCH_EA + 1] = 168;
    vm_base[SCRATCH_EA + 2] = 0;   vm_base[SCRATCH_EA + 3] = 42;
    CHECK(net_inet_ntop(2, SCRATCH_EA, SCRATCH_EA + 16, 16) == SCRATCH_EA + 16);
    CHECK(strcmp((const char*)(vm_base + SCRATCH_EA + 16), "192.168.0.42") == 0);
    CHECK(net_inet_ntop(2, SCRATCH_EA, SCRATCH_EA + 16, 4) == 0);   /* too small */
    CHECK(net_inet_ntop(10, SCRATCH_EA, SCRATCH_EA + 16, 64) == 0); /* AF_INET6 */

    /* CellHttpHeader is {name EA, value EA}; both must reach cellHttp. */
    net_write_be32(SCRATCH_EA + 64, 0x00105000u);
    net_write_be32(SCRATCH_EA + 68, 0x00105100u);
    net_http_request_set_header(3, SCRATCH_EA + 64);
    CHECK(g_header_trans == 3);
    CHECK(g_header_name_ea == 0x00105000u);
    CHECK(g_header_value_ea == 0x00105100u);
}

/* Live TLS check, opt-in because it needs a reachable server:
 *   TAIKO_TLS_LIVE_HOST=my.server TAIKO_TLS_LIVE_PORT=443 ./net_select_tests
 * Proves the mbedTLS wrapper actually completes a handshake and moves bytes,
 * which no offline assertion can. */
static void test_live_handshake(void)
{
    const char* host = getenv("TAIKO_TLS_LIVE_HOST");
    const char* port = getenv("TAIKO_TLS_LIVE_PORT");
    if (!host || !host[0]) {
        printf("net_select_tests: live handshake skipped "
               "(set TAIKO_TLS_LIVE_HOST to run it)\n");
        return;
    }

    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    CHECK(getaddrinfo(host, port && port[0] ? port : "443", &hints, &result) == 0);

    int fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    CHECK(fd >= 0);
    CHECK(connect(fd, result->ai_addr, result->ai_addrlen) == 0);
    freeaddrinfo(result);

    taiko_tls* tls = taiko_tls_open_socket(fd, host);
    CHECK(tls != NULL);

    char request[512];
    int len = snprintf(request, sizeof(request),
                       "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    CHECK(taiko_tls_send(tls, request, (size_t)len) == 0);

    char reply[256];
    int got = taiko_tls_recv(tls, reply, sizeof(reply) - 1);
    CHECK(got > 0);
    reply[got] = '\0';
    CHECK(strncmp(reply, "HTTP/", 5) == 0);
    printf("net_select_tests: live handshake with %s -> %.*s\n", host,
           (int)strcspn(reply, "\r\n"), reply);

    taiko_tls_close(tls);
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

/* cellHttpUtilParseUri writes guest pointers into a guest struct, using a
 * caller-supplied pool. Taking its arguments as host pointers segfaulted the
 * moment the title parsed the URL ALL.Net returns. */
static void test_parse_uri(void)
{
    const uint32_t URL_EA = 0x00106000u, URI_EA = 0x00106100u, POOL_EA = 0x00106200u;
    const uint32_t REQ_EA = 0x00106400u;
    const char* url = "https://user:pw@taiko.example.net:10122/mucha/boardauth.do?x=1";
    strcpy((char*)(vm_base + URL_EA), url);
    memset(vm_base + URI_EA, 0xCD, 32);
    memset(vm_base + POOL_EA, 0, 256);

    /* uri == NULL asks only for the pool size. */
    CHECK(cellHttpUtilParseUri(NULL, (const char*)(uintptr_t)URL_EA, NULL, 0,
                               (uint32_t*)(uintptr_t)REQ_EA) == 0);
    uint32_t needed = net_read_be32(REQ_EA);
    CHECK(needed > 0 && needed < 256);

    /* Too small a pool must fail rather than write past it. */
    CHECK(cellHttpUtilParseUri((CellHttpUtilUri*)(uintptr_t)URI_EA,
                               (const char*)(uintptr_t)URL_EA,
                               (void*)(uintptr_t)POOL_EA, needed - 1,
                               (uint32_t*)(uintptr_t)REQ_EA) != 0);

    CHECK(cellHttpUtilParseUri((CellHttpUtilUri*)(uintptr_t)URI_EA,
                               (const char*)(uintptr_t)URL_EA,
                               (void*)(uintptr_t)POOL_EA, needed,
                               (uint32_t*)(uintptr_t)REQ_EA) == 0);

    const char* scheme   = (const char*)(vm_base + net_read_be32(URI_EA + 0));
    const char* hostname = (const char*)(vm_base + net_read_be32(URI_EA + 4));
    const char* username = (const char*)(vm_base + net_read_be32(URI_EA + 8));
    const char* password = (const char*)(vm_base + net_read_be32(URI_EA + 12));
    const char* path     = (const char*)(vm_base + net_read_be32(URI_EA + 16));
    CHECK(strcmp(scheme, "https") == 0);
    CHECK(strcmp(hostname, "taiko.example.net") == 0);
    CHECK(strcmp(username, "user") == 0);
    CHECK(strcmp(password, "pw") == 0);
    CHECK(strcmp(path, "/mucha/boardauth.do?x=1") == 0);
    CHECK(net_read_be32(URI_EA + 20) == 10122);
    /* Every pointer must land inside the pool the caller gave us. */
    CHECK(net_read_be32(URI_EA + 0) >= POOL_EA &&
          net_read_be32(URI_EA + 16) < POOL_EA + needed);

    /* Bare host, no scheme and no path: default port, path "/". */
    strcpy((char*)(vm_base + URL_EA), "127.0.0.1");
    CHECK(cellHttpUtilParseUri((CellHttpUtilUri*)(uintptr_t)URI_EA,
                               (const char*)(uintptr_t)URL_EA,
                               (void*)(uintptr_t)POOL_EA, 256,
                               (uint32_t*)(uintptr_t)REQ_EA) == 0);
    CHECK(net_read_be32(URI_EA + 0) == 0);   /* no scheme */
    CHECK(strcmp((const char*)(vm_base + net_read_be32(URI_EA + 4)), "127.0.0.1") == 0);
    CHECK(strcmp((const char*)(vm_base + net_read_be32(URI_EA + 16)), "/") == 0);
    CHECK(net_read_be32(URI_EA + 20) == 80);
}

/* The raw ALL.Net path writes its own HTTP; only the Host value may change. */
static void test_host_rewrite(void)
{
    CHECK(taiko_online_enabled());
    CHECK(strcmp(taiko_online_host(), "taiko.example.net") == 0);

    const char* request =
        "POST /sys/servlet/PowerOn HTTP/1.1\r\n"
        "Host: naominet.jp\r\n"
        "User-Agent: ALL.Net_MW/2.0\r\n"
        "Content-Length: 4\r\n"
        "\r\n"
        "body";
    char out[512];
    uint32_t len = net_rewrite_host_header(request, (uint32_t)strlen(request),
                                           out, sizeof(out));
    CHECK(len > 0);
    out[len] = '\0';
    CHECK(strstr(out, "Host: taiko.example.net\r\n") != NULL);
    CHECK(strstr(out, "naominet.jp") == NULL);
    CHECK(strstr(out, "POST /sys/servlet/PowerOn HTTP/1.1\r\n") == out);
    CHECK(strstr(out, "User-Agent: ALL.Net_MW/2.0\r\n") != NULL);
    CHECK(strstr(out, "Content-Length: 4\r\n\r\nbody") != NULL);
    CHECK(len == strlen(out));

    /* Anything that is not an HTTP request head is passed through untouched. */
    CHECK(net_rewrite_host_header("\x01\x02 binary payload", 18, out, sizeof(out)) == 0);
    /* A Host in the body must not be touched: no header block, no rewrite. */
    const char* no_host = "GET / HTTP/1.1\r\nAccept: */*\r\n\r\nHost: nope\r\n";
    CHECK(net_rewrite_host_header(no_host, (uint32_t)strlen(no_host), out, sizeof(out)) == 0);
}

int main(void)
{
    test_socketpoll();
    test_socketselect();
    test_inet_ntop_and_header();
    test_host_rewrite();
    test_parse_uri();
    test_live_handshake();
    printf("net_select_tests: OK\n");
    return 0;
}

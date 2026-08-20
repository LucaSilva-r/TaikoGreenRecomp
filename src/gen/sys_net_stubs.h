/* Auto-generated HLE stubs for sys_net -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for sys_net */
/* NID 0x051EE3EE */ int32_t nid_0x051EE3EE_stub(ppu_context* ctx);
/* NID 0x139A9E9B */ int32_t sys_net_initialize_network_ex_stub(ppu_context* ctx);
/* NID 0x1F953B9F */ int32_t recvfrom_stub(ppu_context* ctx);
/* NID 0x28E208BB */ int32_t listen_stub(ppu_context* ctx);
/* NID 0x3B27C780 */ int32_t sys_net_get_sockinfo_stub(ppu_context* ctx);
/* NID 0x3F09E20A */ int32_t nid_0x3F09E20A_stub(ppu_context* ctx);
/* NID 0x5A045BD1 */ int32_t getsockopt_stub(ppu_context* ctx);
/* NID 0x6005CDE1 */ int32_t _sys_net_errno_loc_stub(ppu_context* ctx);
/* NID 0x64F66D35 */ int32_t connect_stub(ppu_context* ctx);
/* NID 0x6DB6E8CD */ int32_t socketclose_stub(ppu_context* ctx);
/* NID 0x71F4C717 */ int32_t gethostbyname_stub(ppu_context* ctx);
/* NID 0x88F03575 */ int32_t setsockopt_stub(ppu_context* ctx);
/* NID 0x8D1B77FB */ int32_t sys_net_abort_socket_stub(ppu_context* ctx);
/* NID 0x9647570B */ int32_t sendto_stub(ppu_context* ctx);
/* NID 0x9C056962 */ int32_t socket_stub(ppu_context* ctx);
/* NID 0xA50777C6 */ int32_t shutdown_stub(ppu_context* ctx);
/* NID 0xA765D029 */ int32_t sys_net_get_sockinfo_ex_stub(ppu_context* ctx);
/* NID 0xB0A59804 */ int32_t bind_stub(ppu_context* ctx);
/* NID 0xB48636C4 */ int32_t sys_net_show_ifconfig_stub(ppu_context* ctx);
/* NID 0xC9157D30 */ int32_t _sys_net_h_errno_loc_stub(ppu_context* ctx);
/* NID 0xC94F6939 */ int32_t accept_stub(ppu_context* ctx);
/* NID 0xC98A3146 */ int32_t nid_0xC98A3146_stub(ppu_context* ctx);
/* NID 0xDABBC2C0 */ int32_t inet_addr_stub(ppu_context* ctx);
/* NID 0xDC751B40 */ int32_t send_stub(ppu_context* ctx);
/* NID 0xFBA04F37 */ int32_t recv_stub(ppu_context* ctx);
/* NID 0xFDB8F926 */ int32_t sys_net_free_thread_context_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_sys_net;
extern const nid_entry_sys_net sys_net_nid_table[];
extern const int sys_net_nid_table_size;

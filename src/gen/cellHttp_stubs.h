/* Auto-generated HLE stubs for cellHttp -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for cellHttp */
/* NID 0x052A80D9 */ int32_t cellHttpCreateTransaction_stub(ppu_context* ctx);
/* NID 0x10D0D7FC */ int32_t nid_0x10D0D7FC_stub(ppu_context* ctx);
/* NID 0x1395D8D1 */ int32_t nid_0x1395D8D1_stub(ppu_context* ctx);
/* NID 0x2033B878 */ int32_t nid_0x2033B878_stub(ppu_context* ctx);
/* NID 0x250C386C */ int32_t cellHttpInit_stub(ppu_context* ctx);
/* NID 0x2D52848B */ int32_t nid_0x2D52848B_stub(ppu_context* ctx);
/* NID 0x32F5CAE2 */ int32_t cellHttpDestroyTransaction_stub(ppu_context* ctx);
/* NID 0x42205FE0 */ int32_t nid_0x42205FE0_stub(ppu_context* ctx);
/* NID 0x464FF889 */ int32_t nid_0x464FF889_stub(ppu_context* ctx);
/* NID 0x4E4EE53A */ int32_t cellHttpCreateClient_stub(ppu_context* ctx);
/* NID 0x522180BC */ int32_t cellHttpsInit_stub(ppu_context* ctx);
/* NID 0x54F2A4DE */ int32_t nid_0x54F2A4DE_stub(ppu_context* ctx);
/* NID 0x5D473170 */ int32_t nid_0x5D473170_stub(ppu_context* ctx);
/* NID 0x61C90691 */ int32_t cellHttpRecvResponse_stub(ppu_context* ctx);
/* NID 0x980855AC */ int32_t cellHttpDestroyClient_stub(ppu_context* ctx);
/* NID 0xA0D9223C */ int32_t nid_0xA0D9223C_stub(ppu_context* ctx);
/* NID 0xA755B005 */ int32_t cellHttpSendRequest_stub(ppu_context* ctx);
/* NID 0xAF73A64E */ int32_t nid_0xAF73A64E_stub(ppu_context* ctx);
/* NID 0xD7471088 */ int32_t nid_0xD7471088_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_cellHttp;
extern const nid_entry_cellHttp cellHttp_nid_table[];
extern const int cellHttp_nid_table_size;

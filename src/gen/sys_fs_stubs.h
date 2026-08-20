/* Auto-generated HLE stubs for sys_fs -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for sys_fs */
/* NID 0x1EA02E2F */ int32_t nid_0x1EA02E2F_stub(ppu_context* ctx);
/* NID 0x2CB51F0D */ int32_t cellFsClose_stub(ppu_context* ctx);
/* NID 0x3F61245C */ int32_t cellFsOpendir_stub(ppu_context* ctx);
/* NID 0x4CEF342E */ int32_t cellFsAioWrite_stub(ppu_context* ctx);
/* NID 0x4D5FF8E2 */ int32_t cellFsRead_stub(ppu_context* ctx);
/* NID 0x5C74903D */ int32_t cellFsReaddir_stub(ppu_context* ctx);
/* NID 0x718BF5F8 */ int32_t cellFsOpen_stub(ppu_context* ctx);
/* NID 0x7DE6DCED */ int32_t cellFsStat_stub(ppu_context* ctx);
/* NID 0x7F4677A8 */ int32_t cellFsUnlink_stub(ppu_context* ctx);
/* NID 0x9F951810 */ int32_t cellFsAioFinish_stub(ppu_context* ctx);
/* NID 0xA397D042 */ int32_t cellFsLseek_stub(ppu_context* ctx);
/* NID 0xBA901FE6 */ int32_t cellFsMkdir_stub(ppu_context* ctx);
/* NID 0xC1C507E7 */ int32_t cellFsAioRead_stub(ppu_context* ctx);
/* NID 0xDB869F20 */ int32_t cellFsAioInit_stub(ppu_context* ctx);
/* NID 0xECDCF2AB */ int32_t cellFsWrite_stub(ppu_context* ctx);
/* NID 0xEF3EFA34 */ int32_t cellFsFstat_stub(ppu_context* ctx);
/* NID 0xF12EECC8 */ int32_t cellFsRename_stub(ppu_context* ctx);
/* NID 0xFF42DCC3 */ int32_t cellFsClosedir_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_sys_fs;
extern const nid_entry_sys_fs sys_fs_nid_table[];
extern const int sys_fs_nid_table_size;

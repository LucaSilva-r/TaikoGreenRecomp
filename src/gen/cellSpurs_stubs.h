/* Auto-generated HLE stubs for cellSpurs -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for cellSpurs */
/* NID 0x07529113 */ int32_t cellSpursAttributeSetNamePrefix_stub(ppu_context* ctx);
/* NID 0x1051D134 */ int32_t cellSpursAttributeEnableSpuPrintfIfAvailable_stub(ppu_context* ctx);
/* NID 0x1EBCF459 */ int32_t cellSpursDestroyTaskset2_stub(ppu_context* ctx);
/* NID 0x22AAB31D */ int32_t cellSpursEventFlagDetachLv2EventQueue_stub(ppu_context* ctx);
/* NID 0x303C19CD */ int32_t cellSpursCreateJobChainWithAttribute_stub(ppu_context* ctx);
/* NID 0x30AA96C4 */ int32_t cellSpursInitializeWithAttribute2_stub(ppu_context* ctx);
/* NID 0x3548F483 */ int32_t _cellSpursJobChainAttributeInitialize_stub(ppu_context* ctx);
/* NID 0x373523D4 */ int32_t cellSpursEventFlagWait_stub(ppu_context* ctx);
/* NID 0x4A6465E3 */ int32_t cellSpursCreateTaskset2_stub(ppu_context* ctx);
/* NID 0x4E66D483 */ int32_t cellSpursDetachLv2EventQueue_stub(ppu_context* ctx);
/* NID 0x5EF96465 */ int32_t _cellSpursEventFlagInitialize_stub(ppu_context* ctx);
/* NID 0x68AAEBA9 */ int32_t cellSpursJobGuardInitialize_stub(ppu_context* ctx);
/* NID 0x738E40E6 */ int32_t cellSpursShutdownJobChain_stub(ppu_context* ctx);
/* NID 0x838FA4F0 */ int32_t cellSpursTryJoinTask2_stub(ppu_context* ctx);
/* NID 0x86C864A2 */ int32_t nid_0x86C864A2_stub(ppu_context* ctx);
/* NID 0x87630976 */ int32_t cellSpursEventFlagAttachLv2EventQueue_stub(ppu_context* ctx);
/* NID 0x95180230 */ int32_t _cellSpursAttributeInitialize_stub(ppu_context* ctx);
/* NID 0xA7A94892 */ int32_t cellSpursJoinTask2_stub(ppu_context* ctx);
/* NID 0xA7C066DE */ int32_t cellSpursJoinJobChain_stub(ppu_context* ctx);
/* NID 0xAA6269A8 */ int32_t cellSpursInitializeWithAttribute_stub(ppu_context* ctx);
/* NID 0xB9BC6207 */ int32_t cellSpursAttachLv2EventQueue_stub(ppu_context* ctx);
/* NID 0xC2ACDF43 */ int32_t _cellSpursTasksetAttribute2Initialize_stub(ppu_context* ctx);
/* NID 0xCA4C4600 */ int32_t cellSpursFinalize_stub(ppu_context* ctx);
/* NID 0xD5D0B256 */ int32_t cellSpursJobGuardNotify_stub(ppu_context* ctx);
/* NID 0xE14CA62D */ int32_t cellSpursCreateTask2_stub(ppu_context* ctx);
/* NID 0xF31731BB */ int32_t cellSpursRunJobChain_stub(ppu_context* ctx);
/* NID 0xF5507729 */ int32_t cellSpursEventFlagSet_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_cellSpurs;
extern const nid_entry_cellSpurs cellSpurs_nid_table[];
extern const int cellSpurs_nid_table_size;

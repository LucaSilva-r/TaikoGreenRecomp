/* Auto-generated HLE stubs for cellResc -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for cellResc */
/* NID 0x10DB5B1A */ int32_t cellRescSetDsts_stub(ppu_context* ctx);
/* NID 0x22AE06D8 */ int32_t cellRescAdjustAspectRatio_stub(ppu_context* ctx);
/* NID 0x23134710 */ int32_t cellRescSetDisplayMode_stub(ppu_context* ctx);
/* NID 0x25C107E6 */ int32_t cellRescSetConvertAndFlip_stub(ppu_context* ctx);
/* NID 0x2EA3061E */ int32_t cellRescExit_stub(ppu_context* ctx);
/* NID 0x2EA94661 */ int32_t cellRescSetFlipHandler_stub(ppu_context* ctx);
/* NID 0x516EE89E */ int32_t cellRescInit_stub(ppu_context* ctx);
/* NID 0x5A338CDB */ int32_t cellRescGetBufferSize_stub(ppu_context* ctx);
/* NID 0x6CD0F95F */ int32_t cellRescSetSrc_stub(ppu_context* ctx);
/* NID 0x8107277C */ int32_t cellRescSetBufferAddress_stub(ppu_context* ctx);
/* NID 0xD1CA0503 */ int32_t cellRescVideoOutResolutionId2RescBufferMode_stub(ppu_context* ctx);
/* NID 0xD3758645 */ int32_t cellRescSetVBlankHandler_stub(ppu_context* ctx);
/* NID 0xE0CEF79E */ int32_t cellRescCreateInterlaceTable_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_cellResc;
extern const nid_entry_cellResc cellResc_nid_table[];
extern const int cellResc_nid_table_size;

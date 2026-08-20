/* Auto-generated HLE stubs for cellGcmSys -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for cellGcmSys */
/* NID 0x63387071 */ int32_t cellGcmGetLastFlipTime_stub(ppu_context* ctx);
/* NID 0x055BD74D */ int32_t cellGcmGetTiledPitchSize_stub(ppu_context* ctx);
/* NID 0x21AC3697 */ int32_t cellGcmAddressToOffset_stub(ppu_context* ctx);
/* NID 0x4524CCCD */ int32_t cellGcmBindTile_stub(ppu_context* ctx);
/* NID 0x4AE8D215 */ int32_t cellGcmSetFlipMode_stub(ppu_context* ctx);
/* NID 0x06EDEA9E */ int32_t cellGcmSetUserHandler_stub(ppu_context* ctx);
/* NID 0xA114EC67 */ int32_t cellGcmMapMainMemory_stub(ppu_context* ctx);
/* NID 0xA41EF7E8 */ int32_t cellGcmSetFlipHandler_stub(ppu_context* ctx);
/* NID 0xA53D12AE */ int32_t cellGcmSetDisplayBuffer_stub(ppu_context* ctx);
/* NID 0xA547ADDE */ int32_t cellGcmGetControlRegister_stub(ppu_context* ctx);
/* NID 0xA91B0402 */ int32_t cellGcmSetVBlankHandler_stub(ppu_context* ctx);
/* NID 0xBD100DBC */ int32_t cellGcmSetTileInfo_stub(ppu_context* ctx);
/* NID 0xBD6D60D9 */ int32_t cellGcmSetInvalidateTile_stub(ppu_context* ctx);
/* NID 0xD34A420D */ int32_t cellGcmSetZcull_stub(ppu_context* ctx);
/* NID 0xD9B7653E */ int32_t cellGcmUnbindTile_stub(ppu_context* ctx);
/* NID 0xE315A0B2 */ int32_t cellGcmGetConfiguration_stub(ppu_context* ctx);
/* NID 0xEFD00F54 */ int32_t cellGcmUnmapEaIoAddress_stub(ppu_context* ctx);
/* NID 0xF80196C1 */ int32_t cellGcmGetLabelAddress_stub(ppu_context* ctx);
/* NID 0x15BAE46B */ int32_t _cellGcmInitBody_stub(ppu_context* ctx);
/* NID 0x21397818 */ int32_t _cellGcmSetFlipCommand_stub(ppu_context* ctx);
/* NID 0x3A33C1FD */ int32_t _cellGcmFunc15_stub(ppu_context* ctx);
/* NID 0x5E2EE0F0 */ int32_t cellGcmGetDefaultCommandWordSize_stub(ppu_context* ctx);
/* NID 0x8CDF8C70 */ int32_t cellGcmGetDefaultSegmentWordSize_stub(ppu_context* ctx);
/* NID 0x9BA451E4 */ int32_t cellGcmSetDefaultFifoSize_stub(ppu_context* ctx);
/* NID 0xD8F88E1A */ int32_t _cellGcmSetFlipCommandWithWaitLabel_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_cellGcmSys;
extern const nid_entry_cellGcmSys cellGcmSys_nid_table[];
extern const int cellGcmSys_nid_table_size;

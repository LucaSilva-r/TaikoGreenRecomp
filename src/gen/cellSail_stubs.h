/* Auto-generated HLE stubs for cellSail -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for cellSail */
/* NID 0x0247C69E */ int32_t cellSailGraphicsAdapterGetFrame_stub(ppu_context* ctx);
/* NID 0x1139A206 */ int32_t cellSailPlayerSetSoundAdapter_stub(ppu_context* ctx);
/* NID 0x1872331B */ int32_t cellSailGraphicsAdapterPtsToTimePosition_stub(ppu_context* ctx);
/* NID 0x18B4629D */ int32_t cellSailPlayerFinalize_stub(ppu_context* ctx);
/* NID 0x18BCD21B */ int32_t cellSailPlayerSetGraphicsAdapter_stub(ppu_context* ctx);
/* NID 0x1C983864 */ int32_t cellSailGraphicsAdapterInitialize_stub(ppu_context* ctx);
/* NID 0x1C9D5E5A */ int32_t nid_0x1C9D5E5A_stub(ppu_context* ctx);
/* NID 0x23654375 */ int32_t nid_0x23654375_stub(ppu_context* ctx);
/* NID 0x2E3CCB5E */ int32_t cellSailGraphicsAdapterSetPreferredFormat_stub(ppu_context* ctx);
/* NID 0x346EBBA3 */ int32_t cellSailMemAllocatorInitialize_stub(ppu_context* ctx);
/* NID 0x34ECC1B9 */ int32_t cellSailPlayerOpenStream_stub(ppu_context* ctx);
/* NID 0x3D0D3B72 */ int32_t cellSailSoundAdapterInitialize_stub(ppu_context* ctx);
/* NID 0x44A20E79 */ int32_t cellSailGraphicsAdapterUpdateAvSync_stub(ppu_context* ctx);
/* NID 0x5F7C7A6F */ int32_t cellSailPlayerSetParameter_stub(ppu_context* ctx);
/* NID 0x752F8585 */ int32_t nid_0x752F8585_stub(ppu_context* ctx);
/* NID 0x76488BB1 */ int32_t cellSailGraphicsAdapterFinalize_stub(ppu_context* ctx);
/* NID 0x7C8DFF3B */ int32_t cellSailPlayerAddDescriptor_stub(ppu_context* ctx);
/* NID 0x7EB8D6B5 */ int32_t cellSailSoundAdapterGetFrame_stub(ppu_context* ctx);
/* NID 0x85BEFFCC */ int32_t cellSailPlayerCloseStream_stub(ppu_context* ctx);
/* NID 0x9897FBD1 */ int32_t cellSailPlayerRemoveDescriptor_stub(ppu_context* ctx);
/* NID 0xAAFA17B8 */ int32_t cellSailPlayerIsPaused_stub(ppu_context* ctx);
/* NID 0xBDF21B0F */ int32_t cellSailPlayerBoot_stub(ppu_context* ctx);
/* NID 0xD1462438 */ int32_t cellSailSoundAdapterFinalize_stub(ppu_context* ctx);
/* NID 0xD1D55A90 */ int32_t nid_0xD1D55A90_stub(ppu_context* ctx);
/* NID 0xD7938B8D */ int32_t cellSailPlayerCreateDescriptor_stub(ppu_context* ctx);
/* NID 0xE535B0D3 */ int32_t cellSailPlayerStart_stub(ppu_context* ctx);
/* NID 0xEBA8D4EC */ int32_t cellSailPlayerStop_stub(ppu_context* ctx);
/* NID 0xEEC22809 */ int32_t cellSailSoundAdapterUpdateAvSync_stub(ppu_context* ctx);
/* NID 0xF25F197D */ int32_t cellSailSoundAdapterGetFormat_stub(ppu_context* ctx);
/* NID 0xFC5BAF8A */ int32_t cellSailPlayerSetRepeatMode_stub(ppu_context* ctx);
/* NID 0xFC839BD4 */ int32_t cellSailPlayerDestroyDescriptor_stub(ppu_context* ctx);
/* NID 0xFFD58AA4 */ int32_t cellSailGraphicsAdapterGetFormat_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_cellSail;
extern const nid_entry_cellSail cellSail_nid_table[];
extern const int cellSail_nid_table_size;

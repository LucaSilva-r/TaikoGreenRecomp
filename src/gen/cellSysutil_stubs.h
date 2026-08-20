/* Auto-generated HLE stubs for cellSysutil -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for cellSysutil */
/* NID 0x0BAE8772 */ int32_t cellVideoOutConfigure_stub(ppu_context* ctx);
/* NID 0x189A74DA */ int32_t cellSysutilCheckCallback_stub(ppu_context* ctx);
/* NID 0x1E7BFF94 */ int32_t cellSysCacheMount_stub(ppu_context* ctx);
/* NID 0x220894E3 */ int32_t cellSysutilEnableBgmPlayback_stub(ppu_context* ctx);
/* NID 0x744C1544 */ int32_t cellSysCacheClear_stub(ppu_context* ctx);
/* NID 0x75BBB672 */ int32_t cellVideoOutGetNumberOfDevice_stub(ppu_context* ctx);
/* NID 0x887572D5 */ int32_t cellVideoOutGetState_stub(ppu_context* ctx);
/* NID 0x9D98AFA0 */ int32_t cellSysutilRegisterCallback_stub(ppu_context* ctx);
/* NID 0xA11552F6 */ int32_t cellSysutilGetBgmPlaybackStatus_stub(ppu_context* ctx);
/* NID 0xA322DB75 */ int32_t cellVideoOutGetResolutionAvailability_stub(ppu_context* ctx);
/* NID 0xCFDD8E87 */ int32_t cellSysutilDisableBgmPlayback_stub(ppu_context* ctx);
/* NID 0xE558748D */ int32_t cellVideoOutGetResolution_stub(ppu_context* ctx);
/* NID 0xED5D96AF */ int32_t cellAudioOutGetConfiguration_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_cellSysutil;
extern const nid_entry_cellSysutil cellSysutil_nid_table[];
extern const int cellSysutil_nid_table_size;

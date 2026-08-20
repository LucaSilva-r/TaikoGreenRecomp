/* Auto-generated HLE stubs for cellAudio -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for cellAudio */
/* NID 0x0B168F92 */ int32_t cellAudioInit_stub(ppu_context* ctx);
/* NID 0x377E0CD9 */ int32_t cellAudioSetNotifyEventQueue_stub(ppu_context* ctx);
/* NID 0x4109D08C */ int32_t cellAudioGetPortTimestamp_stub(ppu_context* ctx);
/* NID 0x4129FE2D */ int32_t cellAudioPortClose_stub(ppu_context* ctx);
/* NID 0x56DFE179 */ int32_t cellAudioSetPortLevel_stub(ppu_context* ctx);
/* NID 0x5B1E2C73 */ int32_t cellAudioPortStop_stub(ppu_context* ctx);
/* NID 0x74A66AF0 */ int32_t cellAudioGetPortConfig_stub(ppu_context* ctx);
/* NID 0x832DF17E */ int32_t nid_0x832DF17E_stub(ppu_context* ctx);
/* NID 0x89BE28F2 */ int32_t cellAudioPortStart_stub(ppu_context* ctx);
/* NID 0x9E4B1DB8 */ int32_t nid_0x9E4B1DB8_stub(ppu_context* ctx);
/* NID 0xCA5AC370 */ int32_t cellAudioQuit_stub(ppu_context* ctx);
/* NID 0xCD7BC431 */ int32_t cellAudioPortOpen_stub(ppu_context* ctx);
/* NID 0xDAB029AA */ int32_t nid_0xDAB029AA_stub(ppu_context* ctx);
/* NID 0xE4046AFE */ int32_t cellAudioGetPortBlockTag_stub(ppu_context* ctx);
/* NID 0xFF3626FD */ int32_t cellAudioRemoveNotifyEventQueue_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_cellAudio;
extern const nid_entry_cellAudio cellAudio_nid_table[];
extern const int cellAudio_nid_table_size;

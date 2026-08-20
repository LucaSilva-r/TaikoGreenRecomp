/* Auto-generated HLE stubs for cellRtc -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for cellRtc */
/* NID 0x269A1882 */ int32_t nid_0x269A1882_stub(ppu_context* ctx);
/* NID 0x2F010BFA */ int32_t cellRtcTickAddMinutes_stub(ppu_context* ctx);
/* NID 0x332A74DD */ int32_t cellRtcTickAddYears_stub(ppu_context* ctx);
/* NID 0x75744E2A */ int32_t cellRtcTickAddDays_stub(ppu_context* ctx);
/* NID 0x99B13034 */ int32_t cellRtcSetTick_stub(ppu_context* ctx);
/* NID 0x9DAFC0D9 */ int32_t cellRtcGetCurrentTick_stub(ppu_context* ctx);
/* NID 0xBB543189 */ int32_t cellRtcSetTime_t_stub(ppu_context* ctx);
/* NID 0xC2D8CF95 */ int32_t cellRtcGetDayOfWeek_stub(ppu_context* ctx);
/* NID 0xC48D5002 */ int32_t cellRtcConvertUtcToLocalTime_stub(ppu_context* ctx);
/* NID 0xC7BDB7EB */ int32_t cellRtcGetTick_stub(ppu_context* ctx);
/* NID 0xCB90C761 */ int32_t cellRtcGetTime_t_stub(ppu_context* ctx);
/* NID 0xCCCE71BD */ int32_t cellRtcTickAddSeconds_stub(ppu_context* ctx);
/* NID 0xD41D3BD2 */ int32_t cellRtcTickAddHours_stub(ppu_context* ctx);
/* NID 0xE0ECBB45 */ int32_t cellRtcTickAddMonths_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_cellRtc;
extern const nid_entry_cellRtc cellRtc_nid_table[];
extern const int cellRtc_nid_table_size;

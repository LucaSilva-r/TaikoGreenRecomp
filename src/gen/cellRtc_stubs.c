/* Auto-generated HLE stubs for cellRtc -- do not edit by hand. */

#include "cellRtc_stubs.h"
#include <stdio.h>

/* NID 0x269A1882 */
int32_t nid_0x269A1882_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellRtc::nid_0x269A1882\n");
    return CELL_OK;
}

/* NID 0x2F010BFA */
int32_t cellRtcTickAddMinutes_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellRtc::cellRtcTickAddMinutes\n");
    return CELL_OK;
}

/* NID 0x332A74DD */
int32_t cellRtcTickAddYears_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellRtc::cellRtcTickAddYears\n");
    return CELL_OK;
}

/* NID 0x75744E2A */
int32_t cellRtcTickAddDays_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellRtc::cellRtcTickAddDays\n");
    return CELL_OK;
}

/* NID 0x99B13034 */
int32_t cellRtcSetTick_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellRtc::cellRtcSetTick\n");
    return CELL_OK;
}

/* NID 0x9DAFC0D9 */
int32_t cellRtcGetCurrentTick_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellRtc::cellRtcGetCurrentTick\n");
    return CELL_OK;
}

/* NID 0xBB543189 */
int32_t cellRtcSetTime_t_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellRtc::cellRtcSetTime_t\n");
    return CELL_OK;
}

/* NID 0xC2D8CF95 */
int32_t cellRtcGetDayOfWeek_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellRtc::cellRtcGetDayOfWeek\n");
    return CELL_OK;
}

/* NID 0xC48D5002 */
int32_t cellRtcConvertUtcToLocalTime_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellRtc::cellRtcConvertUtcToLocalTime\n");
    return CELL_OK;
}

/* NID 0xC7BDB7EB */
int32_t cellRtcGetTick_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellRtc::cellRtcGetTick\n");
    return CELL_OK;
}

/* NID 0xCB90C761 */
int32_t cellRtcGetTime_t_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellRtc::cellRtcGetTime_t\n");
    return CELL_OK;
}

/* NID 0xCCCE71BD */
int32_t cellRtcTickAddSeconds_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellRtc::cellRtcTickAddSeconds\n");
    return CELL_OK;
}

/* NID 0xD41D3BD2 */
int32_t cellRtcTickAddHours_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellRtc::cellRtcTickAddHours\n");
    return CELL_OK;
}

/* NID 0xE0ECBB45 */
int32_t cellRtcTickAddMonths_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellRtc::cellRtcTickAddMonths\n");
    return CELL_OK;
}


/* NID table */
const nid_entry_cellRtc cellRtc_nid_table[] = {
    { 0x269A1882u, (void*)nid_0x269A1882_stub, "nid_0x269A1882" },
    { 0x2F010BFAu, (void*)cellRtcTickAddMinutes_stub, "cellRtcTickAddMinutes" },
    { 0x332A74DDu, (void*)cellRtcTickAddYears_stub, "cellRtcTickAddYears" },
    { 0x75744E2Au, (void*)cellRtcTickAddDays_stub, "cellRtcTickAddDays" },
    { 0x99B13034u, (void*)cellRtcSetTick_stub, "cellRtcSetTick" },
    { 0x9DAFC0D9u, (void*)cellRtcGetCurrentTick_stub, "cellRtcGetCurrentTick" },
    { 0xBB543189u, (void*)cellRtcSetTime_t_stub, "cellRtcSetTime_t" },
    { 0xC2D8CF95u, (void*)cellRtcGetDayOfWeek_stub, "cellRtcGetDayOfWeek" },
    { 0xC48D5002u, (void*)cellRtcConvertUtcToLocalTime_stub, "cellRtcConvertUtcToLocalTime" },
    { 0xC7BDB7EBu, (void*)cellRtcGetTick_stub, "cellRtcGetTick" },
    { 0xCB90C761u, (void*)cellRtcGetTime_t_stub, "cellRtcGetTime_t" },
    { 0xCCCE71BDu, (void*)cellRtcTickAddSeconds_stub, "cellRtcTickAddSeconds" },
    { 0xD41D3BD2u, (void*)cellRtcTickAddHours_stub, "cellRtcTickAddHours" },
    { 0xE0ECBB45u, (void*)cellRtcTickAddMonths_stub, "cellRtcTickAddMonths" },
};
const int cellRtc_nid_table_size = sizeof(cellRtc_nid_table) / sizeof(cellRtc_nid_table[0]);

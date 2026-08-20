/* Auto-generated HLE stubs for cellResc -- do not edit by hand. */

#include "cellResc_stubs.h"
#include <stdio.h>

/* NID 0x10DB5B1A */
int32_t cellRescSetDsts_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellResc::cellRescSetDsts\n");
    return CELL_OK;
}

/* NID 0x22AE06D8 */
int32_t cellRescAdjustAspectRatio_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellResc::cellRescAdjustAspectRatio\n");
    return CELL_OK;
}

/* NID 0x23134710 */
int32_t cellRescSetDisplayMode_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellResc::cellRescSetDisplayMode\n");
    return CELL_OK;
}

/* NID 0x25C107E6 */
int32_t cellRescSetConvertAndFlip_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellResc::cellRescSetConvertAndFlip\n");
    return CELL_OK;
}

/* NID 0x2EA3061E */
int32_t cellRescExit_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellResc::cellRescExit\n");
    return CELL_OK;
}

/* NID 0x2EA94661 */
int32_t cellRescSetFlipHandler_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellResc::cellRescSetFlipHandler\n");
    return CELL_OK;
}

/* NID 0x516EE89E */
int32_t cellRescInit_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellResc::cellRescInit\n");
    return CELL_OK;
}

/* NID 0x5A338CDB */
int32_t cellRescGetBufferSize_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellResc::cellRescGetBufferSize\n");
    return CELL_OK;
}

/* NID 0x6CD0F95F */
int32_t cellRescSetSrc_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellResc::cellRescSetSrc\n");
    return CELL_OK;
}

/* NID 0x8107277C */
int32_t cellRescSetBufferAddress_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellResc::cellRescSetBufferAddress\n");
    return CELL_OK;
}

/* NID 0xD1CA0503 */
int32_t cellRescVideoOutResolutionId2RescBufferMode_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellResc::cellRescVideoOutResolutionId2RescBufferMode\n");
    return CELL_OK;
}

/* NID 0xD3758645 */
int32_t cellRescSetVBlankHandler_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellResc::cellRescSetVBlankHandler\n");
    return CELL_OK;
}

/* NID 0xE0CEF79E */
int32_t cellRescCreateInterlaceTable_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellResc::cellRescCreateInterlaceTable\n");
    return CELL_OK;
}


/* NID table */
const nid_entry_cellResc cellResc_nid_table[] = {
    { 0x10DB5B1Au, (void*)cellRescSetDsts_stub, "cellRescSetDsts" },
    { 0x22AE06D8u, (void*)cellRescAdjustAspectRatio_stub, "cellRescAdjustAspectRatio" },
    { 0x23134710u, (void*)cellRescSetDisplayMode_stub, "cellRescSetDisplayMode" },
    { 0x25C107E6u, (void*)cellRescSetConvertAndFlip_stub, "cellRescSetConvertAndFlip" },
    { 0x2EA3061Eu, (void*)cellRescExit_stub, "cellRescExit" },
    { 0x2EA94661u, (void*)cellRescSetFlipHandler_stub, "cellRescSetFlipHandler" },
    { 0x516EE89Eu, (void*)cellRescInit_stub, "cellRescInit" },
    { 0x5A338CDBu, (void*)cellRescGetBufferSize_stub, "cellRescGetBufferSize" },
    { 0x6CD0F95Fu, (void*)cellRescSetSrc_stub, "cellRescSetSrc" },
    { 0x8107277Cu, (void*)cellRescSetBufferAddress_stub, "cellRescSetBufferAddress" },
    { 0xD1CA0503u, (void*)cellRescVideoOutResolutionId2RescBufferMode_stub, "cellRescVideoOutResolutionId2RescBufferMode" },
    { 0xD3758645u, (void*)cellRescSetVBlankHandler_stub, "cellRescSetVBlankHandler" },
    { 0xE0CEF79Eu, (void*)cellRescCreateInterlaceTable_stub, "cellRescCreateInterlaceTable" },
};
const int cellResc_nid_table_size = sizeof(cellResc_nid_table) / sizeof(cellResc_nid_table[0]);

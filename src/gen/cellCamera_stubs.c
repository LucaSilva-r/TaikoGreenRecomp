/* Auto-generated HLE stubs for cellCamera -- do not edit by hand. */

#include "cellCamera_stubs.h"
#include <stdio.h>

/* NID 0x02F5CED0 */
int32_t cellCameraStop_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellCamera::cellCameraStop\n");
    return CELL_OK;
}

/* NID 0x0E63C444 */
int32_t cellCameraGetBufferInfoEx_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellCamera::cellCameraGetBufferInfoEx\n");
    return CELL_OK;
}

/* NID 0x379C5DD6 */
int32_t cellCameraClose_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellCamera::cellCameraClose\n");
    return CELL_OK;
}

/* NID 0x3845D39B */
int32_t cellCameraRead_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellCamera::cellCameraRead\n");
    return CELL_OK;
}

/* NID 0x456DC4AA */
int32_t cellCameraStart_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellCamera::cellCameraStart\n");
    return CELL_OK;
}

/* NID 0x532B8AAA */
int32_t cellCameraGetAttribute_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellCamera::cellCameraGetAttribute\n");
    return CELL_OK;
}

/* NID 0x58BC5870 */
int32_t cellCameraGetType_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellCamera::cellCameraGetType\n");
    return CELL_OK;
}

/* NID 0x5AD46570 */
int32_t cellCameraEnd_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellCamera::cellCameraEnd\n");
    return CELL_OK;
}

/* NID 0x5D25F866 */
int32_t cellCameraOpenEx_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellCamera::cellCameraOpenEx\n");
    return CELL_OK;
}

/* NID 0x5EEBF24E */
int32_t cellCameraIsStarted_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellCamera::cellCameraIsStarted\n");
    return CELL_OK;
}

/* NID 0x7E063BBC */
int32_t cellCameraIsAttached_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellCamera::cellCameraIsAttached\n");
    return CELL_OK;
}

/* NID 0x81F83DB9 */
int32_t cellCameraReset_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellCamera::cellCameraReset\n");
    return CELL_OK;
}

/* NID 0x8CD56EEE */
int32_t cellCameraSetAttribute_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellCamera::cellCameraSetAttribute\n");
    return CELL_OK;
}

/* NID 0xBF47C5DD */
int32_t cellCameraInit_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellCamera::cellCameraInit\n");
    return CELL_OK;
}

/* NID 0xFA160F24 */
int32_t cellCameraIsOpen_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellCamera::cellCameraIsOpen\n");
    return CELL_OK;
}


/* NID table */
const nid_entry_cellCamera cellCamera_nid_table[] = {
    { 0x02F5CED0u, (void*)cellCameraStop_stub, "cellCameraStop" },
    { 0x0E63C444u, (void*)cellCameraGetBufferInfoEx_stub, "cellCameraGetBufferInfoEx" },
    { 0x379C5DD6u, (void*)cellCameraClose_stub, "cellCameraClose" },
    { 0x3845D39Bu, (void*)cellCameraRead_stub, "cellCameraRead" },
    { 0x456DC4AAu, (void*)cellCameraStart_stub, "cellCameraStart" },
    { 0x532B8AAAu, (void*)cellCameraGetAttribute_stub, "cellCameraGetAttribute" },
    { 0x58BC5870u, (void*)cellCameraGetType_stub, "cellCameraGetType" },
    { 0x5AD46570u, (void*)cellCameraEnd_stub, "cellCameraEnd" },
    { 0x5D25F866u, (void*)cellCameraOpenEx_stub, "cellCameraOpenEx" },
    { 0x5EEBF24Eu, (void*)cellCameraIsStarted_stub, "cellCameraIsStarted" },
    { 0x7E063BBCu, (void*)cellCameraIsAttached_stub, "cellCameraIsAttached" },
    { 0x81F83DB9u, (void*)cellCameraReset_stub, "cellCameraReset" },
    { 0x8CD56EEEu, (void*)cellCameraSetAttribute_stub, "cellCameraSetAttribute" },
    { 0xBF47C5DDu, (void*)cellCameraInit_stub, "cellCameraInit" },
    { 0xFA160F24u, (void*)cellCameraIsOpen_stub, "cellCameraIsOpen" },
};
const int cellCamera_nid_table_size = sizeof(cellCamera_nid_table) / sizeof(cellCamera_nid_table[0]);

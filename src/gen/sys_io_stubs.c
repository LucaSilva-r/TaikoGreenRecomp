/* Auto-generated HLE stubs for sys_io -- do not edit by hand. */

#include "sys_io_stubs.h"
#include <stdio.h>

/* NID 0x1CF98800 */
int32_t cellPadInit_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::cellPadInit\n");
    return CELL_OK;
}

/* NID 0x1F71ECBE */
int32_t nid_0x1F71ECBE_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::nid_0x1F71ECBE\n");
    return CELL_OK;
}

/* NID 0x2073B7F6 */
int32_t cellKbClearBuf_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::cellKbClearBuf\n");
    return CELL_OK;
}

/* NID 0x2F1774D5 */
int32_t cellKbGetInfo_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::cellKbGetInfo\n");
    return CELL_OK;
}

/* NID 0x3138E632 */
int32_t cellMouseGetData_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::cellMouseGetData\n");
    return CELL_OK;
}

/* NID 0x3F72C56E */
int32_t cellKbSetLEDStatus_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::cellKbSetLEDStatus\n");
    return CELL_OK;
}

/* NID 0x433F6EC0 */
int32_t cellKbInit_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::cellKbInit\n");
    return CELL_OK;
}

/* NID 0x4AB1FA77 */
int32_t cellKbCnvRawCode_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::cellKbCnvRawCode\n");
    return CELL_OK;
}

/* NID 0x4D9B75D5 */
int32_t cellPadEnd_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::cellPadEnd\n");
    return CELL_OK;
}

/* NID 0x578E3C98 */
int32_t cellPadSetPortSetting_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::cellPadSetPortSetting\n");
    return CELL_OK;
}

/* NID 0x5BAF30FB */
int32_t cellMouseGetInfo_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::cellMouseGetInfo\n");
    return CELL_OK;
}

/* NID 0x8B72CDA1 */
int32_t cellPadGetData_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::cellPadGetData\n");
    return CELL_OK;
}

/* NID 0xA5F85E4D */
int32_t cellKbSetCodeType_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::cellKbSetCodeType\n");
    return CELL_OK;
}

/* NID 0xA703A51D */
int32_t cellPadGetInfo2_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::cellPadGetInfo2\n");
    return CELL_OK;
}

/* NID 0xBFCE3285 */
int32_t cellKbEnd_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::cellKbEnd\n");
    return CELL_OK;
}

/* NID 0xC9030138 */
int32_t cellMouseInit_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::cellMouseInit\n");
    return CELL_OK;
}

/* NID 0xDEEFDFA7 */
int32_t cellKbSetReadMode_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::cellKbSetReadMode\n");
    return CELL_OK;
}

/* NID 0xE10183CE */
int32_t cellMouseEnd_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::cellMouseEnd\n");
    return CELL_OK;
}

/* NID 0xF65544EE */
int32_t cellPadSetActDirect_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::cellPadSetActDirect\n");
    return CELL_OK;
}

/* NID 0xFF0A21B7 */
int32_t cellKbRead_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_io::cellKbRead\n");
    return CELL_OK;
}


/* NID table */
const nid_entry_sys_io sys_io_nid_table[] = {
    { 0x1CF98800u, (void*)cellPadInit_stub, "cellPadInit" },
    { 0x1F71ECBEu, (void*)nid_0x1F71ECBE_stub, "nid_0x1F71ECBE" },
    { 0x2073B7F6u, (void*)cellKbClearBuf_stub, "cellKbClearBuf" },
    { 0x2F1774D5u, (void*)cellKbGetInfo_stub, "cellKbGetInfo" },
    { 0x3138E632u, (void*)cellMouseGetData_stub, "cellMouseGetData" },
    { 0x3F72C56Eu, (void*)cellKbSetLEDStatus_stub, "cellKbSetLEDStatus" },
    { 0x433F6EC0u, (void*)cellKbInit_stub, "cellKbInit" },
    { 0x4AB1FA77u, (void*)cellKbCnvRawCode_stub, "cellKbCnvRawCode" },
    { 0x4D9B75D5u, (void*)cellPadEnd_stub, "cellPadEnd" },
    { 0x578E3C98u, (void*)cellPadSetPortSetting_stub, "cellPadSetPortSetting" },
    { 0x5BAF30FBu, (void*)cellMouseGetInfo_stub, "cellMouseGetInfo" },
    { 0x8B72CDA1u, (void*)cellPadGetData_stub, "cellPadGetData" },
    { 0xA5F85E4Du, (void*)cellKbSetCodeType_stub, "cellKbSetCodeType" },
    { 0xA703A51Du, (void*)cellPadGetInfo2_stub, "cellPadGetInfo2" },
    { 0xBFCE3285u, (void*)cellKbEnd_stub, "cellKbEnd" },
    { 0xC9030138u, (void*)cellMouseInit_stub, "cellMouseInit" },
    { 0xDEEFDFA7u, (void*)cellKbSetReadMode_stub, "cellKbSetReadMode" },
    { 0xE10183CEu, (void*)cellMouseEnd_stub, "cellMouseEnd" },
    { 0xF65544EEu, (void*)cellPadSetActDirect_stub, "cellPadSetActDirect" },
    { 0xFF0A21B7u, (void*)cellKbRead_stub, "cellKbRead" },
};
const int sys_io_nid_table_size = sizeof(sys_io_nid_table) / sizeof(sys_io_nid_table[0]);

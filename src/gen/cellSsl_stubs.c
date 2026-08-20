/* Auto-generated HLE stubs for cellSsl -- do not edit by hand. */

#include "cellSsl_stubs.h"
#include <stdio.h>

/* NID 0x218B64DA */
int32_t cellSslCertGetNotAfter_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellSsl::cellSslCertGetNotAfter\n");
    return CELL_OK;
}

/* NID 0x31D9BA8D */
int32_t cellSslCertGetNotBefore_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellSsl::cellSslCertGetNotBefore\n");
    return CELL_OK;
}

/* NID 0xFB02C9D2 */
int32_t cellSslInit_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellSsl::cellSslInit\n");
    return CELL_OK;
}


/* NID table */
const nid_entry_cellSsl cellSsl_nid_table[] = {
    { 0x218B64DAu, (void*)cellSslCertGetNotAfter_stub, "cellSslCertGetNotAfter" },
    { 0x31D9BA8Du, (void*)cellSslCertGetNotBefore_stub, "cellSslCertGetNotBefore" },
    { 0xFB02C9D2u, (void*)cellSslInit_stub, "cellSslInit" },
};
const int cellSsl_nid_table_size = sizeof(cellSsl_nid_table) / sizeof(cellSsl_nid_table[0]);

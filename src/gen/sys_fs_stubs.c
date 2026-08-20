/* Auto-generated HLE stubs for sys_fs -- do not edit by hand. */

#include "sys_fs_stubs.h"
#include <stdio.h>

/* NID 0x1EA02E2F */
int32_t nid_0x1EA02E2F_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_fs::nid_0x1EA02E2F\n");
    return CELL_OK;
}

/* NID 0x2CB51F0D */
int32_t cellFsClose_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_fs::cellFsClose\n");
    return CELL_OK;
}

/* NID 0x3F61245C */
int32_t cellFsOpendir_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_fs::cellFsOpendir\n");
    return CELL_OK;
}

/* NID 0x4CEF342E */
int32_t cellFsAioWrite_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_fs::cellFsAioWrite\n");
    return CELL_OK;
}

/* NID 0x4D5FF8E2 */
int32_t cellFsRead_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_fs::cellFsRead\n");
    return CELL_OK;
}

/* NID 0x5C74903D */
int32_t cellFsReaddir_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_fs::cellFsReaddir\n");
    return CELL_OK;
}

/* NID 0x718BF5F8 */
int32_t cellFsOpen_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_fs::cellFsOpen\n");
    return CELL_OK;
}

/* NID 0x7DE6DCED */
int32_t cellFsStat_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_fs::cellFsStat\n");
    return CELL_OK;
}

/* NID 0x7F4677A8 */
int32_t cellFsUnlink_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_fs::cellFsUnlink\n");
    return CELL_OK;
}

/* NID 0x9F951810 */
int32_t cellFsAioFinish_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_fs::cellFsAioFinish\n");
    return CELL_OK;
}

/* NID 0xA397D042 */
int32_t cellFsLseek_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_fs::cellFsLseek\n");
    return CELL_OK;
}

/* NID 0xBA901FE6 */
int32_t cellFsMkdir_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_fs::cellFsMkdir\n");
    return CELL_OK;
}

/* NID 0xC1C507E7 */
int32_t cellFsAioRead_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_fs::cellFsAioRead\n");
    return CELL_OK;
}

/* NID 0xDB869F20 */
int32_t cellFsAioInit_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_fs::cellFsAioInit\n");
    return CELL_OK;
}

/* NID 0xECDCF2AB */
int32_t cellFsWrite_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_fs::cellFsWrite\n");
    return CELL_OK;
}

/* NID 0xEF3EFA34 */
int32_t cellFsFstat_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_fs::cellFsFstat\n");
    return CELL_OK;
}

/* NID 0xF12EECC8 */
int32_t cellFsRename_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_fs::cellFsRename\n");
    return CELL_OK;
}

/* NID 0xFF42DCC3 */
int32_t cellFsClosedir_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sys_fs::cellFsClosedir\n");
    return CELL_OK;
}


/* NID table */
const nid_entry_sys_fs sys_fs_nid_table[] = {
    { 0x1EA02E2Fu, (void*)nid_0x1EA02E2F_stub, "nid_0x1EA02E2F" },
    { 0x2CB51F0Du, (void*)cellFsClose_stub, "cellFsClose" },
    { 0x3F61245Cu, (void*)cellFsOpendir_stub, "cellFsOpendir" },
    { 0x4CEF342Eu, (void*)cellFsAioWrite_stub, "cellFsAioWrite" },
    { 0x4D5FF8E2u, (void*)cellFsRead_stub, "cellFsRead" },
    { 0x5C74903Du, (void*)cellFsReaddir_stub, "cellFsReaddir" },
    { 0x718BF5F8u, (void*)cellFsOpen_stub, "cellFsOpen" },
    { 0x7DE6DCEDu, (void*)cellFsStat_stub, "cellFsStat" },
    { 0x7F4677A8u, (void*)cellFsUnlink_stub, "cellFsUnlink" },
    { 0x9F951810u, (void*)cellFsAioFinish_stub, "cellFsAioFinish" },
    { 0xA397D042u, (void*)cellFsLseek_stub, "cellFsLseek" },
    { 0xBA901FE6u, (void*)cellFsMkdir_stub, "cellFsMkdir" },
    { 0xC1C507E7u, (void*)cellFsAioRead_stub, "cellFsAioRead" },
    { 0xDB869F20u, (void*)cellFsAioInit_stub, "cellFsAioInit" },
    { 0xECDCF2ABu, (void*)cellFsWrite_stub, "cellFsWrite" },
    { 0xEF3EFA34u, (void*)cellFsFstat_stub, "cellFsFstat" },
    { 0xF12EECC8u, (void*)cellFsRename_stub, "cellFsRename" },
    { 0xFF42DCC3u, (void*)cellFsClosedir_stub, "cellFsClosedir" },
};
const int sys_fs_nid_table_size = sizeof(sys_fs_nid_table) / sizeof(sys_fs_nid_table[0]);

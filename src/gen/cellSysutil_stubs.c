/* Auto-generated HLE stubs for cellSysutil -- do not edit by hand. */

#include "cellSysutil_stubs.h"
#include <stdio.h>

/* NID 0x0BAE8772 */
int32_t cellVideoOutConfigure_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellSysutil::cellVideoOutConfigure\n");
    return CELL_OK;
}

/* NID 0x189A74DA */
int32_t cellSysutilCheckCallback_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellSysutil::cellSysutilCheckCallback\n");
    return CELL_OK;
}

/* NID 0x1E7BFF94 */
int32_t cellSysCacheMount_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellSysutil::cellSysCacheMount\n");
    return CELL_OK;
}

/* NID 0x220894E3 */
int32_t cellSysutilEnableBgmPlayback_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellSysutil::cellSysutilEnableBgmPlayback\n");
    return CELL_OK;
}

/* NID 0x744C1544 */
int32_t cellSysCacheClear_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellSysutil::cellSysCacheClear\n");
    return CELL_OK;
}

/* NID 0x75BBB672 */
int32_t cellVideoOutGetNumberOfDevice_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellSysutil::cellVideoOutGetNumberOfDevice\n");
    return CELL_OK;
}

/* NID 0x887572D5 */
int32_t cellVideoOutGetState_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellSysutil::cellVideoOutGetState\n");
    return CELL_OK;
}

/* NID 0x9D98AFA0 */
int32_t cellSysutilRegisterCallback_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellSysutil::cellSysutilRegisterCallback\n");
    return CELL_OK;
}

/* NID 0xA11552F6 */
int32_t cellSysutilGetBgmPlaybackStatus_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellSysutil::cellSysutilGetBgmPlaybackStatus\n");
    return CELL_OK;
}

/* NID 0xA322DB75 */
int32_t cellVideoOutGetResolutionAvailability_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellSysutil::cellVideoOutGetResolutionAvailability\n");
    return CELL_OK;
}

/* NID 0xCFDD8E87 */
int32_t cellSysutilDisableBgmPlayback_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellSysutil::cellSysutilDisableBgmPlayback\n");
    return CELL_OK;
}

/* NID 0xE558748D */
int32_t cellVideoOutGetResolution_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellSysutil::cellVideoOutGetResolution\n");
    return CELL_OK;
}

/* NID 0xED5D96AF */
int32_t cellAudioOutGetConfiguration_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellSysutil::cellAudioOutGetConfiguration\n");
    return CELL_OK;
}


/* NID table */
const nid_entry_cellSysutil cellSysutil_nid_table[] = {
    { 0x0BAE8772u, (void*)cellVideoOutConfigure_stub, "cellVideoOutConfigure" },
    { 0x189A74DAu, (void*)cellSysutilCheckCallback_stub, "cellSysutilCheckCallback" },
    { 0x1E7BFF94u, (void*)cellSysCacheMount_stub, "cellSysCacheMount" },
    { 0x220894E3u, (void*)cellSysutilEnableBgmPlayback_stub, "cellSysutilEnableBgmPlayback" },
    { 0x744C1544u, (void*)cellSysCacheClear_stub, "cellSysCacheClear" },
    { 0x75BBB672u, (void*)cellVideoOutGetNumberOfDevice_stub, "cellVideoOutGetNumberOfDevice" },
    { 0x887572D5u, (void*)cellVideoOutGetState_stub, "cellVideoOutGetState" },
    { 0x9D98AFA0u, (void*)cellSysutilRegisterCallback_stub, "cellSysutilRegisterCallback" },
    { 0xA11552F6u, (void*)cellSysutilGetBgmPlaybackStatus_stub, "cellSysutilGetBgmPlaybackStatus" },
    { 0xA322DB75u, (void*)cellVideoOutGetResolutionAvailability_stub, "cellVideoOutGetResolutionAvailability" },
    { 0xCFDD8E87u, (void*)cellSysutilDisableBgmPlayback_stub, "cellSysutilDisableBgmPlayback" },
    { 0xE558748Du, (void*)cellVideoOutGetResolution_stub, "cellVideoOutGetResolution" },
    { 0xED5D96AFu, (void*)cellAudioOutGetConfiguration_stub, "cellAudioOutGetConfiguration" },
};
const int cellSysutil_nid_table_size = sizeof(cellSysutil_nid_table) / sizeof(cellSysutil_nid_table[0]);

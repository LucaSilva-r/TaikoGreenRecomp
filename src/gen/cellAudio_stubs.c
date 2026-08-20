/* Auto-generated HLE stubs for cellAudio -- do not edit by hand. */

#include "cellAudio_stubs.h"
#include <stdio.h>

/* NID 0x0B168F92 */
int32_t cellAudioInit_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellAudio::cellAudioInit\n");
    return CELL_OK;
}

/* NID 0x377E0CD9 */
int32_t cellAudioSetNotifyEventQueue_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellAudio::cellAudioSetNotifyEventQueue\n");
    return CELL_OK;
}

/* NID 0x4109D08C */
int32_t cellAudioGetPortTimestamp_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellAudio::cellAudioGetPortTimestamp\n");
    return CELL_OK;
}

/* NID 0x4129FE2D */
int32_t cellAudioPortClose_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellAudio::cellAudioPortClose\n");
    return CELL_OK;
}

/* NID 0x56DFE179 */
int32_t cellAudioSetPortLevel_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellAudio::cellAudioSetPortLevel\n");
    return CELL_OK;
}

/* NID 0x5B1E2C73 */
int32_t cellAudioPortStop_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellAudio::cellAudioPortStop\n");
    return CELL_OK;
}

/* NID 0x74A66AF0 */
int32_t cellAudioGetPortConfig_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellAudio::cellAudioGetPortConfig\n");
    return CELL_OK;
}

/* NID 0x832DF17E */
int32_t nid_0x832DF17E_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellAudio::nid_0x832DF17E\n");
    return CELL_OK;
}

/* NID 0x89BE28F2 */
int32_t cellAudioPortStart_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellAudio::cellAudioPortStart\n");
    return CELL_OK;
}

/* NID 0x9E4B1DB8 */
int32_t nid_0x9E4B1DB8_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellAudio::nid_0x9E4B1DB8\n");
    return CELL_OK;
}

/* NID 0xCA5AC370 */
int32_t cellAudioQuit_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellAudio::cellAudioQuit\n");
    return CELL_OK;
}

/* NID 0xCD7BC431 */
int32_t cellAudioPortOpen_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellAudio::cellAudioPortOpen\n");
    return CELL_OK;
}

/* NID 0xDAB029AA */
int32_t nid_0xDAB029AA_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellAudio::nid_0xDAB029AA\n");
    return CELL_OK;
}

/* NID 0xE4046AFE */
int32_t cellAudioGetPortBlockTag_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellAudio::cellAudioGetPortBlockTag\n");
    return CELL_OK;
}

/* NID 0xFF3626FD */
int32_t cellAudioRemoveNotifyEventQueue_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellAudio::cellAudioRemoveNotifyEventQueue\n");
    return CELL_OK;
}


/* NID table */
const nid_entry_cellAudio cellAudio_nid_table[] = {
    { 0x0B168F92u, (void*)cellAudioInit_stub, "cellAudioInit" },
    { 0x377E0CD9u, (void*)cellAudioSetNotifyEventQueue_stub, "cellAudioSetNotifyEventQueue" },
    { 0x4109D08Cu, (void*)cellAudioGetPortTimestamp_stub, "cellAudioGetPortTimestamp" },
    { 0x4129FE2Du, (void*)cellAudioPortClose_stub, "cellAudioPortClose" },
    { 0x56DFE179u, (void*)cellAudioSetPortLevel_stub, "cellAudioSetPortLevel" },
    { 0x5B1E2C73u, (void*)cellAudioPortStop_stub, "cellAudioPortStop" },
    { 0x74A66AF0u, (void*)cellAudioGetPortConfig_stub, "cellAudioGetPortConfig" },
    { 0x832DF17Eu, (void*)nid_0x832DF17E_stub, "nid_0x832DF17E" },
    { 0x89BE28F2u, (void*)cellAudioPortStart_stub, "cellAudioPortStart" },
    { 0x9E4B1DB8u, (void*)nid_0x9E4B1DB8_stub, "nid_0x9E4B1DB8" },
    { 0xCA5AC370u, (void*)cellAudioQuit_stub, "cellAudioQuit" },
    { 0xCD7BC431u, (void*)cellAudioPortOpen_stub, "cellAudioPortOpen" },
    { 0xDAB029AAu, (void*)nid_0xDAB029AA_stub, "nid_0xDAB029AA" },
    { 0xE4046AFEu, (void*)cellAudioGetPortBlockTag_stub, "cellAudioGetPortBlockTag" },
    { 0xFF3626FDu, (void*)cellAudioRemoveNotifyEventQueue_stub, "cellAudioRemoveNotifyEventQueue" },
};
const int cellAudio_nid_table_size = sizeof(cellAudio_nid_table) / sizeof(cellAudio_nid_table[0]);

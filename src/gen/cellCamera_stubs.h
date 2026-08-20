/* Auto-generated HLE stubs for cellCamera -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for cellCamera */
/* NID 0x02F5CED0 */ int32_t cellCameraStop_stub(ppu_context* ctx);
/* NID 0x0E63C444 */ int32_t cellCameraGetBufferInfoEx_stub(ppu_context* ctx);
/* NID 0x379C5DD6 */ int32_t cellCameraClose_stub(ppu_context* ctx);
/* NID 0x3845D39B */ int32_t cellCameraRead_stub(ppu_context* ctx);
/* NID 0x456DC4AA */ int32_t cellCameraStart_stub(ppu_context* ctx);
/* NID 0x532B8AAA */ int32_t cellCameraGetAttribute_stub(ppu_context* ctx);
/* NID 0x58BC5870 */ int32_t cellCameraGetType_stub(ppu_context* ctx);
/* NID 0x5AD46570 */ int32_t cellCameraEnd_stub(ppu_context* ctx);
/* NID 0x5D25F866 */ int32_t cellCameraOpenEx_stub(ppu_context* ctx);
/* NID 0x5EEBF24E */ int32_t cellCameraIsStarted_stub(ppu_context* ctx);
/* NID 0x7E063BBC */ int32_t cellCameraIsAttached_stub(ppu_context* ctx);
/* NID 0x81F83DB9 */ int32_t cellCameraReset_stub(ppu_context* ctx);
/* NID 0x8CD56EEE */ int32_t cellCameraSetAttribute_stub(ppu_context* ctx);
/* NID 0xBF47C5DD */ int32_t cellCameraInit_stub(ppu_context* ctx);
/* NID 0xFA160F24 */ int32_t cellCameraIsOpen_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_cellCamera;
extern const nid_entry_cellCamera cellCamera_nid_table[];
extern const int cellCamera_nid_table_size;

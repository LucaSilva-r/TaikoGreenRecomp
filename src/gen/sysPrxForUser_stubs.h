/* Auto-generated HLE stubs for sysPrxForUser -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for sysPrxForUser */
/* NID 0x1573DC3F */ int32_t sys_lwmutex_lock_stub(ppu_context* ctx);
/* NID 0x1BC200F4 */ int32_t sys_lwmutex_unlock_stub(ppu_context* ctx);
/* NID 0x24A1EA07 */ int32_t sys_ppu_thread_create_stub(ppu_context* ctx);
/* NID 0x26090058 */ int32_t sys_prx_load_module_stub(ppu_context* ctx);
/* NID 0x2C847572 */ int32_t _sys_process_atexitspawn_stub(ppu_context* ctx);
/* NID 0x2F85C0EF */ int32_t sys_lwmutex_create_stub(ppu_context* ctx);
/* NID 0x350D454E */ int32_t sys_ppu_thread_get_id_stub(ppu_context* ctx);
/* NID 0x42B23552 */ int32_t sys_prx_register_library_stub(ppu_context* ctx);
/* NID 0x45FE2FCE */ int32_t _sys_spu_printf_initialize_stub(ppu_context* ctx);
/* NID 0x620E35A7 */ int32_t nid_0x620E35A7_stub(ppu_context* ctx);
/* NID 0x67F9FEDB */ int32_t sys_game_process_exitspawn2_stub(ppu_context* ctx);
/* NID 0x6E05231D */ int32_t nid_0x6E05231D_stub(ppu_context* ctx);
/* NID 0x744680A2 */ int32_t sys_initialize_tls_stub(ppu_context* ctx);
/* NID 0x80FB0C19 */ int32_t sys_prx_stop_module_stub(ppu_context* ctx);
/* NID 0x8461E528 */ int32_t sys_time_get_system_time_stub(ppu_context* ctx);
/* NID 0x96328741 */ int32_t _sys_process_at_Exitspawn_stub(ppu_context* ctx);
/* NID 0x9E0623B5 */ int32_t nid_0x9E0623B5_stub(ppu_context* ctx);
/* NID 0x9F18429D */ int32_t sys_prx_start_module_stub(ppu_context* ctx);
/* NID 0x9F950780 */ int32_t nid_0x9F950780_stub(ppu_context* ctx);
/* NID 0xA2C7BA64 */ int32_t sys_prx_exitspawn_with_level_stub(ppu_context* ctx);
/* NID 0xA3E3BE68 */ int32_t sys_ppu_thread_once_stub(ppu_context* ctx);
/* NID 0xACAD8FB6 */ int32_t nid_0xACAD8FB6_stub(ppu_context* ctx);
/* NID 0xAEB78725 */ int32_t sys_lwmutex_trylock_stub(ppu_context* ctx);
/* NID 0xAFF080A4 */ int32_t sys_ppu_thread_exit_stub(ppu_context* ctx);
/* NID 0xC3476D0C */ int32_t sys_lwmutex_destroy_stub(ppu_context* ctx);
/* NID 0xDD3B27AC */ int32_t _sys_spu_printf_finalize_stub(ppu_context* ctx);
/* NID 0xE0DA8EFD */ int32_t sys_spu_image_close_stub(ppu_context* ctx);
/* NID 0xE6F2C1E7 */ int32_t sys_process_exit_stub(ppu_context* ctx);
/* NID 0xE76964F5 */ int32_t nid_0xE76964F5_stub(ppu_context* ctx);
/* NID 0xEBE5F72F */ int32_t sys_spu_image_import_stub(ppu_context* ctx);
/* NID 0xF0AECE0D */ int32_t sys_prx_unload_module_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_sysPrxForUser;
extern const nid_entry_sysPrxForUser sysPrxForUser_nid_table[];
extern const int sysPrxForUser_nid_table_size;

/* Taiko's bnusCore audio mixer is a raw SPU thread, not a SPURS workload.
 * Register its lifted entry with the lv2 raw-thread bridge so group_start does
 * not silently complete it. Registration must be unconditional: the
 * standalone harness establishes its default environment in main(), after C
 * constructors have already run. TAIKO_AUDIO_SPU=0 is checked when the group
 * actually starts and remains available for silent/headless diagnosis. */
#include "ps3emu/spu_fallback.h"
#include "spu_context.h"

#include <stdint.h>
#include <stdlib.h>

extern void spu_0004_at_00F3E780_spu_func_000000D0(spu_context*);
extern int32_t spu_thread_run_lifted_raw(uint32_t tid,
                                         void (*entry)(spu_context*),
                                         int image_id);

static int32_t taiko_bnus_spu_fallback(uint32_t tid, uint32_t args_ea,
                                       uint32_t args_size, void* user)
{
    (void)args_ea;
    (void)args_size;
    (void)user;
    const char* enabled = getenv("TAIKO_AUDIO_SPU");
    if (enabled && enabled[0] == '0')
        return 0;
    return spu_thread_run_lifted_raw(
        tid, spu_0004_at_00F3E780_spu_func_000000D0, 5);
}

__attribute__((constructor)) static void taiko_register_audio_spu(void)
{
    spu_register_ppu_fallback(0x000000D0u,
                              taiko_bnus_spu_fallback, NULL);
}

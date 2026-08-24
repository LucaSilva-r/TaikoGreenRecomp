#include "ppu_thread_lifecycle.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    assert(!ppu_thread_flags_joinable(0));
    assert(ppu_thread_flags_joinable(SYS_PPU_THREAD_CREATE_JOINABLE));
    assert(!ppu_thread_flags_joinable(SYS_PPU_THREAD_CREATE_INTERRUPT));

    assert(ppu_thread_state_after_guest_exit(PPU_THREAD_STATE_DETACHED) ==
           PPU_THREAD_STATE_DETACHED);
    assert(ppu_thread_state_after_host_exit(PPU_THREAD_STATE_DETACHED) ==
           PPU_THREAD_STATE_FREE);
    assert(ppu_thread_state_after_guest_exit(PPU_THREAD_STATE_RUNNING) ==
           PPU_THREAD_STATE_RUNNING);
    assert(ppu_thread_state_after_host_exit(PPU_THREAD_STATE_JOINING) ==
           PPU_THREAD_STATE_JOINING);

    /* The attract loop creates thousands of detached CnuSound2 loaders over
     * a kiosk session. Every one must return the descriptor to FREE. */
    int state = PPU_THREAD_STATE_FREE;
    for (unsigned i = 0; i < 10000; ++i) {
        state = ppu_thread_flags_joinable(0)
            ? PPU_THREAD_STATE_RUNNING : PPU_THREAD_STATE_DETACHED;
        state = ppu_thread_state_after_guest_exit(state);
        state = ppu_thread_state_after_host_exit(state);
        assert(state == PPU_THREAD_STATE_FREE);
    }

    puts("ppu thread lifecycle tests passed");
    return 0;
}

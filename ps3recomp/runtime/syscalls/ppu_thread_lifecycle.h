#ifndef PPU_THREAD_LIFECYCLE_H
#define PPU_THREAD_LIFECYCLE_H

#include <stdint.h>

/* LV2 sys_ppu_thread_create flags. */
#define SYS_PPU_THREAD_CREATE_JOINABLE  0x1u
#define SYS_PPU_THREAD_CREATE_INTERRUPT 0x2u

/* Host-side descriptor states. */
#define PPU_THREAD_STATE_FREE       0
#define PPU_THREAD_STATE_RUNNING    1
#define PPU_THREAD_STATE_FINISHED   2
#define PPU_THREAD_STATE_DETACHED   3
#define PPU_THREAD_STATE_JOINING    4

static inline int ppu_thread_flags_joinable(uint64_t flags)
{
    return (flags & SYS_PPU_THREAD_CREATE_JOINABLE) != 0;
}

/* The guest exit syscall runs before the lifted entry has unwound from the
 * host thread. Detached descriptors cannot be reused at that point. */
static inline int ppu_thread_state_after_guest_exit(int state)
{
    return state;
}

/* Only the host entry's final return makes a detached slot reusable. */
static inline int ppu_thread_state_after_host_exit(int state)
{
    if (state == PPU_THREAD_STATE_DETACHED)
        return PPU_THREAD_STATE_FREE;
    if (state == PPU_THREAD_STATE_JOINING)
        return PPU_THREAD_STATE_JOINING;
    return PPU_THREAD_STATE_FINISHED;
}

#endif

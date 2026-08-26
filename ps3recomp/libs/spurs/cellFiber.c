/*
 * ps3recomp - cellFiber HLE implementation
 *
 * Implements PPU fibers using Windows Fibers API or POSIX ucontext.
 * Each CellFiber maps to a native fiber for true cooperative switching.
 */

#include "cellFiber.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif !defined(__ANDROID__)
#include <ucontext.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct FiberSlot {
    int        in_use;
    u32        state;
    CellFiberEntry entry;
    u64        arg;
    u32        stack_size;
    char       name[32];
    u32        priority;
#if defined(_WIN32)
    LPVOID     native_fiber;
#elif !defined(__ANDROID__)
    ucontext_t context;
    u8*        stack;
#endif
} FiberSlot;

static FiberSlot s_fibers[CELL_FIBER_MAX_FIBERS];
static int s_initialized = 0;
static s32 s_current_fiber = -1; /* index into s_fibers, or -1 for scheduler */

#if defined(_WIN32)
static LPVOID s_scheduler_fiber = NULL;
#elif !defined(__ANDROID__)
static ucontext_t s_scheduler_context;
#endif

/* ---------------------------------------------------------------------------
 * Fiber trampoline
 * -----------------------------------------------------------------------*/

#if defined(_WIN32)
static void CALLBACK fiber_trampoline(LPVOID param)
{
    u32 idx = (u32)(uintptr_t)param;
    FiberSlot* f = &s_fibers[idx];
    f->state = CELL_FIBER_STATE_RUNNING;
    f->entry(f->arg);
    f->state = CELL_FIBER_STATE_TERMINATED;

    /* Return to scheduler when fiber function returns */
    s_current_fiber = -1;
    SwitchToFiber(s_scheduler_fiber);
}
#elif !defined(__ANDROID__)
static void fiber_trampoline(int idx_lo, int idx_hi)
{
    u32 idx = ((u32)idx_hi << 16) | (u32)(u16)idx_lo;
    FiberSlot* f = &s_fibers[idx];
    f->state = CELL_FIBER_STATE_RUNNING;
    f->entry(f->arg);
    f->state = CELL_FIBER_STATE_TERMINATED;
    s_current_fiber = -1;
    setcontext(&s_scheduler_context);
}
#endif

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellFiberPpuInitialize(void)
{
    printf("[cellFiber] Initialize()\n");

    if (s_initialized)
        return (s32)CELL_FIBER_ERROR_STAT;

    memset(s_fibers, 0, sizeof(s_fibers));
    s_current_fiber = -1;

#if defined(_WIN32)
    /* Convert the current thread to a fiber so we can switch */
    s_scheduler_fiber = ConvertThreadToFiber(NULL);
    if (!s_scheduler_fiber) {
        /* Already a fiber (e.g., re-init) */
        s_scheduler_fiber = GetCurrentFiber();
    }
#elif !defined(__ANDROID__)
    getcontext(&s_scheduler_context);
#else
    /* Bionic intentionally has no getcontext/makecontext/swapcontext. Green
     * does not import cellFiber in observed boots (its SPURS work is lifted),
     * so retain a synchronous diagnostic fallback instead of making every
     * Android build depend on a second platform context library. */
    printf("[cellFiber] Android synchronous fallback active\n");
#endif

    s_initialized = 1;
    return CELL_OK;
}

s32 cellFiberPpuFinalize(void)
{
    printf("[cellFiber] Finalize()\n");

    if (!s_initialized)
        return (s32)CELL_FIBER_ERROR_STAT;

    for (int i = 0; i < CELL_FIBER_MAX_FIBERS; i++) {
        if (s_fibers[i].in_use) {
#if defined(_WIN32)
            if (s_fibers[i].native_fiber)
                DeleteFiber(s_fibers[i].native_fiber);
#elif !defined(__ANDROID__)
            free(s_fibers[i].stack);
#endif
            s_fibers[i].in_use = 0;
        }
    }

#if defined(_WIN32)
    ConvertFiberToThread();
    s_scheduler_fiber = NULL;
#endif

    s_initialized = 0;
    return CELL_OK;
}

s32 cellFiberPpuCreateFiber(CellFiber* fiber, CellFiberEntry entry,
                              u64 arg, const CellFiberAttribute* attr)
{
    if (!s_initialized)
        return (s32)CELL_FIBER_ERROR_STAT;

    if (!fiber || !entry)
        return (s32)CELL_FIBER_ERROR_NULL_POINTER;

    /* Find a free slot */
    int idx = -1;
    for (int i = 0; i < CELL_FIBER_MAX_FIBERS; i++) {
        if (!s_fibers[i].in_use) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return (s32)CELL_FIBER_ERROR_NOMEM;

    FiberSlot* f = &s_fibers[idx];
    memset(f, 0, sizeof(FiberSlot));
    f->in_use = 1;
    f->entry = entry;
    f->arg = arg;
    f->state = CELL_FIBER_STATE_INITIALIZED;
    f->stack_size = CELL_FIBER_DEFAULT_STACK_SIZE;

    if (attr) {
        if (attr->stackSize > 0)
            f->stack_size = attr->stackSize;
        if (attr->name)
            strncpy(f->name, attr->name, sizeof(f->name) - 1);
        f->priority = attr->priority;
    }

    printf("[cellFiber] CreateFiber(id=%d, name=%s, stack=%u)\n",
           idx, f->name[0] ? f->name : "(unnamed)", f->stack_size);

#ifdef _WIN32
    f->native_fiber = CreateFiber(f->stack_size, fiber_trampoline,
                                   (LPVOID)(uintptr_t)idx);
    if (!f->native_fiber) {
        f->in_use = 0;
        return (s32)CELL_FIBER_ERROR_NOMEM;
    }
#elif !defined(__ANDROID__)
    f->stack = (u8*)malloc(f->stack_size);
    if (!f->stack) {
        f->in_use = 0;
        return (s32)CELL_FIBER_ERROR_NOMEM;
    }
    getcontext(&f->context);
    f->context.uc_stack.ss_sp = f->stack;
    f->context.uc_stack.ss_size = f->stack_size;
    f->context.uc_link = &s_scheduler_context;
    makecontext(&f->context, (void (*)(void))fiber_trampoline, 2,
                (int)(idx & 0xFFFF), (int)(idx >> 16));
#endif

    *fiber = (CellFiber)idx;
    return CELL_OK;
}

s32 cellFiberPpuDeleteFiber(CellFiber fiber)
{
    if (!s_initialized)
        return (s32)CELL_FIBER_ERROR_STAT;

    u32 idx = fiber;
    if (idx >= CELL_FIBER_MAX_FIBERS || !s_fibers[idx].in_use)
        return (s32)CELL_FIBER_ERROR_INVAL;

    if (s_fibers[idx].state == CELL_FIBER_STATE_RUNNING)
        return (s32)CELL_FIBER_ERROR_BUSY;

    printf("[cellFiber] DeleteFiber(id=%u)\n", idx);

#if defined(_WIN32)
    if (s_fibers[idx].native_fiber)
        DeleteFiber(s_fibers[idx].native_fiber);
#elif !defined(__ANDROID__)
    free(s_fibers[idx].stack);
#endif

    s_fibers[idx].in_use = 0;
    return CELL_OK;
}

s32 cellFiberPpuSwitchFiber(CellFiber fiber)
{
    if (!s_initialized)
        return (s32)CELL_FIBER_ERROR_STAT;

    u32 idx = fiber;
    if (idx >= CELL_FIBER_MAX_FIBERS || !s_fibers[idx].in_use)
        return (s32)CELL_FIBER_ERROR_INVAL;

    if (s_fibers[idx].state == CELL_FIBER_STATE_TERMINATED)
        return (s32)CELL_FIBER_ERROR_STAT;

    /* Suspend current fiber if one is running */
    if (s_current_fiber >= 0 && s_current_fiber != (s32)idx)
        s_fibers[s_current_fiber].state = CELL_FIBER_STATE_SUSPENDED;

    s_current_fiber = (s32)idx;
    s_fibers[idx].state = CELL_FIBER_STATE_RUNNING;

#if defined(_WIN32)
    SwitchToFiber(s_fibers[idx].native_fiber);
#elif !defined(__ANDROID__)
    if (s_current_fiber == -1) {
        swapcontext(&s_scheduler_context, &s_fibers[idx].context);
    } else {
        ucontext_t* from = (s_current_fiber >= 0 && s_current_fiber != (s32)idx)
                           ? &s_fibers[s_current_fiber].context
                           : &s_scheduler_context;
        swapcontext(from, &s_fibers[idx].context);
    }
#else
    CellFiberEntry entry = s_fibers[idx].entry;
    u64 arg = s_fibers[idx].arg;
    entry(arg);
    if (s_fibers[idx].state == CELL_FIBER_STATE_RUNNING)
        s_fibers[idx].state = CELL_FIBER_STATE_TERMINATED;
    s_current_fiber = -1;
#endif

    return CELL_OK;
}

s32 cellFiberPpuYieldFiber(void)
{
    if (!s_initialized || s_current_fiber < 0)
        return (s32)CELL_FIBER_ERROR_PERM;

    s_fibers[s_current_fiber].state = CELL_FIBER_STATE_SUSPENDED;
    s32 prev = s_current_fiber;
    s_current_fiber = -1;

#if defined(_WIN32)
    SwitchToFiber(s_scheduler_fiber);
#elif !defined(__ANDROID__)
    swapcontext(&s_fibers[prev].context, &s_scheduler_context);
#else
    printf("[cellFiber] Yield is unavailable in Android synchronous mode\n");
    s_current_fiber = (s32)prev;
    s_fibers[prev].state = CELL_FIBER_STATE_RUNNING;
    return (s32)CELL_FIBER_ERROR_PERM;
#endif
    (void)prev;

    return CELL_OK;
}

s32 cellFiberPpuExitFiber(void)
{
    if (!s_initialized || s_current_fiber < 0)
        return (s32)CELL_FIBER_ERROR_PERM;

    s_fibers[s_current_fiber].state = CELL_FIBER_STATE_TERMINATED;
    s_current_fiber = -1;

#if defined(_WIN32)
    SwitchToFiber(s_scheduler_fiber);
#elif !defined(__ANDROID__)
    setcontext(&s_scheduler_context);
#endif

    return CELL_OK; /* unreachable */
}

s32 cellFiberPpuGetCurrentFiber(CellFiber* fiber)
{
    if (!fiber)
        return (s32)CELL_FIBER_ERROR_NULL_POINTER;

    if (s_current_fiber < 0)
        return (s32)CELL_FIBER_ERROR_PERM;

    *fiber = (CellFiber)s_current_fiber;
    return CELL_OK;
}

s32 cellFiberPpuGetFiberState(CellFiber fiber, u32* state)
{
    u32 idx = fiber;
    if (idx >= CELL_FIBER_MAX_FIBERS || !s_fibers[idx].in_use)
        return (s32)CELL_FIBER_ERROR_INVAL;

    if (!state)
        return (s32)CELL_FIBER_ERROR_NULL_POINTER;

    *state = s_fibers[idx].state;
    return CELL_OK;
}

s32 cellFiberPpuAttributeInitialize(CellFiberAttribute* attr)
{
    if (!attr)
        return (s32)CELL_FIBER_ERROR_NULL_POINTER;

    attr->name = NULL;
    attr->priority = 0;
    attr->stackSize = CELL_FIBER_DEFAULT_STACK_SIZE;
    return CELL_OK;
}

s32 cellFiberPpuSleep(void)
{
    if (!s_initialized || s_current_fiber < 0)
        return (s32)CELL_FIBER_ERROR_PERM;

    printf("[cellFiber] Sleep(fiber=%d)\n", s_current_fiber);
    s_fibers[s_current_fiber].state = CELL_FIBER_STATE_SUSPENDED;
    s32 prev = s_current_fiber;
    s_current_fiber = -1;

#if defined(_WIN32)
    SwitchToFiber(s_scheduler_fiber);
#elif !defined(__ANDROID__)
    swapcontext(&s_fibers[prev].context, &s_scheduler_context);
#else
    printf("[cellFiber] Sleep is unavailable in Android synchronous mode\n");
    s_current_fiber = (s32)prev;
    s_fibers[prev].state = CELL_FIBER_STATE_RUNNING;
    return (s32)CELL_FIBER_ERROR_PERM;
#endif
    (void)prev;

    return CELL_OK;
}

s32 cellFiberPpuWakeup(CellFiber fiber)
{
    u32 idx = fiber;
    if (idx >= CELL_FIBER_MAX_FIBERS || !s_fibers[idx].in_use)
        return (s32)CELL_FIBER_ERROR_INVAL;

    if (s_fibers[idx].state != CELL_FIBER_STATE_SUSPENDED) {
        printf("[cellFiber] Wakeup(fiber=%u): not suspended (state=%u)\n",
               idx, s_fibers[idx].state);
        return (s32)CELL_FIBER_ERROR_STAT;
    }

    printf("[cellFiber] Wakeup(fiber=%u)\n", idx);
    s_fibers[idx].state = CELL_FIBER_STATE_INITIALIZED; /* ready to run */
    return CELL_OK;
}

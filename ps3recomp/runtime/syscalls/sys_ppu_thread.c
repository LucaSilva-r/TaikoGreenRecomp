/*
 * ps3recomp - PPU thread management syscalls (implementation)
 */

#include "sys_ppu_thread.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv (else return value truncated to int on x64) */
#include <ps3emu/host_platform.h>
#ifdef _WIN32
#include <process.h>   /* _beginthreadex: CRT-aware thread creation (raw CreateThread
                        * leaves per-thread CRT state uninit -> buffered fread() silently
                        * returns 0 on those threads). */
#endif

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
ppu_thread_info       g_ppu_threads[PPU_THREAD_MAX];
vm_stack_alloc        g_vm_stack_alloc;
ppu_thread_entry_fn   g_ppu_thread_entry_trampoline = NULL;

/* Implemented by the lifted-code runtime. */
extern void ppu_dump_guest_stack(ppu_context* ctx, const char* tag);

void sys_ppu_thread_dump_guest_stacks(void)
{
    /* Diagnostic-only lock-free snapshot.  Taking s_table_lock here could
     * deadlock when the watchdog suspended its current owner. */
    for (int i = 0; i < PPU_THREAD_MAX; i++) {
        ppu_thread_info* t = &g_ppu_threads[i];
        if (t->state == PPU_THREAD_STATE_FREE) continue;
        fprintf(stderr,
                "[WATCHDOG]   guest tid=%llu state=%d name=\"%s\" "
                "cia=0x%08X lr=0x%08X sp=0x%08X entry=0x%08llX\n",
                (unsigned long long)t->ctx.thread_id, t->state, t->name,
                (uint32_t)t->ctx.cia, (uint32_t)t->ctx.lr,
                (uint32_t)t->ctx.gpr[1], (unsigned long long)t->entry_addr);
        ppu_dump_guest_stack(&t->ctx, t->name[0] ? t->name : "thread");
    }
}

/* Simple mutex for thread table access */
#ifdef _WIN32
static CRITICAL_SECTION s_table_lock;
static int              s_table_lock_init = 0;
#else
static pthread_mutex_t  s_table_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void table_lock(void)
{
#ifdef _WIN32
    if (!s_table_lock_init) {
        InitializeCriticalSection(&s_table_lock);
        s_table_lock_init = 1;
    }
    EnterCriticalSection(&s_table_lock);
#else
    pthread_mutex_lock(&s_table_lock);
#endif
}

static void table_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&s_table_lock);
#else
    pthread_mutex_unlock(&s_table_lock);
#endif
}

/* Find a free slot. Returns index or -1. Must be called under lock. */
static int find_free_slot(void)
{
    /* LV2 reserves a real, non-zero ID for the process's initial PPU thread.
     * The main recompiler context is not stored in this table, so reserve slot
     * zero for it and allocate worker IDs from 2 onward.  Reusing ID 1 for the
     * first worker made sys_ppu_thread_get_id report the same identity for main
     * and a worker, breaking guest recursive-mutex ownership checks. */
    for (int i = 1; i < PPU_THREAD_MAX; i++) {
        if (g_ppu_threads[i].state == PPU_THREAD_STATE_FREE)
            return i;
    }
    return -1;
}

/* Find thread by ID. The ID is the index + 1 (0 is invalid). */
static ppu_thread_info* find_thread(uint64_t thread_id)
{
    if (thread_id == 0 || thread_id > PPU_THREAD_MAX) return NULL;
    ppu_thread_info* t = &g_ppu_threads[thread_id - 1];
    if (t->state == PPU_THREAD_STATE_FREE) return NULL;
    return t;
}

/* ---------------------------------------------------------------------------
 * Host thread entry point
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
static DWORD WINAPI ppu_host_thread_proc(LPVOID param)
#else
static void* ppu_host_thread_proc(void* param)
#endif
{
    ppu_thread_info* info = (ppu_thread_info*)param;

    /* Join the lwarx/stwcx reservation set, else nobody breaks this thread's
     * reservations and ABA slips through for it (see ppu_loader.cpp). */
    { extern void ppu_resv_register(ppu_context*); ppu_resv_register(&info->ctx); }

    /* Keep the frame callbacks off the deadline threads (see ppu_hle.cpp). */
    { extern void ppu_set_no_gcm_pump(int on);
      ppu_set_no_gcm_pump(info->priority == 0); }

    fprintf(stderr, "[THREAD %llu] host thread started, host_tid=%lu name=\"%s\" entry=0x%08llX\n",
            (unsigned long long)info->ctx.thread_id,
            (unsigned long)ps3_host_thread_id(), info->name,
            (unsigned long long)info->entry_addr);

    /* Invoke the recompiled entry point */
    if (g_ppu_thread_entry_trampoline) {
        g_ppu_thread_entry_trampoline(&info->ctx);
        fprintf(stderr, "[THREAD %llu] entry RETURNED (r3=0x%llX) -- thread finished\n",
                (unsigned long long)info->ctx.thread_id,
                (unsigned long long)info->ctx.gpr[3]);
    } else {
        fprintf(stderr, "[THREAD %llu] g_ppu_thread_entry_trampoline is NULL — thread is a no-op!\n",
                (unsigned long long)info->ctx.thread_id);
    }

    /* Mark as finished.  A detached descriptor must not become reusable until
     * this host entry has completely unwound: sys_ppu_thread_exit runs inside
     * the lifted entry and is therefore too early to publish FREE. */
    table_lock();
    info->exit_status = (int64_t)info->ctx.gpr[3];

    const int detached = (info->state == PPU_THREAD_STATE_DETACHED);
    const int final_state = ppu_thread_state_after_host_exit(info->state);
    if (detached) {
        /* Threads created detached have no completion objects.  A running
         * joinable thread can also be detached later, in which case its now
         * unreachable completion objects are owned and destroyed here. */
#ifdef _WIN32
        if (info->completion_initialized && info->finish_event)
            CloseHandle(info->finish_event);
        if (info->host_thread)
            CloseHandle(info->host_thread);
        info->finish_event = NULL;
        info->host_thread = NULL;
#else
        if (info->completion_initialized) {
            pthread_mutex_destroy(&info->finish_mutex);
            pthread_cond_destroy(&info->finish_cond);
        }
#endif
        info->completion_initialized = 0;
    }
    /* A joiner reserves the slot while it waits outside s_table_lock. */
    info->state = final_state;

    /* Signal before dropping s_table_lock.  Otherwise a concurrent detach of
     * this now-FINISHED thread could destroy the completion object first. */
    if (!detached && info->completion_initialized) {
#ifdef _WIN32
        SetEvent(info->finish_event);
#else
        pthread_mutex_lock(&info->finish_mutex);
        info->finished = 1;
        pthread_cond_signal(&info->finish_cond);
        pthread_mutex_unlock(&info->finish_mutex);
#endif
    }
    table_unlock();

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* YDKJ_THREADGATE: PS3 priority scheduling — a newly created same/lower-priority
 * thread does NOT run until the creating thread blocks. Our HLE spawns host threads
 * immediately, so a worker (GThread entry=0x5353C0) can read its job object's
 * [arg+0x10] owner link BEFORE the main thread finishes linking it -> null -> spin.
 * Fix: create workers SUSPENDED and resume them only when a thread first blocks on
 * an event-queue wait (by then the creator has finished initialization). */
#ifdef _WIN32
static HANDLE g_gate_pending[256];
static int    g_gate_close_after_resume[256];
static int    g_gate_n = 0;
static int    g_gate_on = -1;
void ydkj_release_pending_threads(void)
{
    if (g_gate_on <= 0) return;
    table_lock();
    int n = g_gate_n; g_gate_n = 0;
    for (int i = 0; i < n; i++) {
        if (!g_gate_pending[i]) continue;
        ResumeThread(g_gate_pending[i]);
        if (g_gate_close_after_resume[i])
            CloseHandle(g_gate_pending[i]);
        g_gate_pending[i] = NULL;
        g_gate_close_after_resume[i] = 0;
    }
    table_unlock();
    if (n) fprintf(stderr, "[THREADGATE] released %d pending worker thread(s) on first block\n", n);
}
#else
void ydkj_release_pending_threads(void) {}
#endif

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_create
 *
 * r3 = pointer to receive thread ID (u64*)
 * r4 = entry point address
 * r5 = argument (passed in new thread's r3)
 * r6 = priority (s32)
 * r7 = stack size
 * r8 = flags
 * r9 = thread name pointer
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_create(ppu_context* ctx)
{
    uint32_t tid_out_addr = LV2_ARG_PTR(ctx, 0);
    uint64_t entry        = LV2_ARG_U64(ctx, 1);
    uint64_t arg          = LV2_ARG_U64(ctx, 2);
    int32_t  priority     = LV2_ARG_S32(ctx, 3);
    uint32_t stack_size   = LV2_ARG_U32(ctx, 4);
    uint64_t flags        = LV2_ARG_U64(ctx, 5);
    uint32_t name_addr    = LV2_ARG_PTR(ctx, 6);
    const int joinable    = ppu_thread_flags_joinable(flags);

    if (stack_size == 0) stack_size = VM_PPU_STACK_SIZE;
    if (stack_size < 0x4000) stack_size = 0x4000; /* 16 KB minimum */
    stack_size = VM_ALIGN_UP(stack_size, vm_host_page_size());

    table_lock();

    int slot = find_free_slot();
    if (slot < 0) {
        table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    ppu_thread_info* t = &g_ppu_threads[slot];
    /* Keep one guest stack cached per descriptor.  The old bump-only path
     * leaked a stack on every joined or detached short-lived thread. */
    const uint32_t cached_stack_addr = t->stack_addr;
    const uint32_t cached_stack_size = t->stack_size;
    memset(t, 0, sizeof(*t));

    /* Allocate guest stack */
    uint32_t stack_addr = 0;
    if (cached_stack_addr && cached_stack_size >= stack_size)
        stack_addr = cached_stack_addr;
    else
        stack_addr = vm_stack_allocate(&g_vm_stack_alloc, stack_size);
    if (stack_addr == 0) {
        t->stack_addr = cached_stack_addr;
        t->stack_size = cached_stack_size;
        table_unlock();
        return (int64_t)(int32_t)CELL_ENOMEM;
    }

    /* Set up the PPU context for the new thread */
    ppu_context_init(&t->ctx);
    t->ctx.cia = entry;
    t->ctx.gpr[3] = arg;
    ppu_set_stack(&t->ctx, (uint64_t)stack_addr, (uint64_t)stack_size);
    /* Copy TOC from creating thread */
    t->ctx.gpr[2] = ctx->gpr[2];

    uint64_t thread_id = (uint64_t)(slot + 1);
    t->ctx.thread_id = thread_id;

    t->state      = joinable ? PPU_THREAD_STATE_RUNNING
                             : PPU_THREAD_STATE_DETACHED;
    t->priority   = priority;
    t->joinable   = joinable;
    t->stack_addr = stack_addr;
    t->stack_size = stack_size;
    t->entry_addr = entry;

    /* Copy thread name if provided */
    if (name_addr != 0) {
        const char* name = (const char*)vm_to_host(name_addr);
        strncpy(t->name, name, sizeof(t->name) - 1);
        t->name[sizeof(t->name) - 1] = '\0';
    }

    /* Detached-at-creation threads have no legal joiner, so do not create
     * completion objects that would need a second owner to reap them. */
    t->completion_initialized = joinable;
    if (joinable) {
#ifdef _WIN32
        t->finish_event = CreateEventA(NULL, TRUE, FALSE, NULL);
#else
        pthread_mutex_init(&t->finish_mutex, NULL);
        pthread_cond_init(&t->finish_cond, NULL);
        t->finished = 0;
#endif
    }

    /* Write thread ID to output pointer */
    if (tid_out_addr != 0) {
        uint64_t* out = (uint64_t*)vm_to_host(tid_out_addr);
        /* Store as big-endian u64 */
        uint64_t be_id = thread_id;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        be_id = ((be_id >> 56) & 0xFF) |
                ((be_id >> 40) & 0xFF00) |
                ((be_id >> 24) & 0xFF0000) |
                ((be_id >>  8) & 0xFF000000ULL) |
                ((be_id <<  8) & 0xFF00000000ULL) |
                ((be_id << 24) & 0xFF0000000000ULL) |
                ((be_id << 40) & 0xFF000000000000ULL) |
                ((be_id << 56) & 0xFF00000000000000ULL);
#endif
        *out = be_id;
    }

    fprintf(stderr, "[SYS] sys_ppu_thread_create name=\"%s\" entry=0x%08llX arg=0x%llX stack=0x%X prio=%d flags=0x%llX %s\n",
            t->name, (unsigned long long)entry, (unsigned long long)arg,
            stack_size, priority, (unsigned long long)flags,
            joinable ? "joinable" : "detached");
    /* YDKJ: dump the worker arg-object: func_000750A8 (thread body) does
     * this=[arg+0x8], vtable=[arg+0xC], method=[vtable+0]. If this(+0x8) is null
     * the worker dispatches its job on a null object -> construction never runs. */
    { extern uint8_t* vm_base; uint32_t a=(uint32_t)arg;
      if(a && a<0x50000000u && getenv("YDKJ_THREADARG")){
        #define RB(o) (((uint32_t)vm_base[(a+(o))&0x0FFFFFFFu]<<24)|((uint32_t)vm_base[(a+(o)+1)&0x0FFFFFFFu]<<16)|((uint32_t)vm_base[(a+(o)+2)&0x0FFFFFFFu]<<8)|vm_base[(a+(o)+3)&0x0FFFFFFFu])
        uint32_t self=RB(0x0), thisp=RB(0x8), vtbl=RB(0xC);
        fprintf(stderr,"[THREADARG] arg=0x%08X [+0]=0x%08X this[+8]=0x%08X vtbl[+C]=0x%08X\n", a, self, thisp, vtbl);
        #undef RB
      } }

    /* Diagnostic (YDKJ_NOHDLR): suppress libsre's SPURS handler threads (entry in
     * the libsre image range) -- they assert that the SPU side isn't operational
     * and crash. Skipping them lets the main thread (already past
     * cellSpursInitialize) keep running, to see how far it gets. The thread is
     * "created" (tid returned) but never spawned. */
    if (getenv("YDKJ_NOHDLR") && entry >= 0x30000000 && entry < 0x30040000) {
        fprintf(stderr, "[SYS]   (suppressed libsre handler thread entry=0x%08llX)\n",
                (unsigned long long)entry);
        t->state = PPU_THREAD_STATE_RUNNING; /* leave it parked */
        table_unlock();
        return CELL_OK;
    }

    /* Create the host thread. Give it a large RESERVED stack: each recompiled
     * guest call is a real host call, so deep guest call chains nest deeply on
     * the host stack and overflow the 1 MB default. Reserve 256 MB (committed
     * lazily by the OS via STACK_SIZE_PARAM_IS_A_RESERVATION). */
#ifdef _WIN32
    if (g_gate_on < 0) g_gate_on = getenv("YDKJ_THREADGATE") ? 1 : 0;
    /* Gate only guest worker threads (game .text entry), never libsre/system threads. */
    unsigned _initflag = STACK_SIZE_PARAM_IS_A_RESERVATION;
    int _gate_this = (g_gate_on > 0 && entry >= 0x10000 && entry < 0x10000000);
    if (_gate_this) _initflag |= CREATE_SUSPENDED;
    t->host_thread = (HANDLE)_beginthreadex(NULL, 256u * 1024 * 1024,
                                  (unsigned (__stdcall*)(void*))ppu_host_thread_proc, t,
                                  _initflag, (unsigned*)&t->host_tid);
    if (t->host_thread == NULL) {
        t->state = PPU_THREAD_STATE_FREE;
        if (t->completion_initialized)
            CloseHandle(t->finish_event);
        t->completion_initialized = 0;
        table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
    if (_gate_this && g_gate_n < 256) {
        g_gate_pending[g_gate_n] = t->host_thread;
        g_gate_close_after_resume[g_gate_n] = !joinable;
        g_gate_n++;
    }
    if (!joinable) {
        /* Closing a Windows thread handle does not terminate the thread.  Do
         * it while s_table_lock prevents the descriptor from being reused. */
        if (!_gate_this)
            CloseHandle(t->host_thread);
        t->host_thread = NULL;
    }
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u * 1024 * 1024);
    if (!joinable)
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t->host_thread, &attr, ppu_host_thread_proc, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        t->state = PPU_THREAD_STATE_FREE;
        if (t->completion_initialized) {
            pthread_mutex_destroy(&t->finish_mutex);
            pthread_cond_destroy(&t->finish_cond);
        }
        t->completion_initialized = 0;
        table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
#endif

    table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_exit
 *
 * r3 = exit status
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_exit(ppu_context* ctx)
{
    uint64_t status = LV2_ARG_U64(ctx, 0);
    uint64_t tid = ctx->thread_id;
    fprintf(stderr, "[SYS] sys_ppu_thread_exit(tid=%llu status=%llu)\n",
            (unsigned long long)tid, (unsigned long long)status);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (t) {
        t->exit_status = (int64_t)status;
        const int detached = (t->state == PPU_THREAD_STATE_DETACHED);
        t->state = ppu_thread_state_after_guest_exit(t->state);

        if (!detached && t->completion_initialized) {
#ifdef _WIN32
            SetEvent(t->finish_event);
#else
            pthread_mutex_lock(&t->finish_mutex);
            t->finished = 1;
            pthread_cond_signal(&t->finish_cond);
            pthread_mutex_unlock(&t->finish_mutex);
#endif
        }
    }
    table_unlock();

    /* In a real implementation this would terminate the calling thread.
     * The thread proc wrapper handles this after return. */
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_join
 *
 * r3 = thread_id
 * r4 = pointer to receive exit status (s64*)
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_join(ppu_context* ctx)
{
    uint64_t tid          = LV2_ARG_U64(ctx, 0);
    uint32_t status_addr  = LV2_ARG_PTR(ctx, 1);
    { static int n=0; if(n++<30) fprintf(stderr,"[WAIT] ppu_thread_join(tid=%llu)\n", (unsigned long long)tid); }

    if (tid == ctx->thread_id)
        return (int64_t)(int32_t)CELL_EDEADLK;

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    if (!t->joinable || t->state == PPU_THREAD_STATE_DETACHED ||
        t->state == PPU_THREAD_STATE_JOINING) {
        table_unlock();
        return (int64_t)(int32_t)CELL_EINVAL;
    }

    /* Reserve this descriptor so another join/detach cannot clean it up or
     * reuse its slot while the host wait runs without s_table_lock. Holding
     * s_table_lock across pthread_join deadlocks: ppu_host_thread_proc needs
     * that lock to mark itself finished before the host thread can return. */
    t->joinable = 0;
    t->state = PPU_THREAD_STATE_JOINING;
#ifdef _WIN32
    HANDLE host_thread = t->host_thread;
#else
    pthread_t host_thread = t->host_thread;
#endif
    table_unlock();

    /* Wait for the actual host thread, not merely finish_event/finished.
     * sys_ppu_thread_exit can publish the guest status before the lifted entry
     * has unwound back through ppu_host_thread_proc. */
#ifdef _WIN32
    WaitForSingleObject(host_thread, INFINITE);
#else
    pthread_join(host_thread, NULL);
#endif

    /* Write exit status */
    if (status_addr != 0) {
        int64_t es = t->exit_status;
        int64_t* out = (int64_t*)vm_to_host(status_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        uint64_t u = (uint64_t)es;
        u = ((u >> 56) & 0xFF) |
            ((u >> 40) & 0xFF00) |
            ((u >> 24) & 0xFF0000) |
            ((u >>  8) & 0xFF000000ULL) |
            ((u <<  8) & 0xFF00000000ULL) |
            ((u << 24) & 0xFF0000000000ULL) |
            ((u << 40) & 0xFF000000000000ULL) |
            ((u << 56) & 0xFF00000000000000ULL);
        *out = (int64_t)u;
#else
        *out = es;
#endif
    }

    /* The target can no longer touch its descriptor or completion objects. */
#ifdef _WIN32
    CloseHandle(host_thread);
    CloseHandle(t->finish_event);
#else
    pthread_mutex_destroy(&t->finish_mutex);
    pthread_cond_destroy(&t->finish_cond);
#endif

    table_lock();
#ifdef _WIN32
    t->host_thread = NULL;
    t->finish_event = NULL;
#endif
    t->completion_initialized = 0;
    t->state = PPU_THREAD_STATE_FREE;
    table_unlock();

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_detach
 *
 * r3 = thread_id
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_detach(ppu_context* ctx)
{
    uint64_t tid = LV2_ARG_U64(ctx, 0);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    if (!t->joinable || t->state == PPU_THREAD_STATE_JOINING) {
        table_unlock();
        return (int64_t)(int32_t)CELL_EINVAL;
    }

    if (t->state == PPU_THREAD_STATE_FINISHED) {
        /* Already finished, free it */
#ifdef _WIN32
        CloseHandle(t->host_thread);
        CloseHandle(t->finish_event);
        t->host_thread = NULL;
        t->finish_event = NULL;
#else
        pthread_detach(t->host_thread);
        pthread_mutex_destroy(&t->finish_mutex);
        pthread_cond_destroy(&t->finish_cond);
#endif
        t->completion_initialized = 0;
        t->state = PPU_THREAD_STATE_FREE;
    } else {
        t->state = PPU_THREAD_STATE_DETACHED;
        t->joinable = 0;
#ifdef _WIN32
        if (t->host_thread) {
            CloseHandle(t->host_thread);
            t->host_thread = NULL;
        }
#else
        pthread_detach(t->host_thread);
#endif
    }

    table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_yield
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_yield(ppu_context* ctx)
{
    (void)ctx;
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_get_priority
 *
 * r3 = thread_id
 * r4 = pointer to receive priority (s32*)
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_get_priority(ppu_context* ctx)
{
    uint64_t tid       = LV2_ARG_U64(ctx, 0);
    uint32_t prio_addr = LV2_ARG_PTR(ctx, 1);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        /* The main thread (and any thread we didn't spawn via sys_ppu_thread_create)
         * isn't in our table. Returning ESRCH here is fatal for engines that query
         * their own priority at startup (PhyreEngine PApplication::PlatformInit ->
         * "Error initializing PSSG"). Report a sane default priority + success. */
        table_unlock();
        if (prio_addr != 0) {
            int32_t prio = 1000;
            int32_t* out = (int32_t*)vm_to_host(prio_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
            uint32_t u = (uint32_t)prio;
            u = ((u >> 24) & 0xFF) | ((u >> 8) & 0xFF00) |
                ((u <<  8) & 0xFF0000) | ((u << 24) & 0xFF000000u);
            *out = (int32_t)u;
#else
            *out = prio;
#endif
        }
        return CELL_OK;
    }

    if (prio_addr != 0) {
        int32_t prio = t->priority;
        int32_t* out = (int32_t*)vm_to_host(prio_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        uint32_t u = (uint32_t)prio;
        u = ((u >> 24) & 0xFF) | ((u >> 8) & 0xFF00) |
            ((u <<  8) & 0xFF0000) | ((u << 24) & 0xFF000000u);
        *out = (int32_t)u;
#else
        *out = prio;
#endif
    }

    table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_set_priority
 *
 * r3 = thread_id
 * r4 = priority
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_set_priority(ppu_context* ctx)
{
    uint64_t tid      = LV2_ARG_U64(ctx, 0);
    int32_t  priority = LV2_ARG_S32(ctx, 1);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    t->priority = priority;
    table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_rename
 *
 * r3 = thread_id
 * r4 = name pointer
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_rename(ppu_context* ctx)
{
    uint64_t tid       = LV2_ARG_U64(ctx, 0);
    uint32_t name_addr = LV2_ARG_PTR(ctx, 1);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    if (name_addr != 0) {
        const char* name = (const char*)vm_to_host(name_addr);
        strncpy(t->name, name, sizeof(t->name) - 1);
        t->name[sizeof(t->name) - 1] = '\0';
    }

    table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_get_join_state
 *
 * r3 = pointer to receive join state (s32*)
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_get_join_state(ppu_context* ctx)
{
    uint32_t out_addr = LV2_ARG_PTR(ctx, 0);
    uint64_t tid = ctx->thread_id;

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    int32_t joinable = t ? t->joinable : 0;
    table_unlock();

    if (out_addr != 0) {
        int32_t* out = (int32_t*)vm_to_host(out_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        uint32_t u = (uint32_t)joinable;
        u = ((u >> 24) & 0xFF) | ((u >> 8) & 0xFF00) |
            ((u <<  8) & 0xFF0000) | ((u << 24) & 0xFF000000u);
        *out = (int32_t)u;
#else
        *out = joinable;
#endif
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_get_stack_information
 *
 * r3 = pointer to receive stack info struct
 *   struct { u32 addr; u32 size; }
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_get_stack_information(ppu_context* ctx)
{
    uint32_t out_addr = LV2_ARG_PTR(ctx, 0);
    uint64_t tid = ctx->thread_id;

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        /* For the main thread, return sensible defaults */
        if (out_addr != 0) {
            uint32_t* out = (uint32_t*)vm_to_host(out_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
            /* byteswap both fields */
            uint32_t sa = VM_STACK_BASE;
            uint32_t ss = VM_PPU_STACK_SIZE;
            sa = ((sa >> 24) & 0xFF) | ((sa >> 8) & 0xFF00) |
                 ((sa <<  8) & 0xFF0000) | ((sa << 24) & 0xFF000000u);
            ss = ((ss >> 24) & 0xFF) | ((ss >> 8) & 0xFF00) |
                 ((ss <<  8) & 0xFF0000) | ((ss << 24) & 0xFF000000u);
            out[0] = sa;
            out[1] = ss;
#else
            out[0] = VM_STACK_BASE;
            out[1] = VM_PPU_STACK_SIZE;
#endif
        }
        return CELL_OK;
    }

    if (out_addr != 0) {
        uint32_t* out = (uint32_t*)vm_to_host(out_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        uint32_t sa = t->stack_addr;
        uint32_t ss = t->stack_size;
        sa = ((sa >> 24) & 0xFF) | ((sa >> 8) & 0xFF00) |
             ((sa <<  8) & 0xFF0000) | ((sa << 24) & 0xFF000000u);
        ss = ((ss >> 24) & 0xFF) | ((ss >> 8) & 0xFF00) |
             ((ss <<  8) & 0xFF0000) | ((ss << 24) & 0xFF000000u);
        out[0] = sa;
        out[1] = ss;
#else
        out[0] = t->stack_addr;
        out[1] = t->stack_size;
#endif
    }

    table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_ppu_thread_init(lv2_syscall_table* tbl)
{
    /* Initialize stack allocator */
    vm_stack_alloc_init(&g_vm_stack_alloc);

    /* Clear thread table */
    memset(g_ppu_threads, 0, sizeof(g_ppu_threads));

#ifdef _WIN32
    if (!s_table_lock_init) {
        InitializeCriticalSection(&s_table_lock);
        s_table_lock_init = 1;
    }
#endif

    lv2_syscall_register(tbl, SYS_PPU_THREAD_CREATE,              sys_ppu_thread_create);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_EXIT,                sys_ppu_thread_exit);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_YIELD,               sys_ppu_thread_yield);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_JOIN,                sys_ppu_thread_join);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_DETACH,              sys_ppu_thread_detach);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_GET_JOIN_STATE,      sys_ppu_thread_get_join_state);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_SET_PRIORITY,        sys_ppu_thread_set_priority);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_GET_PRIORITY,        sys_ppu_thread_get_priority);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_GET_STACK_INFORMATION, sys_ppu_thread_get_stack_information);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_RENAME,              sys_ppu_thread_rename);
}

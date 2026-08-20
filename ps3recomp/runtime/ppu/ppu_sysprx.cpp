/*
 * ps3recomp - sysPrxForUser CRT (boot-critical HLE)
 *
 * The first firmware functions a PS3 program calls at startup come from
 * sysPrxForUser (the libc/CRT bridge). Some need the full ppu_context (e.g.
 * sys_initialize_tls sets the thread pointer r13), so they register as
 * context-aware handlers (ps3_hle_register_ctx) rather than through the generic
 * integer-ABI table.
 *
 * NIDs are computed from the names (ps3_compute_nid), so this stays correct
 * without hand-written NID literals.
 */
#include "ppu_recomp.h"     /* ppu_context */
#include "ps3emu/nid.h"     /* ps3_compute_nid */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <map>

extern "C" uint8_t* vm_base;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" uint32_t vm_read32(uint64_t a);
extern "C" void     vm_write32(uint64_t a, uint32_t v);
extern "C" void     vm_write64(uint64_t a, uint64_t v);
extern "C" void     ps3_indirect_call(ppu_context* ctx);
extern "C" uint64_t ppu_timebase_usec_now(void);

/* TLS blocks come from the loader's allocator so this and the per-thread
 * allocation in ppu_thread_entry_trampoline cannot hand out overlapping areas. */
extern "C" uint32_t ppu_tls_alloc(uint32_t seg_addr, uint32_t seg_size, uint32_t mem_size);

/* sys_initialize_tls(u64 main_thread_id, u32 tls_seg_addr, u32 tls_seg_size,
 *                     u32 tls_mem_size) -- set up the main thread's TLS block
 * and point r13 (the PPC64 thread pointer) at it. TLS variables are accessed
 * at r13 - 0x7000 (the static TLS block bias). */
static void sys_initialize_tls(ppu_context* ctx)
{
    uint32_t seg_addr = (uint32_t)ctx->gpr[4];
    uint32_t seg_size = (uint32_t)ctx->gpr[5];
    uint32_t mem_size = (uint32_t)ctx->gpr[6];

    ctx->gpr[13] = ppu_tls_alloc(seg_addr, seg_size, mem_size);
    ctx->gpr[3]  = 0;                  /* CELL_OK */
    fprintf(stderr, "[crt] sys_initialize_tls: r13=0x%08X (seg 0x%X+%u, mem %u)\n",
            (uint32_t)ctx->gpr[13], seg_addr, seg_size, mem_size);
}

/* sys_ppu_thread_once(sys_ppu_thread_once_t* once, void(*init)(void))
 *
 * The PS3 pthread_once. The init routine is GUEST code behind an OPD, so this
 * has to actually dispatch it -- returning CELL_OK without running it leaves
 * every one-time initializer unexecuted, and the statics they build stay zero
 * (which reads downstream as e.g. an empty std::string path handed to
 * cellFsOpen, far from the real cause).
 *
 * ponytail: the guard word is set BEFORE dispatch, not after, so an init
 * routine that re-enters sys_ppu_thread_once on the same control word returns
 * instead of recursing forever. That is also why there is no lock here: a
 * second thread racing the first would skip the init rather than wait for it.
 * Boot-time one-time init is single-threaded; add a per-control-word condvar if
 * a title is ever seen calling this concurrently.
 */
static void sys_ppu_thread_once(ppu_context* ctx)
{
    uint32_t once_ea  = (uint32_t)ctx->gpr[3];
    uint32_t init_opd = (uint32_t)ctx->gpr[4];

    if (once_ea && init_opd && vm_read32(once_ea) == 0) {
        vm_write32(once_ea, 1);
        uint64_t save_lr = ctx->lr, save_r2 = ctx->gpr[2], save_ctr = ctx->ctr;
        ctx->ctr     = vm_read32(init_opd);
        ctx->gpr[2]  = vm_read32(init_opd + 4);
        ps3_indirect_call(ctx);
        ctx->ctr = save_ctr; ctx->gpr[2] = save_r2; ctx->lr = save_lr;
    }
    ctx->gpr[3] = 0;
}

/* sys_time_get_system_time() -> monotonic microseconds since system start.
 *
 * This must be time-driven, not call-driven.  The old bring-up stub advanced
 * one millisecond per invocation, so a polling-heavy guest made its gameplay
 * clock race ahead of the independently device-paced audio clock.  Periodic
 * synchronization then pulled the gameplay clock back, producing a repeating
 * fast-forward/snap-back cycle.
 *
 * Derive this API from the same 79.8 MHz clock used by lifted mftb/mftbu so all
 * guest clocks remain in one monotonic domain.  Split the conversion to avoid
 * overflowing ticks * 1,000,000 during long runs. */
static void sys_time_get_system_time(ppu_context* ctx)
{
    ctx->gpr[3] = ppu_timebase_usec_now();
}

/* sys_process_is_stack(u32 addr) -> 1 if addr is in the stack region. We model
 * a single stack just below the TLS region; good enough for boot checks. */
static void sys_process_is_stack(ppu_context* ctx)
{
    uint32_t a = (uint32_t)ctx->gpr[3];
    ctx->gpr[3] = (a >= 0x0E000000u && a < 0x10000000u) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * Lightweight mutex (sys_lwmutex) — sysPrxForUser.
 *
 * The CRT guards global/singleton initialization with lwmutexes. If create is
 * a no-op that never initializes the structure, the guarded init is skipped
 * and the protected registry is left with null function pointers (the early
 * boot then spins calling a null vtable entry). We model the structure for
 * real, and lock for real -- see the host-mutex block below.
 *
 * sys_lwmutex_t (big-endian, 24 bytes):
 *   +0x00 owner (u32)   +0x04 waiter (u32)   +0x08 attribute (u32)
 *   +0x0C recursive_count (u32)   +0x10 sleep_queue (u32)   +0x14 pad
 * sys_lwmutex_attribute_t: +0x00 protocol  +0x04 recursive  +0x08 name[8]
 * -----------------------------------------------------------------------*/
#define LWM_OWNER  0x00
#define LWM_ATTR   0x08
#define LWM_RECUR  0x0C
/* Guest-visible owner id. Worker slot zero is reserved in sys_ppu_thread.c, so
 * ID 1 uniquely names the initial PPU thread and worker IDs begin at 2. */
#define LWM_FREE   0u
static inline uint32_t lwm_self(ppu_context* ctx)
{
    uint32_t t = ctx ? (uint32_t)ctx->thread_id : 0u;
    return t ? t : 1u;
}

/* Real per-lwmutex host mutex. The lock impl below used to be a no-op owner
 * stamp (single-thread boot assumption) providing NO mutual exclusion — fatal
 * once SPURSKERNEL/GThreads/AsyncLoad run concurrently and share heap structures
 * bracketed by sys_lwmutex_lock/unlock. A real mutex keyed on the guest lwmutex
 * EA serializes them, fixing the free-list + pointer races systemically.
 *
 * Why this matters here: the title takes lwmutexes at 888 call sites across 523
 * distinct guest functions, including 35 inside the allocator/CRT range
 * (0x5A0000-0x5B0000). With the no-op, 17 guest threads shared every one of
 * those structures with zero serialization, and the guest's OWN allocator
 * catches the result -- `chunksize(p) == small_index2size(i)` in malloc_lv2.c,
 * i.e. overwritten heap chunk headers. Measured 2026-08-11: two runs of the same
 * binary died at 52 and 884 file opens at different call sites, which is a race,
 * not a lifted-code bug.
 *
 * Blocking acquires are BOUNDED (lwm_lock_timed). A plain lock() deadlocked the
 * boot at 5 file opens: a guest thread takes an lwmutex and then blocks forever
 * on the SPU event queue that is never satisfied (the "[cellSpurs] EventFlagWait
 * ... force-satisfying" / q=5 gap), so everyone queues behind it. The no-op hid
 * that by making the lock invisible. PPU_NOLWM=1 restores the no-op for A/B.
 * Locks that protect short, proven loader critical sections use strict mode;
 * allocator/CRT locks retain a diagnostic timeout fallback. */
/* std::recursive_timed_mutex permits barging: a busy thread can release and
 * immediately reacquire while an older waiter is still waking.  That is fatal
 * for this title's allocator lock -- service threads perform runs of small
 * allocations, and USBAuth can lose every race until each acquire reaches the
 * deadline.  It then crawls forward at one allocation per LWM_WAIT_MS and the
 * main thread never receives the service-ready bit that starts drawing.
 *
 * Keep recursive ownership, but queue contended first-level acquisitions in
 * arrival order.  Timed-out tickets remove themselves so an abandoned guest
 * lock still cannot wedge the host queue permanently. */
class FairRecursiveTimedMutex {
public:
    void lock(uint32_t self, uint32_t lr)
    {
        std::unique_lock<std::mutex> lock(gate_);
        if (owner_ == self) {
            ++depth_;
            return;
        }
        if (owner_ == 0 && waiters_.empty()) {
            owner_ = self;
            depth_ = 1;
            owner_lr_ = lr;
            return;
        }

        const uint64_t ticket = next_ticket_++;
        waiters_.push_back(ticket);
        cv_.wait(lock, [&] {
            return owner_ == 0 &&
                   !waiters_.empty() && waiters_.front() == ticket;
        });
        waiters_.pop_front();
        owner_ = self;
        depth_ = 1;
        owner_lr_ = lr;
    }

    bool try_lock(uint32_t self, uint32_t lr)
    {
        std::lock_guard<std::mutex> lock(gate_);
        if (owner_ == self) {
            ++depth_;
            return true;
        }
        if (owner_ == 0 && waiters_.empty()) {
            owner_ = self;
            depth_ = 1;
            owner_lr_ = lr;
            return true;
        }
        return false;
    }

    template<class Rep, class Period>
    bool try_lock_for(uint32_t self, uint32_t lr,
                      const std::chrono::duration<Rep, Period>& timeout)
    {
        std::unique_lock<std::mutex> lock(gate_);
        if (owner_ == self) {
            ++depth_;
            return true;
        }
        if (owner_ == 0 && waiters_.empty()) {
            owner_ = self;
            depth_ = 1;
            owner_lr_ = lr;
            return true;
        }

        const uint64_t ticket = next_ticket_++;
        waiters_.push_back(ticket);
        const auto ready = [&] {
            return owner_ == 0 &&
                   !waiters_.empty() && waiters_.front() == ticket;
        };
        if (!cv_.wait_for(lock, timeout, ready)) {
            const auto it = std::find(waiters_.begin(), waiters_.end(), ticket);
            const bool was_front = it != waiters_.end() && it == waiters_.begin();
            if (it != waiters_.end()) waiters_.erase(it);
            if (was_front && owner_ == 0) cv_.notify_all();
            return false;
        }

        waiters_.pop_front();
        owner_ = self;
        depth_ = 1;
        owner_lr_ = lr;
        return true;
    }

    void unlock(uint32_t self)
    {
        std::unique_lock<std::mutex> lock(gate_);
        if (owner_ != self || depth_ == 0) return;
        if (--depth_ != 0) return;
        owner_ = 0;
        owner_lr_ = 0;
        lock.unlock();
        cv_.notify_all();
    }

    /* Condition waits must drop every recursive level atomically.  Keeping
     * this beside owner_/depth_ avoids a second per-thread ownership table. */
    int release_all(uint32_t self)
    {
        std::unique_lock<std::mutex> lock(gate_);
        if (owner_ != self || depth_ == 0) return 0;
        const int depth = (int)depth_;
        owner_ = 0;
        depth_ = 0;
        owner_lr_ = 0;
        lock.unlock();
        cv_.notify_all();
        return depth;
    }

    void owner_snapshot(uint32_t& tid, uint32_t& lr)
    {
        std::lock_guard<std::mutex> lock(gate_);
        tid = owner_;
        lr = owner_lr_;
    }

private:
    std::mutex gate_;
    std::condition_variable cv_;
    uint32_t owner_ = 0;
    uint32_t depth_ = 0;
    uint32_t owner_lr_ = 0;
    uint64_t next_ticket_ = 0;
    std::deque<uint64_t> waiters_;
};

static std::mutex g_lwm_map_mtx;
static std::map<uint32_t, FairRecursiveTimedMutex> g_lwm_mtxs;
struct LwCondHost {
    std::mutex gate;
    std::condition_variable cv;
    uint64_t sequence = 0;
    uint32_t lwmutex = 0;
};
static std::map<uint32_t, LwCondHost> g_lwc_conds;
static inline int ydkj_reallwm(){ static int v=-1; if(v<0) v=getenv("PPU_NOLWM")?0:1; return v; }

static uint32_t lwm_cache_hash(uint32_t value, uint32_t mask)
{
    /* Guest mutex EAs are strongly aligned; mix upper bits before masking. */
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    return value & mask;
}

/* std::map nodes are stable, so cache the host mutex pointer after its first
 * creation.  This removes a global mutex + tree walk from every subsequent
 * recursive lock/unlock while preserving the authoritative map and lifetime. */
static constexpr uint32_t LWM_HOST_CACHE_SIZE = 4096;
struct LwmHostCacheEntry {
    std::atomic<uint32_t> lwm{0};
    std::atomic<FairRecursiveTimedMutex*> mutex{nullptr};
};
static LwmHostCacheEntry g_lwm_host_cache[LWM_HOST_CACHE_SIZE];

static FairRecursiveTimedMutex* lwm_host_mutex_cached(uint32_t lwm)
{
    uint32_t slot = lwm_cache_hash(lwm, LWM_HOST_CACHE_SIZE - 1u);
    for (uint32_t n = 0; n < LWM_HOST_CACHE_SIZE; ++n) {
        LwmHostCacheEntry& e = g_lwm_host_cache[(slot + n) & (LWM_HOST_CACHE_SIZE - 1u)];
        const uint32_t key = e.lwm.load(std::memory_order_acquire);
        if (key == lwm) return e.mutex.load(std::memory_order_acquire);
        if (key == 0) return nullptr;
    }
    return nullptr;
}

/* Called with g_lwm_map_mtx held. */
static void lwm_host_mutex_publish(uint32_t lwm, FairRecursiveTimedMutex* mutex)
{
    uint32_t slot = lwm_cache_hash(lwm, LWM_HOST_CACHE_SIZE - 1u);
    for (uint32_t n = 0; n < LWM_HOST_CACHE_SIZE; ++n) {
        LwmHostCacheEntry& e = g_lwm_host_cache[(slot + n) & (LWM_HOST_CACHE_SIZE - 1u)];
        const uint32_t key = e.lwm.load(std::memory_order_relaxed);
        if (key == lwm) {
            e.mutex.store(mutex, std::memory_order_release);
            return;
        }
        if (key == 0) {
            e.mutex.store(mutex, std::memory_order_relaxed);
            e.lwm.store(lwm, std::memory_order_release);
            return;
        }
    }
}

static FairRecursiveTimedMutex& lwm_host_mutex(uint32_t lwm){
    if (FairRecursiveTimedMutex* cached = lwm_host_mutex_cached(lwm))
        return *cached;
    std::lock_guard<std::mutex> g(g_lwm_map_mtx);
    FairRecursiveTimedMutex& mutex = g_lwm_mtxs[lwm];
    lwm_host_mutex_publish(lwm, &mutex);
    return mutex;
}

/* Bounded acquire. A plain lock() is correct but deadlocks this title outright:
 * a guest thread blocks forever inside a held lwmutex waiting on the SPU event
 * queue that is never satisfied, and every other thread then queues behind it
 * (measured: boot stops dead at 5 file opens).
 *
 * Waiting with a deadline makes that structurally impossible while keeping the
 * serialization that matters -- an uncontended or briefly-contended acquire
 * (which is nearly all of the 888 call sites) still blocks properly, and only a
 * wait that has clearly hit the dead SPU path gives up and proceeds unserialized,
 * exactly as the old no-op did for EVERY acquire.
 *
 * ponytail: this is a mitigation with a known ceiling, not a fix. Proceeding
 *           without the lock can still corrupt; it just needs a >LWM_WAIT_MS
 *           stall to do so instead of happening constantly. The real fix is to
 *           make the SPU wait complete, after which the timeout never fires and
 *           this can become a plain lock(). PPU_LWM_WAIT_MS tunes the deadline;
 *           the [lwm-timeout] line tells you whether it is firing at all. */
static long lwm_wait_ms(){ static long v=-1; if(v<0){ const char* e=getenv("PPU_LWM_WAIT_MS"); v=e?strtol(e,nullptr,10):200; if(v<1)v=1; } return v; }

/* Which lwmutexes to actually serialize.
 *
 * Serializing ALL of them is correct but unshippable here: measured 2026-08-11,
 * blanket locking starves the RSX texture uploads (Lumen content draws black,
 * the 0xCC0300 font atlas uploads as all zeros at a 200ms deadline) and drops
 * the frame rate to ~4 FPS, because a large share of the 888 acquisitions queue
 * behind the one lwmutex whose holder is parked forever in the SPU wait.
 *
 * The corruption we are actually chasing is HEAP METADATA -- the guest's
 * dlmalloc asserting on overwritten chunk headers -- so serialize the
 * allocator's own lock and leave the rest alone. An lwmutex is tagged as an
 * allocator lock the first time it is taken from a guest call site inside the
 * allocator/CRT text range; thereafter it is serialized no matter who takes it,
 * which is what matters (the heap lock must be honoured by every caller).
 *
 * PPU_LWM_ALL=1 serializes everything (the unshippable but strictly-correct
 * mode, for A/B); PPU_NOLWM=1 serializes nothing (the original no-op). */
#define LWM_ALLOC_LO 0x005A0000u
#define LWM_ALLOC_HI 0x005B0000u
enum class LwmPolicy : uint8_t { none, timed, strict };
static std::mutex g_lwm_state_mtx;
static std::map<uint32_t,LwmPolicy> g_lwm_policy;
static std::map<uint32_t,bool> g_lwm_trace_lrs;

/* Only a small subset of the title's lwmutexes need host serialization.  The
 * old policy lookup nevertheless took g_lwm_state_mtx and searched a std::map
 * on every one of the 888 guest lock call sites, including the overwhelmingly
 * common `none` result.  Cache only positive policies in a publish-only table:
 * an empty lookup can return `none` without touching a host mutex, while tagged
 * allocator/resource locks retain exactly the same timed/strict behaviour.
 *
 * Entries never disappear.  Writers are already serialized by
 * g_lwm_state_mtx; publishing the key last lets readers use the table without
 * a lock.  If the generously sized table ever fills, lookups fall back to the
 * authoritative std::map rather than weakening synchronization. */
struct LwmPolicyCacheEntry {
    std::atomic<uint32_t> lwm{0};
    std::atomic<uint8_t> policy{(uint8_t)LwmPolicy::none};
};
static constexpr uint32_t LWM_POLICY_CACHE_SIZE = 4096;
static LwmPolicyCacheEntry g_lwm_policy_cache[LWM_POLICY_CACHE_SIZE];
static std::atomic<bool> g_lwm_policy_cache_full{false};

static uint32_t lwm_policy_hash(uint32_t lwm)
{
    return lwm_cache_hash(lwm, LWM_POLICY_CACHE_SIZE - 1u);
}

static LwmPolicy lwm_policy_cached(uint32_t lwm)
{
    uint32_t slot = lwm_policy_hash(lwm);
    for (uint32_t n = 0; n < LWM_POLICY_CACHE_SIZE; ++n) {
        LwmPolicyCacheEntry& e = g_lwm_policy_cache[(slot + n) & (LWM_POLICY_CACHE_SIZE - 1u)];
        const uint32_t key = e.lwm.load(std::memory_order_acquire);
        if (key == lwm)
            return (LwmPolicy)e.policy.load(std::memory_order_acquire);
        if (key == 0)
            return LwmPolicy::none;
    }
    return LwmPolicy::none;
}

/* Called with g_lwm_state_mtx held. */
static void lwm_policy_publish(uint32_t lwm, LwmPolicy policy)
{
    uint32_t slot = lwm_policy_hash(lwm);
    for (uint32_t n = 0; n < LWM_POLICY_CACHE_SIZE; ++n) {
        LwmPolicyCacheEntry& e = g_lwm_policy_cache[(slot + n) & (LWM_POLICY_CACHE_SIZE - 1u)];
        const uint32_t key = e.lwm.load(std::memory_order_relaxed);
        if (key == lwm) {
            e.policy.store((uint8_t)policy, std::memory_order_release);
            return;
        }
        if (key == 0) {
            e.policy.store((uint8_t)policy, std::memory_order_relaxed);
            e.lwm.store(lwm, std::memory_order_release);
            return;
        }
    }
    g_lwm_policy_cache_full.store(true, std::memory_order_release);
}
static inline int lwm_all(){ static int v=-1; if(v<0) v=getenv("PPU_LWM_ALL")?1:0; return v; }
static inline int lwm_trace(){ static int v=-1; if(v<0) v=getenv("PPU_LWM_TRACE")?1:0; return v; }

/* Diagnostic: one line per dynamically observed lock call site.  Logging only
 * the first occurrence keeps a full boot small enough to compare while still
 * exposing which of the title's hundreds of lwmutex users are on this path. */
static void lwm_trace_first(uint32_t lwm, uint32_t lr, uint32_t tid)
{
    if (!lwm_trace()) return;
    std::lock_guard<std::mutex> g(g_lwm_state_mtx);
    if (g_lwm_trace_lrs.emplace(lr, true).second)
        fprintf(stderr, "[lwmtrace] lr=0x%08X lwm=0x%08X tid=%u\n", lr, lwm, tid);
}

/* These are the short lock wrappers around the async-job/resource registry's
 * lifetime accounting and hash tables
 * (erase, update, insert, find, clear) and its resource-pool lifetime counters
 * (acquire, reset, release). The table implementation uses a mutable sentinel:
 * every operation writes its search key to table+0x18 before walking the tree.
 * Even two concurrent readers can therefore make one search miss its sentinel,
 * fall through to guest address zero, and spin forever. The pool wrappers guard
 * object initialization, reference counts, and the final free; leaving those
 * as no-ops produces partially initialized 0xCD objects and 0xDD poisoned frees.
 *
 * Unlike the broad PPU_LWM_ALL experiment, these critical sections contain
 * only an in-memory table operation and cannot park on the unimplemented SPU
 * worker. They must never use the "proceed unserialized" timeout fallback. */
static bool lwm_strict_guard_lr(uint32_t lr)
{
    switch (lr) {
    /* Refcount/publish and release/destroy of an async file-load job. */
    case 0x002746DCu:
    case 0x00285C74u:
    /* Shared asset-manager dispatch.  These five entry points all take the
     * same singleton mutex, update its in-flight count, call one of the
     * manager operations, then release it.  The main loader and asynchronous
     * asset workers both enter this family. */
    case 0x001A89F4u:
    case 0x001A8AE0u:
    case 0x001A8B8Cu:
    case 0x001A8C40u:
    case 0x001A8D6Cu:
    /* Async callback slots used by the fumen/XML loader.  Each slot is a
     * mutex plus callback/state fields; the main thread publishes and polls
     * them while the worker (observed at 002BD534) changes completion state.
     * The generated code increments an in-flight count under this mutex before
     * reading or invoking those fields. */
    case 0x002BA34Cu:
    case 0x002BA460u:
    case 0x002BA4F8u:
    case 0x002BA57Cu:
    case 0x002BA6A4u:
    case 0x002BA80Cu:
    case 0x002BAACCu:
    case 0x002BAD94u:
    case 0x002BB088u:
    case 0x002BB33Cu:
    case 0x002BB600u:
    case 0x002BB8C4u:
    case 0x002BBB3Cu:
    case 0x002BBE04u:
    case 0x002BBE74u:
    case 0x002BBEECu:
    case 0x002BBF68u:
    case 0x002BC0E0u:
    case 0x002BC368u:
    case 0x002BC434u:
    case 0x002BC8E0u:
    case 0x002BCDB8u:
    case 0x002BD534u:
    /* Typed asset registries used while parsing tuning/fumen data.  These
     * wrappers all lock object+8 before querying or mutating the registry at
     * object+0x28.  The same wrapper runs over several adjacent registry
     * instances; leaving it unlocked produced an infinite repeated-string
     * lookup at 0x01405A30 in 007D5034. */
    case 0x0051CF1Cu:
    case 0x0051CFF0u:
    case 0x0051D0ACu:
    case 0x0051D158u:
    case 0x0051D20Cu:
    case 0x0051D2C0u:
    case 0x0051D3F0u:
    case 0x0051D574u:
    case 0x0051D618u:
    case 0x0051D6CCu:
    case 0x0051D780u:
    case 0x0051D83Cu:
    /* Resource-object manager: protected hash lookup, intrusive-list edits,
     * reference counts and final release.  This family is entered by the
     * asynchronous model/texture loader while the main thread queries the
     * same manager (00503F0C). */
    case 0x00503F0Cu:
    case 0x0053C18Cu:
    case 0x0053C278u:
    case 0x0053C360u:
    case 0x0053C45Cu:
    case 0x0053CAE8u:
    case 0x0053CC08u:
    case 0x0053CCBCu:
    case 0x0053CE04u:
    case 0x0053D00Cu:
    case 0x0053D1F4u:
    case 0x0053D304u:
    case 0x0053D414u:
    case 0x0053D4FCu:
    case 0x0053D66Cu:
    case 0x0053D780u:
    case 0x0053D948u:
    case 0x00555C3Cu:
    case 0x00555D3Cu:
    case 0x00556064u:
    /* Resource hash tables and pool lifetime counters. */
    case 0x00537D64u:
    case 0x00537E4Cu:
    case 0x00537FA4u:
    case 0x00538128u:
    case 0x00538214u:
    case 0x005385F4u:
    case 0x00538728u:
    case 0x00538870u:
        return true;
    default:
        return false;
    }
}

/* Select how this lwmutex is serialized. `lr` is the guest return address of
 * the caller, i.e. who is taking the lock. Once an EA is identified, remember
 * the strongest policy so every caller of the same guest mutex honours it. */
static LwmPolicy lwm_policy(uint32_t lwm, uint32_t lr)
{
    if (lwm_all()) return LwmPolicy::timed;

    const bool wants_strict = lwm_strict_guard_lr(lr);
    const bool wants_timed = lr >= LWM_ALLOC_LO && lr < LWM_ALLOC_HI;
    /* The positive cache is sufficient for almost every call.  A strict
     * caller must still upgrade a previously timed allocator policy. */
    const LwmPolicy cached = lwm_policy_cached(lwm);
    if (cached != LwmPolicy::none &&
        (!wants_strict || cached == LwmPolicy::strict))
        return cached;

    if (!wants_strict && !wants_timed &&
        !g_lwm_policy_cache_full.load(std::memory_order_acquire))
        return LwmPolicy::none;

    std::lock_guard<std::mutex> g(g_lwm_state_mtx);
    auto it = g_lwm_policy.find(lwm);
    if (wants_strict) {
        if (it == g_lwm_policy.end() || it->second != LwmPolicy::strict) {
            g_lwm_policy[lwm] = LwmPolicy::strict;
            lwm_policy_publish(lwm, LwmPolicy::strict);
            fprintf(stderr, "[lwm] strictly serializing critical lwmutex 0x%08X (first taken from lr=0x%08X)\n", lwm, lr);
        }
        return LwmPolicy::strict;
    }
    if (wants_timed) {
        if (it == g_lwm_policy.end()) {
            g_lwm_policy[lwm] = LwmPolicy::timed;
            lwm_policy_publish(lwm, LwmPolicy::timed);
            fprintf(stderr, "[lwm] serializing allocator lwmutex 0x%08X (first taken from lr=0x%08X)\n", lwm, lr);
            return LwmPolicy::timed;
        }
        lwm_policy_publish(lwm, it->second);
        return it->second;
    }
    return it == g_lwm_policy.end() ? LwmPolicy::none : it->second;
}

static void taiko_dump_service_table(void);

struct LwmProfileEntry {
    uint32_t lwm = 0, lr = 0, calls = 0, max_us = 0;
    uint64_t total_us = 0;
    char policy = '?';
};
static LwmProfileEntry g_lwm_profile[512];
static std::chrono::steady_clock::time_point g_lwm_profile_report_at;

static void lwm_profile_record(uint32_t lwm, uint32_t lr, char policy,
                               uint32_t elapsed_us)
{
    uint32_t slot = lwm_cache_hash(lwm, 511u);
    LwmProfileEntry* entry = nullptr;
    for (uint32_t n = 0; n < 512; ++n) {
        LwmProfileEntry& candidate = g_lwm_profile[(slot + n) & 511u];
        if (candidate.lwm == lwm || candidate.lwm == 0) {
            if (candidate.lwm == 0) candidate.lwm = lwm;
            entry = &candidate;
            break;
        }
    }
    if (entry) {
        entry->lr = lr;
        entry->policy = policy;
        ++entry->calls;
        entry->total_us += elapsed_us;
        if (elapsed_us > entry->max_us) entry->max_us = elapsed_us;
    }

    const auto now = std::chrono::steady_clock::now();
    if (g_lwm_profile_report_at.time_since_epoch().count() == 0)
        g_lwm_profile_report_at = now + std::chrono::seconds(5);
    if (now < g_lwm_profile_report_at) return;
    fprintf(stderr, "[LWMPROF] top main-thread waits over 5s:");
    for (unsigned rank = 0; rank < 10; ++rank) {
        LwmProfileEntry* best = nullptr;
        for (auto& candidate : g_lwm_profile)
            if (candidate.calls && !(candidate.calls & 0x80000000u) &&
                (!best || candidate.total_us > best->total_us))
                best = &candidate;
        if (!best) break;
        fprintf(stderr, " %08X@%08X%c=%u/%lluus max=%uus", best->lwm,
                best->lr, best->policy, best->calls,
                (unsigned long long)best->total_us, best->max_us);
        best->calls |= 0x80000000u;
    }
    fprintf(stderr, "\n");
    for (auto& candidate : g_lwm_profile) candidate = LwmProfileEntry{};
    g_lwm_profile_report_at = now + std::chrono::seconds(5);
}

static bool lwm_profile_enabled(uint32_t self_tid)
{
    static const bool enabled = getenv("PPU_LWM_PROFILE") != nullptr;
    return enabled && self_tid == 1;
}

static void lwm_lock_timed(uint32_t lwm, uint32_t self_tid, uint32_t self_lr)
{
    auto& m = lwm_host_mutex(lwm);
    const bool profile = lwm_profile_enabled(self_tid);
    const auto profile_start = profile ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
    if (m.try_lock_for(self_tid, self_lr,
                       std::chrono::milliseconds(lwm_wait_ms()))) {
        if (profile) {
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - profile_start).count();
            lwm_profile_record(lwm, self_lr, 'T', (uint32_t)us);
        }
        return;
    }
    uint32_t owner_tid = 0, owner_lr = 0;
    m.owner_snapshot(owner_tid, owner_lr);
    static int n = 0;
    if (n++ < 16)
        fprintf(stderr, "[lwm-timeout] lwmutex 0x%08X held >%ldms by tid=%u taken@lr=0x%08X; "
                        "waiter tid=%u lr=0x%08X -- proceeding UNSERIALIZED\n",
                lwm, lwm_wait_ms(), owner_tid, owner_lr, self_tid, self_lr);
    if (profile) {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - profile_start).count();
        lwm_profile_record(lwm, self_lr, 'X', (uint32_t)us);
    }
    taiko_dump_service_table();
}

static void lwm_lock_strict(uint32_t lwm, uint32_t self_tid, uint32_t self_lr)
{
    const bool profile = lwm_profile_enabled(self_tid);
    const auto profile_start = profile ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
    lwm_host_mutex(lwm).lock(self_tid, self_lr);
    if (profile) {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - profile_start).count();
        lwm_profile_record(lwm, self_lr, 'S', (uint32_t)us);
    }
}

/* Resolve the MUCHA service table once, the first time a lock times out (by
 * then every service object is constructed).
 *
 * func_001C9C34 is the shared body of all 8 service threads; per Ghidra it is a
 * thin dispatcher -- it takes no locks and allocates nothing itself:
 *
 *     base = *(u32*)0x0102F708;  slot = base + idx*0x18 + 0x5c;
 *     obj  = *slot;  (*obj->vtable[0x08])(obj, slot[1]);   // the service main loop
 *
 * So whichever service is holding the allocator lwmutex is holding it inside
 * its vtable[0x08]. Printing that per index names the guilty function directly
 * (resolve the address against meta/names.json / ghidra_out). Runtime rather
 * than static because the slots are populated during init, possibly via a
 * factory, so the constant is not necessarily visible in the disassembly. */
static void taiko_dump_service_table(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    uint32_t base = vm_read32(0x0102F708u);
    fprintf(stderr, "[svc] service table base=0x%08X (from *0x0102F708)\n", base);
    if (base < 0x10000u) return;
    static const char* kNames[9] = { "Resident", "Delay", "Priority", "Versionup",
                                     "MuchaMain", "USBAuthentication", "svc6", "svc7", "svc8" };
    for (uint32_t idx = 0; idx < 9; idx++) {
        uint32_t slot = base + idx * 0x18u + 0x5cu;
        uint32_t obj  = vm_read32(slot);
        uint32_t arg  = vm_read32(slot + 4);
        if (!obj) { fprintf(stderr, "[svc] %u %-18s obj=0 (not constructed)\n", idx, kNames[idx]); continue; }
        uint32_t vt   = vm_read32(obj);
        uint32_t opd  = vt ? vm_read32(vt + 8) : 0;      /* vtable[0x08] -> OPD */
        uint32_t code = opd ? vm_read32(opd) : 0;        /* OPD -> code address */
        fprintf(stderr, "[svc] %u %-18s obj=0x%08X arg=0x%08X vtable=0x%08X run=func_%08X\n",
                idx, kNames[idx], obj, arg, vt, code);
    }
    fflush(stderr);
}

static void sys_lwmutex_create(ppu_context* ctx)
{
    uint32_t lwm  = (uint32_t)ctx->gpr[3];
    uint32_t attr = (uint32_t)ctx->gpr[4];
    uint32_t protocol = attr ? vm_read32(attr + 0) : 0;
    vm_write32(lwm + 0x00, 0);          /* owner */
    vm_write32(lwm + 0x04, 0);          /* waiter */
    vm_write32(lwm + LWM_ATTR, protocol);
    vm_write32(lwm + LWM_RECUR, 0);     /* recursive_count */
    vm_write32(lwm + 0x10, 0);          /* sleep_queue */
    vm_write32(lwm + 0x14, 0);
    ctx->gpr[3] = 0;
}
static void sys_lwmutex_lock(ppu_context* ctx)
{
    uint32_t lwm = (uint32_t)ctx->gpr[3];
    lwm_trace_first(lwm, (uint32_t)ctx->lr, lwm_self(ctx));
    if (ydkj_reallwm()) {
        const uint32_t lr = (uint32_t)ctx->lr;
        const LwmPolicy policy = lwm_policy(lwm, lr);
        if (policy == LwmPolicy::strict) lwm_lock_strict(lwm, lwm_self(ctx), lr);
        else if (policy == LwmPolicy::timed) lwm_lock_timed(lwm, lwm_self(ctx), lr);
    }
    vm_write32(lwm + LWM_OWNER, lwm_self(ctx));
    vm_write32(lwm + LWM_RECUR, vm_read32(lwm + LWM_RECUR) + 1);
    ctx->gpr[3] = 0;   /* CELL_OK */
}
static void sys_lwmutex_trylock(ppu_context* ctx)
{
    uint32_t lwm = (uint32_t)ctx->gpr[3];
    if (ydkj_reallwm() && lwm_policy(lwm, (uint32_t)ctx->lr) != LwmPolicy::none) {
        if (!lwm_host_mutex(lwm).try_lock(lwm_self(ctx), (uint32_t)ctx->lr)) {
            ctx->gpr[3] = 0x80010005u;
            return;
        } /* EBUSY */
    }
    vm_write32(lwm + LWM_OWNER, lwm_self(ctx));
    vm_write32(lwm + LWM_RECUR, vm_read32(lwm + LWM_RECUR) + 1);
    ctx->gpr[3] = 0;
}
static void sys_lwmutex_unlock(ppu_context* ctx)
{
    uint32_t lwm = (uint32_t)ctx->gpr[3];
    uint32_t rc = vm_read32(lwm + LWM_RECUR);
    if (rc) vm_write32(lwm + LWM_RECUR, rc - 1);
    if (rc <= 1) vm_write32(lwm + LWM_OWNER, LWM_FREE);
    /* The host mutex itself verifies guest-thread ownership.  Thus a timed-out
     * acquire can still execute this path safely without a redundant held-lock
     * table or global owner-map mutex on every lock pair. */
    const uint32_t self_tid = lwm_self(ctx);
    if (ydkj_reallwm() && lwm_policy_cached(lwm) != LwmPolicy::none)
        lwm_host_mutex(lwm).unlock(self_tid);
    ctx->gpr[3] = 0;
}

/* sys_lwcond (sysPrxForUser) — guest-side condition variable, paired with an
 * lwmutex. The CRT and (newly) libsre's cellSpurs create/wait/signal these. Like
 * sys_lwmutex above, model it directly in guest memory so the args stay GUEST
 * EAs (the generic adapter would pass them raw and the C sysPrxForUser impl
 * deref'd them as host pointers -> AV during cellSpurs init). A no-op wait is
 * adequate here: the CRT/SPURS paths that reach us use these for one-shot init
 * handshakes, not long-term blocking. sys_lwcond_t: +0x00 lwmutex EA (be64),
 * +0x08 lwcond_queue id. */
static void sys_lwcond_create(ppu_context* ctx)
{
    static uint32_t s_lwcond_id = 0x4C000000u;
    uint32_t lwcond  = (uint32_t)ctx->gpr[3];
    uint32_t lwmutex = (uint32_t)ctx->gpr[4];
    vm_write64(lwcond + 0x00, (uint64_t)lwmutex);
    vm_write32(lwcond + 0x08, ++s_lwcond_id);
    if (ydkj_reallwm()) {
        std::lock_guard<std::mutex> g(g_lwm_map_mtx);
        LwCondHost& c = g_lwc_conds[lwcond];
        std::lock_guard<std::mutex> cg(c.gate);
        c.lwmutex = lwmutex;
        c.sequence = 0;
    }
    ctx->gpr[3] = 0;
}
static LwCondHost& lwc_host_cond(uint32_t lwcond)
{
    std::lock_guard<std::mutex> g(g_lwm_map_mtx);
    return g_lwc_conds[lwcond];
}
static void sys_lwcond_destroy(ppu_context* ctx)
{
    /* Keep the host node allocated: a waiter can still hold a reference while
     * guest teardown races it.  Reusing the same guest EA resets it in create. */
    ctx->gpr[3] = 0;
}
static void sys_lwcond_signal(ppu_context* ctx)
{
    if (ydkj_reallwm()) {
        LwCondHost& c = lwc_host_cond((uint32_t)ctx->gpr[3]);
        { std::lock_guard<std::mutex> g(c.gate); ++c.sequence; }
        c.cv.notify_one();
    }
    ctx->gpr[3] = 0;
}
static void sys_lwcond_signal_all(ppu_context* ctx)
{
    if (ydkj_reallwm()) {
        LwCondHost& c = lwc_host_cond((uint32_t)ctx->gpr[3]);
        { std::lock_guard<std::mutex> g(c.gate); ++c.sequence; }
        c.cv.notify_all();
    }
    ctx->gpr[3] = 0;
}
static void sys_lwcond_signal_to(ppu_context* ctx) { sys_lwcond_signal(ctx); }
static void sys_lwcond_wait(ppu_context* ctx)
{
    if (!ydkj_reallwm()) { ctx->gpr[3] = 0; return; }

    uint32_t lwcond = (uint32_t)ctx->gpr[3];
    uint64_t timeout = ctx->gpr[4]; /* PS3 timeout is in microseconds; 0 = infinite. */
    LwCondHost& c = lwc_host_cond(lwcond);
    uint32_t lwm = c.lwmutex;
    FairRecursiveTimedMutex& m = lwm_host_mutex(lwm);

    /* Serialize the sequence snapshot with signal, then completely release
     * the recursive lwmutex.  A separate gate makes the release+sleep atomic
     * with respect to notifications even when the guest held the mutex more
     * than once (condition_variable_any can only release it once). */
    std::unique_lock<std::mutex> gate(c.gate);
    uint64_t before = c.sequence;
    /* Drop exactly what THIS thread holds, not what the guest's recursion count
     * claims: a timed-out acquire left the guest count incremented without the
     * host lock, so trusting LWM_RECUR here would unlock a mutex we don't own. */
    const uint32_t self_tid = lwm_self(ctx);
    int depth = m.release_all(self_tid);
    vm_write32(lwm + LWM_RECUR, 0);
    vm_write32(lwm + LWM_OWNER, 0);

    /* An infinite guest wait is bounded here for the same reason the acquire is:
     * if the signal it waits for comes from a path we do not emulate, an
     * untimed wait never returns. Guests treat ETIMEDOUT as retry-able, so
     * capping turns a hang into a slow loop that keeps making progress. */
    bool woke;
    if (timeout == 0)
        woke = c.cv.wait_for(gate, std::chrono::milliseconds(lwm_wait_ms() * 10),
                             [&] { return c.sequence != before; });
    else
        woke = c.cv.wait_for(gate, std::chrono::microseconds(timeout),
                             [&] { return c.sequence != before; });
    gate.unlock();

    for (int i = 0; i < depth; ++i) lwm_lock_timed(lwm, lwm_self(ctx), (uint32_t)ctx->lr);
    vm_write32(lwm + LWM_OWNER, lwm_self(ctx));
    vm_write32(lwm + LWM_RECUR, (uint32_t)depth);
    ctx->gpr[3] = woke ? 0 : 0x8001000Bu; /* CELL_ETIMEDOUT */
}

/* sys_ppu_thread_get_id(vm::ptr<u64> id). The initial context carries internal
 * ID zero but is guest-visible as reserved ID 1; workers already carry their
 * guest-visible IDs (2+). Returning 1 for every context made the CRT _Mtxlock
 * treat unrelated workers as recursive acquisitions by one owner. */
static void sys_ppu_thread_get_id(ppu_context* ctx)
{
    uint32_t p = (uint32_t)ctx->gpr[3];
    if (p) vm_write64(p, lwm_self(ctx));
    ctx->gpr[3] = 0;
}

/* Fast entry for the CRT primitives that dominate Taiko's normal frame loop.
 * At attract the initial guest thread calls lock/unlock about 110,000 times per
 * second and get_id another 34,000 times per second. Routing each through the
 * full generic HLE/PRX/diagnostic resolver costs far more than the uncontended
 * operation itself. The public HLE bridge calls this only in its normal mode;
 * diagnostic environments retain the full slow path. */
extern "C" int ppu_sysprx_try_hot_hle(uint32_t nid, ppu_context* ctx)
{
    switch (nid) {
    case 0x1573DC3Fu: sys_lwmutex_lock(ctx); return 1;
    case 0x1BC200F4u: sys_lwmutex_unlock(ctx); return 1;
    case 0x350D454Eu: sys_ppu_thread_get_id(ctx); return 1;
    default: return 0;
    }
}

/* sys_mmapper_allocate_memory(u32 size, u64 flags, vm::ptr<u32> mem_id) ->
 * hand back a unique opaque id; the backing is the flat VM, so the later
 * search_and_map just needs a non-zero id to track. */
/* id -> size so sys_mmapper_search_and_map (lv2 337) can lay blocks out
 * without overlap. Ids are dense from 0x1000. */
static uint32_t s_mm_sizes[256];
static uint32_t s_mmapper_next_id = 0x1000;
extern "C" uint32_t ps3_mmapper_block_size(uint32_t mem_id)
{
    uint32_t i = mem_id - 0x1000u;
    return (i < 256) ? s_mm_sizes[i] : 0;
}

static uint32_t mmapper_new_id(uint32_t size)
{
    uint32_t id = s_mmapper_next_id++;
    if (id - 0x1000u < 256) s_mm_sizes[id - 0x1000u] = size;
    return id;
}

static void sys_mmapper_allocate_memory(ppu_context* ctx)
{
    uint32_t size       = (uint32_t)ctx->gpr[3];
    uint32_t mem_id_ptr = (uint32_t)ctx->gpr[5];
    uint32_t id         = mmapper_new_id(size);
    if (getenv("FLOW_MEMTRACE"))
        fprintf(stderr, "[mmapper] allocate_memory(size=0x%X flags=0x%llX id_ptr=0x%X) -> id 0x%X\n",
                size, (unsigned long long)ctx->gpr[4], mem_id_ptr, id);
    if (mem_id_ptr) vm_write32(mem_id_ptr, id);
    ctx->gpr[3] = 0;
}
/* sys_mmapper_allocate_memory_from_container(u32 size, u32 container, u64 flags,
 * vm::ptr<u32> mem_id) -> id in *r6. flОw's CRT uses this for its heap/mutex pool;
 * it was previously UNregistered (CRT saw failure -> "not enough memory"). */
static void sys_mmapper_allocate_memory_from_container(ppu_context* ctx)
{
    uint32_t size = (uint32_t)ctx->gpr[3];
    uint32_t mem_id_ptr = (uint32_t)ctx->gpr[6];
    uint32_t id = mmapper_new_id(size);
    if (getenv("FLOW_MEMTRACE"))
        fprintf(stderr, "[mmapper] alloc_from_container(size=0x%X cid=0x%X flags=0x%llX id_ptr=0x%X) -> id 0x%X\n",
                size, (uint32_t)ctx->gpr[4], (unsigned long long)ctx->gpr[5], mem_id_ptr, id);
    if (mem_id_ptr) vm_write32(mem_id_ptr, id);
    ctx->gpr[3] = 0;
}

/* A handful of CRT helpers the early boot tends to hit; accept and continue. */
static void crt_ok(ppu_context* ctx) { ctx->gpr[3] = 0; }

/* Real preemptive thread create/exit live in the lv2 syscall layer
 * (syscalls/sys_ppu_thread.c) and spawn a host thread that runs the guest
 * entry through the recompiled code. The CRT also reaches them as
 * sysPrxForUser import NIDs (gen_hle_nids can't see them — they're not defined
 * in the sysPrxForUser lib), so bridge the NIDs to the same implementation.
 * Without this the CRT's thread/static-init runs through an uninitialised
 * object table and calls heap addresses as function pointers. */
extern "C" int64_t sys_ppu_thread_create(ppu_context* ctx);
extern "C" int64_t sys_ppu_thread_exit(ppu_context* ctx);
/* Must write the int64 result into r3 -- the game checks it (0 == CELL_OK).
 * Dropping it left r3 = the tid_out arg, read as a nonzero "create failed"
 * (flОw's PSSGSPUPrintfServerInitialize aborted PhyreEngine init on this). */
static void hle_ppu_thread_create(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)sys_ppu_thread_create(ctx); }
static void hle_ppu_thread_exit(ppu_context* ctx)   { sys_ppu_thread_exit(ctx); }

/* _cellGcmInitBody (NID 0x15BAE46B) -- the GCM init every PS3 game calls via the
 * cellGcmInit() SDK macro. cellGcmSys.c provides the layout-correct core
 * (cellGcmSetupContext) but needs the owning vm to allocate the guest
 * CellGcmContextData and write the game's context-out pointer; supply those as
 * callbacks. Without this the game's GCM context stays null -> null deref. */
typedef unsigned int (*CellGcmGuestAlloc)(unsigned int, unsigned int);
typedef void (*CellGcmGuestWrite32)(unsigned int, unsigned int);
extern "C" unsigned int cellGcmSetupContext(unsigned int ctx_out_addr,
    unsigned int cmdSize, unsigned int ioSize, unsigned int ioAddress,
    CellGcmGuestAlloc galloc, CellGcmGuestWrite32 gwrite32);

static unsigned int gcm_guest_alloc(unsigned int size, unsigned int align)
{
    /* Bump from a small scratch region below the main stack (0x0FF00000) and
     * above the TLS image -- a few control structs, never freed. */
    static unsigned int bump = 0x0F800000u;
    if (align < 16) align = 16;
    bump = (bump + align - 1) & ~(align - 1);
    unsigned int a = bump;
    bump += (size + 15u) & ~15u;
    return a;
}
static void gcm_guest_write32(unsigned int addr, unsigned int val) { vm_write32(addr, val); }

/* FIFO command-buffer-full callback. cellGcmSetupContext points the guest
 * context's callback OPD at GCM_FIFO_CALLBACK_SENTINEL_EA; the title's inline
 * gcmReserve calls context->callback(context, count) on ring wrap, which the
 * indirect dispatcher routes here. r3 = guest context EA. Must match the sentinel
 * define in libs/video/cellGcmSys.c. */
#define GCM_FIFO_CALLBACK_SENTINEL_EA 0x03002F00u
extern "C" void cellGcm_fifo_recycle(unsigned int ctx_ea);
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
static void hle_gcm_callback(ppu_context* ctx)
{
    cellGcm_fifo_recycle((unsigned int)ctx->gpr[3]);   /* r3 = context EA */
    ctx->gpr[3] = 0;                                   /* CELL_OK */
}

static void hle_cellGcmInitBody(ppu_context* ctx)
{
    uint32_t ctx_out = (uint32_t)ctx->gpr[3];
    uint32_t cmdSize = (uint32_t)ctx->gpr[4];
    uint32_t ioSize  = (uint32_t)ctx->gpr[5];
    uint32_t ioAddr  = (uint32_t)ctx->gpr[6];
    fprintf(stderr, "[HLE] _cellGcmInitBody(ctx_out=0x%08X, cmdSize=0x%X, ioSize=0x%X, ioAddr=0x%X)\n",
            ctx_out, cmdSize, ioSize, ioAddr);
    cellGcmSetupContext(ctx_out, cmdSize, ioSize, ioAddr, gcm_guest_alloc, gcm_guest_write32);
    ctx->gpr[3] = 0;   /* CELL_OK */
}

/* _sys_spu_image_import (sysPrxForUser NID 0xEBE5F72F) -- the user-space wrapper libsre
 * uses to parse the SPURS-kernel SPU ELF into a sys_spu_image (entry+segs) WITHOUT the
 * syscall. Ported from D:/recomp/ps3. Without it the NID is unresolved -> returns 0 ->
 * the SPU image is never parsed -> the 5 cellSpurs SPU threads come up with a garbage
 * entry (e.g. 0x5B555253) instead of the real kernel entry (0x818) -> SPURS never
 * bootstraps -> CellSpurs instance @0x40009F00 stays empty -> menu SPU work stalls. */
static void hle_sys_spu_image_import(ppu_context* ctx)
{
    uint32_t img_ea = (uint32_t)ctx->gpr[3];
    uint32_t src_ea = (uint32_t)ctx->gpr[4];
    fprintf(stderr, "[HLE] _sys_spu_image_import(img=0x%08X src=0x%08X r5=0x%08X r6=0x%08X)\n",
            img_ea, src_ea, (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6]);
    if (!img_ea || !src_ea || !vm_base) { ctx->gpr[3] = 0; return; }
    const uint8_t* e = vm_base + src_ea;
    if (!(e[0]==0x7F && e[1]=='E' && e[2]=='L' && e[3]=='F')) {
        fprintf(stderr, "[HLE] _sys_spu_image_import: src not an ELF -> no-op\n");
        fflush(stderr); ctx->gpr[3] = 0; return;
    }
    uint16_t machine = (uint16_t)((e[0x12] << 8) | e[0x13]);   /* 23 = SPU */
    uint32_t entry   = vm_read32(src_ea + 0x18);
    uint32_t phoff   = vm_read32(src_ea + 0x1C);
    uint16_t phentsz = (uint16_t)((e[0x2A] << 8) | e[0x2B]); if (!phentsz) phentsz = 0x20;
    uint16_t phnum   = (uint16_t)((e[0x2C] << 8) | e[0x2D]);
    static uint32_t s_seg_bump = 0x0D000000u;
    uint32_t segs_ea = s_seg_bump; int nsegs = 0;
    for (uint16_t i = 0; i < phnum && nsegs < 32; i++) {
        uint32_t ph = phoff + (uint32_t)i * phentsz;
        if (vm_read32(src_ea + ph + 0x00) != 1) continue;      /* PT_LOAD */
        uint32_t p_off = vm_read32(src_ea + ph + 0x04);
        uint32_t p_va  = vm_read32(src_ea + ph + 0x08);
        uint32_t p_fsz = vm_read32(src_ea + ph + 0x10);
        uint32_t p_msz = vm_read32(src_ea + ph + 0x14);
        uint32_t seg = segs_ea + (uint32_t)nsegs * 0x18;       /* COPY */
        vm_write32(seg + 0x00, 1); vm_write32(seg + 0x04, p_va);
        vm_write32(seg + 0x08, p_fsz); vm_write32(seg + 0x10, 0);
        vm_write32(seg + 0x14, src_ea + p_off); nsegs++;
        if (p_msz > p_fsz && nsegs < 32) {                     /* BSS tail -> FILL 0 */
            seg = segs_ea + (uint32_t)nsegs * 0x18;
            vm_write32(seg + 0x00, 2); vm_write32(seg + 0x04, p_va + p_fsz);
            vm_write32(seg + 0x08, p_msz - p_fsz);
            vm_write32(seg + 0x10, 0); vm_write32(seg + 0x14, 0); nsegs++;
        }
    }
    s_seg_bump += (uint32_t)nsegs * 0x18;
    if (s_seg_bump >= 0x0E000000u) s_seg_bump = 0x0D000000u;
    vm_write32(img_ea + 0x00, 0);                              /* type = USER */
    vm_write32(img_ea + 0x04, entry);
    vm_write32(img_ea + 0x08, nsegs ? segs_ea : 0);
    vm_write32(img_ea + 0x0C, (uint32_t)nsegs);
    fprintf(stderr, "[HLE] _sys_spu_image_import -> entry=0x%05X nsegs=%d machine=%u (SPU=23)\n",
            entry, nsegs, machine);
    fflush(stderr);
    ctx->gpr[3] = 0;
}

/* Diagnostic: libsre's internal assert/error path. _sys_printf(0x9F04F7AF) and
 * the abort NID 0x9FB6228E are both currently unresolved no-ops, so libsre's
 * failure message is swallowed and the SPURS group gets torn down blind. Read
 * the guest strings so we can see WHAT libsre is asserting on. */
static void hle_dbg_read_gstr(uint32_t p, char* buf, int cap) {
    int i = 0; if (p && vm_base) for (; i < cap-1; i++) { uint8_t c = vm_base[p+i]; if (!c) break; buf[i] = (char)c; }
    buf[i] = 0;
}
static void hle_dbg_sys_printf(ppu_context* ctx) {
    char fmt[192], s5[160], s6[160];
    hle_dbg_read_gstr((uint32_t)ctx->gpr[3], fmt, sizeof fmt);
    hle_dbg_read_gstr((uint32_t)ctx->gpr[5], s5, sizeof s5);
    hle_dbg_read_gstr((uint32_t)ctx->gpr[6], s6, sizeof s6);
    fprintf(stderr, "[libsre-printf] fmt=\"%s\" | r4=0x%llX r5-str=\"%s\" r6-str=\"%s\" r7=%lld r8=0x%llX\n",
            fmt, (unsigned long long)ctx->gpr[4], s5, s6,
            (long long)(int32_t)ctx->gpr[7], (unsigned long long)ctx->gpr[8]);
    fflush(stderr); ctx->gpr[3] = 0;
}
static void hle_dbg_abort_9FB6(ppu_context* ctx) {
    char s3[192];
    hle_dbg_read_gstr((uint32_t)ctx->gpr[3], s3, sizeof s3);
    fprintf(stderr, "[libsre-abort 0x9FB6228E] r3-str=\"%s\" r3=0x%08X r4=0x%llX lr=0x%08X\n",
            s3, (uint32_t)ctx->gpr[3], (unsigned long long)ctx->gpr[4], (uint32_t)ctx->lr);
    fflush(stderr); ctx->gpr[3] = 0;
}

extern "C" void ppu_sysprx_register(void)
{
    if (getenv("YDKJ_GFXSCAN")) {
        ps3_hle_register_ctx(0x9F04F7AFu, "_sys_printf(dbg)", hle_dbg_sys_printf);
        ps3_hle_register_ctx(0x9FB6228Eu, "libsre_abort(dbg)", hle_dbg_abort_9FB6);
    }
    ps3_hle_register_ctx(0x15BAE46Bu, "_cellGcmInitBody", hle_cellGcmInitBody);
    ps3_hle_register_ctx(0xEBE5F72Fu, "_sys_spu_image_import", hle_sys_spu_image_import);
    /* Route the GCM command-buffer-full callback (invoked indirectly via the
     * context OPD) into cellGcm_fifo_recycle so the FIFO ring recycles on wrap. */
    ppu_register_function(GCM_FIFO_CALLBACK_SENTINEL_EA, hle_gcm_callback);
    ps3_hle_register_ctx(ps3_compute_nid("sys_initialize_tls"),       "sys_initialize_tls",       sys_initialize_tls);
    ps3_hle_register_ctx(ps3_compute_nid("sys_time_get_system_time"), "sys_time_get_system_time", sys_time_get_system_time);
    ps3_hle_register_ctx(ps3_compute_nid("sys_process_is_stack"),     "sys_process_is_stack",     sys_process_is_stack);
    /* Atexit registration: nothing to do at boot, just succeed. */
    ps3_hle_register_ctx(ps3_compute_nid("_sys_process_atexitspawn"), "_sys_process_atexitspawn", crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("_sys_process_at_Exitspawn"),"_sys_process_at_Exitspawn",crt_ok);

    /* Publishes the caller's own export table so loaded PRXs can import from it.
     * A statically recompiled title resolves every call at link time, so there
     * is nothing to publish -- but it must still return CELL_OK: callers treat a
     * non-zero result as "registration failed" and carry on with an export
     * descriptor they never filled in, which later gets called as an OPD. */
    ps3_hle_register_ctx(ps3_compute_nid("sys_prx_register_library"), "sys_prx_register_library", crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_once"), "sys_ppu_thread_once", sys_ppu_thread_once);

    /* Lightweight mutex family (guards global/singleton init in the CRT). */
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_create"),  "sys_lwmutex_create",  sys_lwmutex_create);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_destroy"), "sys_lwmutex_destroy", crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_lock"),    "sys_lwmutex_lock",    sys_lwmutex_lock);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_unlock"),  "sys_lwmutex_unlock",  sys_lwmutex_unlock);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_trylock"), "sys_lwmutex_trylock", sys_lwmutex_trylock);

    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_create"),     "sys_lwcond_create",     sys_lwcond_create);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_destroy"),    "sys_lwcond_destroy",    sys_lwcond_destroy);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_signal"),     "sys_lwcond_signal",     sys_lwcond_signal);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_signal_all"), "sys_lwcond_signal_all", sys_lwcond_signal_all);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_signal_to"),  "sys_lwcond_signal_to",  sys_lwcond_signal_to);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_wait"),       "sys_lwcond_wait",       sys_lwcond_wait);

    /* Thread id + memory manager (high-frequency boot imports). The flat VM
     * means map/unmap/free are no-ops: the memory already exists everywhere. */
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_get_id"),      "sys_ppu_thread_get_id",      sys_ppu_thread_get_id);
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_create"),      "sys_ppu_thread_create",      hle_ppu_thread_create);
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_exit"),        "sys_ppu_thread_exit",        hle_ppu_thread_exit);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_allocate_memory"), "sys_mmapper_allocate_memory", sys_mmapper_allocate_memory);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_allocate_memory_from_container"), "sys_mmapper_allocate_memory_from_container", sys_mmapper_allocate_memory_from_container);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_map_memory"),     "sys_mmapper_map_memory",     crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_unmap_memory"),   "sys_mmapper_unmap_memory",   crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_free_memory"),    "sys_mmapper_free_memory",    crt_ok);
}

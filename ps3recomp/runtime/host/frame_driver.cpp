#include <ps3emu/host_platform.h>

#include "rsx_commands.h"
#include "rsx_null_backend.h"
#include "rsx_sdl_gpu_backend.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

extern "C" void cellGcmTickVBlank(void);
extern "C" void cellGcmTickFlip(void);
extern "C" int cellGcm_take_flip_pending(void);
extern "C" int cellGcm_flip_is_pending(void);
extern "C" void cellGcm_rsx_process_fifo(void);
extern "C" unsigned cellGcm_flip_request_count(void);
extern "C" int sys_event_queue_inject(unsigned, unsigned long long,
                                        unsigned long long, unsigned long long,
                                        unsigned long long);

namespace {

std::atomic<bool> s_frame_stop{false};
#ifdef _WIN32
HANDLE s_frame_thread = nullptr;
#else
pthread_t s_frame_thread{};
bool s_frame_thread_started = false;
#endif

bool enabled(const char* name)
{
    const char* value = std::getenv(name);
    return value && value[0] != '0';
}

unsigned env_hz(const char* name, unsigned fallback)
{
    const char* value = std::getenv(name);
    if (!value) return fallback;
    int v = std::atoi(value);
    return (v >= 10 && v <= 1000) ? (unsigned)v : fallback;
}

/* Set once the title starts its attract audio; ends the boot fast-forward. */
std::atomic<bool> s_boot_fast_done{false};

void present_pending()
{
    if (!cellGcm_take_flip_pending()) return;
    rsx_backend* backend = rsx_get_backend();
    if (backend && backend->present)
        backend->present(backend->userdata, 0);
}

void run_frame_driver()
{
    ps3_host_apply_thread_affinity("TAIKO_CPU_FRAME_AFFINITY", "frame/FIFO");
#ifdef _WIN32
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST))
        std::fprintf(stderr, "[rsx] warning: failed to raise vblank priority (%lu)\n",
                     (unsigned long)GetLastError());
#endif

#ifdef PS3RECOMP_RSX_BACKEND_NULL
    const bool null_rsx = true;
#else
    const bool null_rsx = enabled("PS3RECOMP_NULL_RSX");
#endif
#ifdef PS3RECOMP_RSX_BACKEND_SDL_GPU
    const bool sdl_rsx = !null_rsx;
#else
    const bool sdl_rsx = false;
#endif
    int init_result;
    init_result = null_rsx
        ? rsx_null_backend_init(1280, 720, "Taiko no Tatsujin (ps3recomp)")
        : 0; /* SDL and the recorder are already initialized on the main thread. */
    const bool rsx_ok = init_result == 0;
    std::fprintf(stderr, "[rsx] %s backend init %s\n",
                 null_rsx ? "null" : "SDL_GPU",
                 rsx_ok ? "OK" : "FAILED");

    constexpr uint64_t kFifoPeriodNs = 4000000u;
    /* The title's boot -- arcade system checks, the chassis service sequence and
     * the asset load -- is paced by the guest's per-frame state machine, not by
     * the network or by disk. Ticking vblank faster than 60 Hz until the game
     * starts its attract music shortens boot proportionally (measured: 30 s of
     * online checks -> 6 s at 240 Hz, and asset loading starts at 6 s instead of
     * 22 s). Gameplay must run at 60 Hz, so the rate reverts on the first ATRAC
     * decode (ps3_frame_boot_fast_finish) or at the deadline below.
     *   TAIKO_VBLANK_HZ      -- normal rate, default 60
     *   TAIKO_BOOT_VBLANK_HZ -- boot rate, default 240; set to 60 to disable */
    unsigned vblank_hz = env_hz("TAIKO_VBLANK_HZ", 60);
    unsigned boot_hz = env_hz("TAIKO_BOOT_VBLANK_HZ", 240);
    if (boot_hz < vblank_hz) boot_hz = vblank_hz;
    const uint64_t kVblankPeriodNs = 1000000000ull / vblank_hz;
    const uint64_t kBootVblankPeriodNs = 1000000000ull / boot_hz;
    /* ponytail: hard deadline so a boot that never reaches attract (no audio,
     * failed online) cannot leave the guest clock overclocked forever. */
    const uint64_t kBootFastDeadlineNs = ps3_host_monotonic_ns() + 180000000000ull;
    bool boot_fast = boot_hz > vblank_hz;
    if (boot_fast)
        std::fprintf(stderr, "[frame] boot fast-forward at %u Hz (play rate %u Hz)\n",
                     boot_hz, vblank_hz);
    uint64_t now = ps3_host_monotonic_ns();
    uint64_t next_fifo = now + kFifoPeriodNs;
    uint64_t next_vblank = now;
    uint64_t next_report = now + 5000000000u;
    uint64_t vblanks = 0, fifo_drains = 0;

    while (!s_frame_stop.load(std::memory_order_acquire)) {
        /* Vblank and FIFO polling have different clocks. Sleeping only to the
         * 4 ms FIFO deadline quantized the nominal 16.667 ms vblank cadence to
         * alternating 16/20 ms intervals. Average FPS still read as 60, but
         * scrolling notes visibly juddered and a late submission could miss a
         * host VSync. Wake at the earlier deadline so vblank is not rounded to
         * the FIFO grid; retain the independent 4 ms drain cadence. */
        if (boot_fast &&
            (s_boot_fast_done.load(std::memory_order_acquire) ||
             ps3_host_monotonic_ns() >= kBootFastDeadlineNs)) {
            boot_fast = false;
            std::fprintf(stderr, "[frame] boot fast-forward off, back to %u Hz\n",
                         vblank_hz);
        }
        const uint64_t vblank_period = boot_fast ? kBootVblankPeriodNs
                                                 : kVblankPeriodNs;
        const uint64_t next_wake = next_vblank < next_fifo
            ? next_vblank : next_fifo;
        ps3_host_sleep_until_ns(next_wake);
        now = ps3_host_monotonic_ns();
        const bool fifo_due = now >= next_fifo;
        if (fifo_due) {
            if (now >= next_fifo + kFifoPeriodNs)
                next_fifo = now + kFifoPeriodNs;
            else
                next_fifo += kFifoPeriodNs;
        }

        unsigned fired = 0;
        while (now >= next_vblank && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            ++vblanks;
            ++fired;
            const bool queue_ready = !sdl_rsx ||
                rsx_sdl_gpu_backend_queue_has_capacity();
            if (rsx_ok && queue_ready) {
                present_pending();
                cellGcm_rsx_process_fifo();
                ++fifo_drains;
            }
            next_vblank += vblank_period;
        }
        if (fired == 240)
            next_vblank = now + vblank_period;

        if (fifo_due) {
            const bool queue_ready = !sdl_rsx ||
                rsx_sdl_gpu_backend_queue_has_capacity();
            if (rsx_ok && queue_ready && !cellGcm_flip_is_pending()) {
                /* A flip seals the current recorder batch. Leave it untouched
                 * until the exact vblank path above presents it; presenting
                 * here rounded frame delivery to this 4 ms polling grid. */
                cellGcm_rsx_process_fifo();
                ++fifo_drains;
            }
        }

        if (std::getenv("YDKJ_INJECT_Q3")) {
            static unsigned tick;
            const char* text = std::getenv("YDKJ_INJECT_Q3");
            uint32_t queue = (uint32_t)std::strtoul(text, nullptr, 0);
            if (queue <= 1) queue = 3;
            if (++tick % 8 == 0)
                sys_event_queue_inject(queue, 0x1234, 0, 0, 0);
        }

        if (null_rsx && now >= next_report) {
            uint64_t clears = 0, draws = 0, textures = 0, presents = 0;
            if (null_rsx)
                rsx_null_backend_get_counters(&clears, &draws, &textures,
                                               &presents);
            std::fprintf(stderr,
                         "[HEADLESS-PROGRESS] vblank=%llu fifo=%llu flips=%u "
                         "clears=%llu draws=%llu textures=%llu presents=%llu\n",
                         (unsigned long long)vblanks,
                         (unsigned long long)fifo_drains,
                         cellGcm_flip_request_count(),
                         (unsigned long long)clears,
                         (unsigned long long)draws,
                         (unsigned long long)textures,
                         (unsigned long long)presents);
            next_report = now + 5000000000u;
        }
    }

    if (null_rsx)
        rsx_null_backend_shutdown();
}

#ifdef _WIN32
DWORD WINAPI frame_thread(void*) { run_frame_driver(); return 0; }
#else
void* frame_thread(void*) { run_frame_driver(); return nullptr; }
#endif

} // namespace

/* Called by the title layer when the game starts its attract audio, which is
 * the point the boot state machine has finished. Safe to call repeatedly. */
extern "C" void ps3_frame_boot_fast_finish(void)
{
    s_boot_fast_done.store(true, std::memory_order_release);
}

extern "C" int ps3_frame_boot_fast_is_done(void)
{
    return s_boot_fast_done.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" int ps3recomp_start_frame_driver(void)
{
    s_frame_stop.store(false, std::memory_order_release);
#ifdef _WIN32
    s_frame_thread = CreateThread(NULL, 4u * 1024 * 1024, frame_thread,
                                  NULL, 0, NULL);
    if (!s_frame_thread) return -1;
    return 0;
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4u * 1024 * 1024);
    int rc = pthread_create(&s_frame_thread, &attr, frame_thread, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) return -1;
    s_frame_thread_started = true;
    return 0;
#endif
}

extern "C" void ps3recomp_stop_frame_driver(void)
{
    s_frame_stop.store(true, std::memory_order_release);
}

extern "C" void ps3recomp_join_frame_driver(void)
{
#ifdef _WIN32
    if (s_frame_thread) {
        WaitForSingleObject(s_frame_thread, INFINITE);
        CloseHandle(s_frame_thread);
        s_frame_thread = nullptr;
    }
#else
    if (s_frame_thread_started) {
        pthread_join(s_frame_thread, nullptr);
        s_frame_thread_started = false;
    }
#endif
}

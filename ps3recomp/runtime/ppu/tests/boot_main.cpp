/*
 * ps3recomp - integrated PPU boot harness (first-boot attempt).
 *
 * Links the whole PPU runtime half into one executable and starts executing
 * the recompiled game's entry point:
 *
 *   lifted code (ppu_recomp.c) + loader (ppu_loader.cpp) + HLE bridge
 *   (ppu_hle.cpp + generated NID table) + HLE libs (cellGcmSys, rsx_commands)
 *
 * It loads the real EBOOT image, registers the lifted functions and the HLE
 * NID handlers, then dispatches the entry. Execution runs real Uncharted boot
 * code until it reaches a function outside the lifted subset (logged by the
 * unlifted stub), an unimplemented firmware import (logged by ps3_hle_call),
 * or an lv2 syscall (logged by lv2_syscall) -- telling us exactly what to
 * implement next.
 *
 * This proves the integration builds + runs; a full-image build additionally
 * needs the lifter to split output into multiple TUs (88 MB single-file
 * otherwise).
 */
#include "ppu_recomp.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" {
uint32_t ppu_load_elf(const char* path);
uint32_t ppu_load_embedded_image(const void* data, size_t size);
void     ppu_recomp_register(void);
void     ppu_hle_init(void);
void     ppu_sysprx_register(void);
void     ppu_fs_register(void);
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);
extern const char* ppu_vfs_root;   /* host dir that PS3 mount points map into */
/* Optional hook: load real system PRX modules (libsre = cellSpurs/cellSync) into
 * guest RAM and register their exports. Weak default is a no-op; a title that
 * links a lifted PRX defines a strong version. Called after the lifted function
 * table is registered and vm_base is live, before the game runs. */
void     ps3_load_prx_modules(void) __attribute__((weak));
void     ps3_load_prx_modules(void) {}
}

#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <atomic>
#include <cstdio>
#include <ps3emu/host_platform.h>
#include <ps3emu/host_sdl.h>
#ifdef _WIN32
#include <algorithm>
#include <unordered_map>
#include <vector>
#else
#include <pthread.h>
#endif

#ifdef _WIN32
#include <windows.h>
extern "C" void vm_note_accessible_range(uint32_t addr, uint32_t size,
                                           int accessible);
/* Last-chance crash reporter: vm_base accesses are bounds-guarded, so a real
 * access violation means a HOST pointer deref (e.g. a bad function pointer or a
 * runtime-struct walk). Print the faulting address and the RIP as a module
 * offset (RVA) so it can be symbolized with llvm-symbolizer against the PDB. */
extern "C" uint32_t    g_last_hle_nid;    /* ppu_hle.cpp breadcrumb */
extern "C" const char* g_last_hle_name;

extern "C" thread_local ppu_context* g_active_ctx;
extern "C" void ppu_dump_guest_stack(ppu_context* ctx, const char* tag);
extern "C" void sys_ppu_thread_dump_guest_stacks(void);
static LONG WINAPI ydkj_crash_filter(EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    fprintf(stderr, "\n[CRASH] code=0x%08lX rip=%p\n",
            (unsigned long)er->ExceptionCode, er->ExceptionAddress);
    fprintf(stderr, "[CRASH] last HLE NID 0x%08X (%s)\n",
            g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(stderr, "[CRASH] %s fault address 0x%llX\n",
                er->ExceptionInformation[0] ? "write" : "read",
                (unsigned long long)er->ExceptionInformation[1]);
    if (g_active_ctx) fprintf(stderr, "[CRASH] guest ctr=0x%08X lr=0x%08X r3=0x%08X\n",
          (uint32_t)g_active_ctx->ctr, (uint32_t)g_active_ctx->lr, (uint32_t)g_active_ctx->gpr[3]);
    HMODULE mod = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)er->ExceptionAddress, &mod);
    fprintf(stderr, "[CRASH] module=%p rva=0x%llX  (llvm-symbolizer --obj=ydkj_boot.exe 0x%llX)\n",
            (void*)mod, (unsigned long long)((char*)er->ExceptionAddress - (char*)mod),
            (unsigned long long)((char*)er->ExceptionAddress - (char*)mod));
    /* Host call stack (RVAs) so the lifted caller can be symbolized. */
    void* frames[24];
    USHORT n = RtlCaptureStackBackTrace(0, 24, frames, NULL);
    for (USHORT i = 0; i < n; i++) {
        HMODULE m = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m);
        if (m == mod)
            fprintf(stderr, "[CRASH]   #%-2u rva=0x%llX\n", i,
                    (unsigned long long)((char*)frames[i] - (char*)m));
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

#ifdef _WIN32
/* abort()/exit(3) reporter: the recompiled CRT (or a failed invariant) can call
 * abort() — Windows turns that into exit code 3 with no message. Capture a host
 * backtrace (RVAs) + the last HLE NID so the aborting caller can be symbolized. */
static void ydkj_abort_handler(int)
{
    fprintf(stderr, "\n[ABORT] SIGABRT raised; last HLE NID 0x%08X (%s)\n",
            g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
    void* frames[32];
    USHORT n = RtlCaptureStackBackTrace(0, 32, frames, NULL);
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ydkj_abort_handler, &self);
    for (USHORT i = 0; i < n; i++) {
        HMODULE m = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m);
        if (m == self)
            fprintf(stderr, "[ABORT]   #%-2u rva=0x%llX\n", i,
                    (unsigned long long)((char*)frames[i] - (char*)m));
    }
    fflush(stderr);
    _exit(3);
}
#endif

/* Derive the VFS root (the dir containing PS3_GAME) from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf  -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];
static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }
    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip three trailing components: EBOOT.elf / USRDIR / PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

#ifdef _WIN32
static void set_default_environment(const char* name, const char* value)
{
    if (!getenv(name))
        _putenv_s(name, value);
}

static int configure_standalone_usrdir(void)
{
    const char* override_root = getenv("PS3_VFS_ROOT");
    if (override_root && *override_root) {
        strncpy(s_vfs_root, override_root, sizeof s_vfs_root - 1);
        s_vfs_root[sizeof s_vfs_root - 1] = 0;
    } else {
        DWORD length = GetModuleFileNameA(NULL, s_vfs_root,
                                          (DWORD)sizeof s_vfs_root);
        if (!length || length >= sizeof s_vfs_root) {
            fprintf(stderr, "[boot] cannot determine executable directory\n");
            return 0;
        }
        char* slash = strrchr(s_vfs_root, '\\');
        char* forward = strrchr(s_vfs_root, '/');
        if (forward && (!slash || forward > slash)) slash = forward;
        if (!slash) {
            strcpy(s_vfs_root, ".");
        } else if (slash == s_vfs_root) {
            slash[1] = 0;
        } else {
            *slash = 0;
        }
        set_default_environment("PS3_VFS_ROOT", s_vfs_root);
    }
    set_default_environment("PS3_VFS_LAYOUT", "usrdir");
    ppu_vfs_root = s_vfs_root;

    set_default_environment("PS3_TOC_SET",
                            "0x1027c58,0x1037a88,0x1047a38");
    set_default_environment("FLOW_NOSPILL", "1");
    set_default_environment("TAIKO_DNS_LOOPBACK", "1");
    set_default_environment("TAIKO_OFFLINE_COMPLETE", "1");
    set_default_environment("TAIKO_FS_YIELD", "0");
    set_default_environment("TAIKO_AUDIO_DECODE", "1");
    set_default_environment("TAIKO_AUDIO_SPU", "1");

    return 1;
}

static uint32_t load_embedded_ppu_image(void)
{
#ifdef TAIKO_EMBED_PPU_IMAGE
    HRSRC resource = FindResourceA(NULL, MAKEINTRESOURCEA(101), RT_RCDATA);
    if (!resource) {
        fprintf(stderr, "[boot] embedded PPU image resource is missing\n");
        return 0;
    }
    HGLOBAL loaded = LoadResource(NULL, resource);
    const DWORD size = SizeofResource(NULL, resource);
    const void* data = loaded ? LockResource(loaded) : NULL;
    return ppu_load_embedded_image(data, size);
#else
    fprintf(stderr, "[boot] this build has no embedded PPU image\n");
    return 0;
#endif
}
#endif

/* Host-provided symbols the runtime + HLE libs need. */
extern "C" uint8_t* vm_base = nullptr;
extern "C" uint32_t ppu_vm_size;   /* defined in ppu_loader.cpp (OOB guard) */
extern "C" void lv2_init_syscalls(void);   /* runtime/syscalls/lv2_register.c */

#ifdef _WIN32
/* Opt-in statistical profiler for the initial PPU thread.  Sampling the host
 * RIP is substantially cheaper than instrumenting every lifted function call,
 * and every lifted guest function already has a host pointer in
 * function_table[].  Keep this separate from the normal runtime: it suspends
 * the main thread for a CONTEXT_CONTROL snapshot for only a few microseconds,
 * then reports the hottest guest addresses every five seconds.
 *
 * Enable with PPU_SAMPLE_PROFILE=1. */
struct ppu_sample_symbol {
    uintptr_t host;
    uint32_t guest;
};

static HANDLE s_ppu_sample_main_thread;

static DWORD WINAPI ppu_sample_profiler(LPVOID)
{
    std::vector<ppu_sample_symbol> symbols;
    symbols.reserve((size_t)function_table_count);
    for (uint64_t i = 0; i < function_table_count; i++)
        symbols.push_back({(uintptr_t)function_table[i].func,
                           (uint32_t)function_table[i].addr});
    std::sort(symbols.begin(), symbols.end(),
              [](const ppu_sample_symbol& a, const ppu_sample_symbol& b) {
                  return a.host < b.host;
              });

    const uintptr_t image_base = (uintptr_t)GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)image_base;
    const IMAGE_NT_HEADERS64* nt =
        (const IMAGE_NT_HEADERS64*)(image_base + (uintptr_t)dos->e_lfanew);
    const uintptr_t image_end = image_base + nt->OptionalHeader.SizeOfImage;
    uintptr_t text_begin = image_base, text_end = image_base;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(section[i].Name, ".text", 5) == 0) {
            text_begin = image_base + section[i].VirtualAddress;
            const uintptr_t text_size = std::max<uint32_t>(
                section[i].Misc.VirtualSize, section[i].SizeOfRawData);
            text_end = text_begin + text_size;
            break;
        }
    }

    std::unordered_map<uint32_t, uint32_t> counts;
    std::unordered_map<uint32_t, uint32_t> callers;
    std::unordered_map<uint32_t, uint32_t> rip_buckets;
    std::unordered_map<uint32_t, uint32_t> caller_rvas;
    std::unordered_map<uint32_t, uint32_t> host_call_rvas;
    uint32_t in_image_other = 0, outside_image = 0, failed = 0, total = 0;
    ULONGLONG report_at = GetTickCount64() + 5000;

    auto resolve_guest = [&symbols](uintptr_t pc) -> uint32_t {
        auto it = std::upper_bound(
            symbols.begin(), symbols.end(), pc,
            [](uintptr_t v, const ppu_sample_symbol& s) { return v < s.host; });
        if (it == symbols.begin()) return 0;
        --it;
        /* Do not let the runtime/HLE .text following the generated chunks
         * inherit the final guest symbol. The largest generated host body in
         * this image is just under 48 KiB. */
        return pc - it->host < 0xC000u ? it->guest : 0;
    };

    for (;;) {
        Sleep(4);
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_CONTROL;
        uint64_t stack_words[256] = {};
        SIZE_T stack_bytes = 0;
        if (SuspendThread(s_ppu_sample_main_thread) == (DWORD)-1) {
            failed++;
            continue;
        }
        BOOL ok = GetThreadContext(s_ppu_sample_main_thread, &ctx);
        if (ok)
            ReadProcessMemory(GetCurrentProcess(), (const void*)(uintptr_t)ctx.Rsp,
                              stack_words, sizeof(stack_words), &stack_bytes);
        ResumeThread(s_ppu_sample_main_thread);
        if (!ok) {
            failed++;
            continue;
        }

        total++;
        uintptr_t rip = (uintptr_t)ctx.Rip;
        if (rip < image_base || rip >= image_end) {
            outside_image++;
        } else {
            uint32_t guest = resolve_guest(rip);
            if (guest) {
                counts[guest]++;
                rip_buckets[(uint32_t)((rip - image_base) & ~(uintptr_t)0xFu)]++;
            }
            else in_image_other++;
        }

        /* When the main thread is in Wine/ntdll (sleep, event, mutex, file IO),
         * the first lifted return address on its host stack identifies which
         * guest function paid for that wait. This is the useful distinction
         * between a CPU-heavy frame builder and an incorrectly paced HLE wait. */
        if (rip < image_base || rip >= image_end) {
            size_t nw = (size_t)(stack_bytes / sizeof(stack_words[0]));
            bool recorded_host_call = false;
            for (size_t i = 0; i < nw; i++) {
                uintptr_t ret = (uintptr_t)stack_words[i];
                /* Raw stack scans also encounter arbitrary data pointers.  A
                 * previous profiler revision accepted any address in the PE
                 * image and consequently reported .rdata/.bss objects as
                 * return addresses.  Only executable .text can be a caller. */
                if (ret < text_begin || ret >= text_end) continue;
                if (!recorded_host_call) {
                    host_call_rvas[(uint32_t)(ret - image_base)]++;
                    recorded_host_call = true;
                }
                uint32_t guest = resolve_guest(ret);
                if (guest) {
                    callers[guest]++;
                    caller_rvas[(uint32_t)(ret - image_base)]++;
                    break;
                }
            }
        }

        ULONGLONG now = GetTickCount64();
        if (now < report_at) continue;

        std::vector<std::pair<uint32_t, uint32_t>> top;
        top.reserve(counts.size());
        for (const auto& kv : counts) top.push_back({kv.second, kv.first});
        std::sort(top.begin(), top.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });

        fprintf(stderr,
                "[PPUSAMPLE] total=%u outside=%u runtime=%u failed=%u top:",
                total, outside_image, in_image_other, failed);
        size_t n = top.size() < 12 ? top.size() : 12;
        for (size_t i = 0; i < n; i++)
            fprintf(stderr, " %08X=%u(%.1f%%)", top[i].second, top[i].first,
                    total ? top[i].first * 100.0 / total : 0.0);
        std::vector<std::pair<uint32_t, uint32_t>> caller_top;
        caller_top.reserve(callers.size());
        for (const auto& kv : callers) caller_top.push_back({kv.second, kv.first});
        std::sort(caller_top.begin(), caller_top.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        fprintf(stderr, " callers:");
        n = caller_top.size() < 8 ? caller_top.size() : 8;
        for (size_t i = 0; i < n; i++)
            fprintf(stderr, " %08X=%u(%.1f%%)", caller_top[i].second,
                    caller_top[i].first,
                    total ? caller_top[i].first * 100.0 / total : 0.0);
        auto print_rvas = [](const char* tag,
                             const std::unordered_map<uint32_t, uint32_t>& map,
                             size_t limit) {
            std::vector<std::pair<uint32_t, uint32_t>> v;
            v.reserve(map.size());
            for (const auto& kv : map) v.push_back({kv.second, kv.first});
            std::sort(v.begin(), v.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
            fprintf(stderr, " %s:", tag);
            size_t m = v.size() < limit ? v.size() : limit;
            for (size_t i = 0; i < m; i++)
                fprintf(stderr, " %08X=%u", v[i].second, v[i].first);
        };
        print_rvas("rva", rip_buckets, 6);
        print_rvas("hostcall", host_call_rvas, 8);
        print_rvas("callrva", caller_rvas, 6);
        fprintf(stderr, "\n");

        counts.clear();
        callers.clear();
        rip_buckets.clear();
        caller_rvas.clear();
        host_call_rvas.clear();
        in_image_other = outside_image = failed = total = 0;
        report_at = now + 5000;
    }
}

static void start_ppu_sample_profiler(void)
{
    const char* env = getenv("PPU_SAMPLE_PROFILE");
    if (!env || env[0] == '0') return;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &s_ppu_sample_main_thread,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                         FALSE, 0)) {
        fprintf(stderr, "[PPUSAMPLE] failed to open main thread (%lu)\n",
                (unsigned long)GetLastError());
        return;
    }
    HANDLE th = CreateThread(NULL, 0, ppu_sample_profiler, NULL, 0, NULL);
    if (th) CloseHandle(th);
    fprintf(stderr, "[PPUSAMPLE] sampling initial PPU thread every 4ms\n");
}
#endif

/* Guest-callback dispatch + RSX vblank/flip driver.
 *
 * g_ps3_guest_caller (defined NULL by libs/system/cellSysutil.c) is the hook the
 * HLE runtime uses to call back into recompiled code -- cellSysutil events and
 * the GCM vblank/flip handlers. ppu_guest_call (ppu_loader.cpp) does the OPD ->
 * dispatch. On real hardware the RSX fires a vblank interrupt ~60x/s that drives
 * the game's frame loop; with no RSX we synthesize it from a host timer thread
 * calling cellGcmTickVBlank()/TickFlip(), which invoke the registered handlers.
 * Without this the game inits, registers its handlers, and then waits forever
 * for a vblank that never comes. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" ps3_guest_caller_fn g_ps3_guest_caller;        /* libs/system/cellSysutil.c */
extern "C" uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" void cellGcmTickVBlank(void);
extern "C" void cellGcmTickFlip(void);
extern "C" int  cellGcm_take_flip_pending(void);  /* hoisted to file scope: clang-cl rejects block-scope extern "C" */
extern "C" int ps3recomp_start_frame_driver(void);
extern "C" void ps3recomp_stop_frame_driver(void);
extern "C" void ps3recomp_join_frame_driver(void);
extern "C" void cellAudioHostShutdown(void);
extern "C" void cellPadHostShutdown(void);
#ifdef PS3RECOMP_RSX_BACKEND_SDL_GPU
extern "C" int rsx_sdl_gpu_backend_main_init(unsigned, unsigned, const char*);
extern "C" int rsx_sdl_gpu_backend_main_iterate(int);
extern "C" void rsx_sdl_gpu_backend_main_shutdown(void);
#endif

static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{ ppu_guest_call(opd, a0, a1, a2, a3); }

#ifdef _WIN32
#if 0 /* Legacy D3D12 ticker; superseded by runtime/host/frame_driver.cpp. */
/* RSX present backend (libs/video/rsx_d3d12_backend.c). Driven on the vblank
 * thread so the D3D12 device + window message pump live on one thread. */
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int cellGcm_take_flip_pending(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" int  rsx_null_backend_pump_messages(void);
extern "C" void cellGcm_rsx_process_fifo(void);   /* cellGcmSys.c: drain get->put */
extern "C" unsigned cellGcm_flip_request_count(void);
extern "C" int sys_event_queue_inject(unsigned int, unsigned long long, unsigned long long, unsigned long long, unsigned long long);

static DWORD WINAPI vblank_ticker(LPVOID)
{
    /* This thread emulates an independent 60 Hz hardware clock plus the RSX
     * command processor.  Ordinary Wine UI/server activity must not preempt it
     * long enough to slow the guest's animation and beatmap clocks.  HIGHEST
     * remains below TIME_CRITICAL, so audio and OS-critical work can still run. */
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST))
        fprintf(stderr, "[rsx] warning: failed to raise vblank priority (%lu)\n",
                (unsigned long)GetLastError());
    const char* null_env = getenv("PS3RECOMP_NULL_RSX");
    const int null_rsx = null_env && null_env[0] != '0';
    int rsx_ok = ((null_rsx ? rsx_null_backend_init
                            : rsx_d3d12_backend_init)
                  (1280, 720, "Taiko no Tatsujin (ps3recomp)") == 0);
    fprintf(stderr, "[rsx] %s backend init %s\n",
            null_rsx ? "null" : "D3D12",
            rsx_ok ? "OK -- window open" : "FAILED");
    unsigned last_flip = 0;
    const char* rsx_prof_env = getenv("RSX_PROFILE");
    const bool rsx_prof = rsx_prof_env && rsx_prof_env[0] != '0';
    ULONGLONG prof_window = GetTickCount64(), prof_prev_loop = prof_window;
    unsigned prof_loops = 0, prof_ticks = 0, prof_catchups = 0;
    ULONGLONG prof_max_gap = 0;
    unsigned prof_flip_start = cellGcm_flip_request_count();
    LARGE_INTEGER prof_qpc_freq;
    QueryPerformanceFrequency(&prof_qpc_freq);
    enum { PROF_SLEEP, PROF_CALLBACKS, PROF_PRESENT, PROF_FIFO, PROF_PUMP, PROF_STAGE_COUNT };
    unsigned long long prof_stage_ticks[PROF_STAGE_COUNT] = {};
    unsigned long long prof_stage_max[PROF_STAGE_COUNT] = {};
    unsigned prof_stage_calls[PROF_STAGE_COUNT] = {};
    auto prof_now = [&]() -> unsigned long long {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return (unsigned long long)t.QuadPart;
    };
    auto prof_record = [&](int stage, unsigned long long begin) {
        if (!rsx_prof) return;
        const unsigned long long dt = prof_now() - begin;
        prof_stage_ticks[stage] += dt;
        if (dt > prof_stage_max[stage]) prof_stage_max[stage] = dt;
        prof_stage_calls[stage]++;
    };
    /* The game's frame pacing (vblank/flip handlers -> display frame counter) must
     * advance at ~60Hz regardless of how long present() blocks. On a hidden/occluded
     * window DXGI Present throttles hard, which previously stalled these ticks and
     * paced the game's main loop to ~0.5fps. Drive the ticks off REAL elapsed time
     * and catch up in a bounded burst so present latency never slows the game. */
    LARGE_INTEGER next_tick_qpc;
    QueryPerformanceCounter(&next_tick_qpc);
    const long long tick_step_qpc = prof_qpc_freq.QuadPart / 60;
    ULONGLONG next_poll = GetTickCount64() + 4;
    for (;;) {
        const unsigned long long prof_sleep_t0 = rsx_prof ? prof_now() : 0;
        /* Keep the FIFO poll cadence at roughly 4 ms start-to-start.  An
         * unconditional Sleep(4) here used to add four milliseconds after
         * however long the previous drain/render took; a 12-15 ms busy pass
         * therefore became a 16-19 ms cycle before any new work was even
         * observed.  If work ran past the next poll, do one immediate pass and
         * rebase instead of accumulating a burst of empty catch-up polls. */
        ULONGLONG before_poll = GetTickCount64();
        if ((long long)(next_poll - before_poll) > 0)
            Sleep((DWORD)(next_poll - before_poll));
        prof_record(PROF_SLEEP, prof_sleep_t0);
        ULONGLONG now = GetTickCount64();
        LARGE_INTEGER tick_now_qpc;
        QueryPerformanceCounter(&tick_now_qpc);
        if ((long long)(now - next_poll) >= 4)
            next_poll = now + 4;
        else
            next_poll += 4;
        if (rsx_prof) {
            ULONGLONG gap = now - prof_prev_loop;
            if (gap > prof_max_gap) prof_max_gap = gap;
            prof_prev_loop = now;
            prof_loops++;
        }
        int fired = 0;
        while (tick_now_qpc.QuadPart >= next_tick_qpc.QuadPart && fired < 240) {
            const unsigned long long prof_callback_t0 = rsx_prof ? prof_now() : 0;
            cellGcmTickVBlank();
            cellGcmTickFlip();
            prof_record(PROF_CALLBACKS, prof_callback_t0);
            /* Present a pending flip BEFORE draining further: the flip fires
             * at a get==put frame boundary (guest thread), so the batch held
             * right now is exactly the completed frame. Presenting on a raw
             * flip-count change after the drain raced the guest's next-frame
             * writes and showed empty or mixed batches. */
            {
                if (rsx_ok && cellGcm_take_flip_pending()) {
                    const unsigned long long prof_present_t0 = rsx_prof ? prof_now() : 0;
                    if (!null_rsx)
                    rsx_d3d12_backend_present();
                    prof_record(PROF_PRESENT, prof_present_t0);
                    last_flip = cellGcm_flip_request_count();
                }
            }
            /* Drain the game's GCM FIFO every tick -- this writes the RSX sync-fence
             * labels (e.g. @0x03000410) that the game's per-frame logic blocks on.
             * Doing it here (not after present) keeps those fences advancing at 60Hz
             * even when present() throttles on a hidden/occluded window. */
            if (rsx_ok) {
                const unsigned long long prof_fifo_t0 = rsx_prof ? prof_now() : 0;
                cellGcm_rsx_process_fifo();
                prof_record(PROF_FIFO, prof_fifo_t0);
            }
            next_tick_qpc.QuadPart += tick_step_qpc;
            fired++;
        }
        if (fired >= 240)                    /* fell too far behind -> resync */
            next_tick_qpc = tick_now_qpc;
        if (rsx_prof) {
            prof_ticks += (unsigned)fired;
            if (fired > 1) prof_catchups++;
        }
        /* Drain at the outer ~4ms cadence too (not just the 16ms tick):
         * titles fence EVERY render pass on an RSX label the drain writes;
         * at 16ms per fence wave's six passes paced the guest to ~7 fps.
         * The real RSX writes those fences in microseconds. */
        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                const unsigned long long prof_present_t0 = rsx_prof ? prof_now() : 0;
                if (!null_rsx)
                rsx_d3d12_backend_present();
                prof_record(PROF_PRESENT, prof_present_t0);
                last_flip = cellGcm_flip_request_count();
            }
            const unsigned long long prof_fifo_t0 = rsx_prof ? prof_now() : 0;
            cellGcm_rsx_process_fifo();
            prof_record(PROF_FIFO, prof_fifo_t0);
        }
        /* YDKJ_INJECT_Q3: the main thread polls q3 (load/state-complete) each frame but
         * it's never posted (producer signals a condvar, never enqueues). Inject a q3
         * event periodically to test whether delivering the completion advances the
         * game's state machine to instantiate + display the (force-parsed) menu movie. */
        if (getenv("YDKJ_INJECT_Q3")) {
            static int s_q3=0; const char* qe=getenv("YDKJ_INJECT_Q3"); uint32_t qid=(uint32_t)strtoul(qe,0,0); if(qid==0||qid==1) qid=3;
            if(++s_q3 % 8 == 0){ int r=sys_event_queue_inject(qid, 0x1234, 0, 0, 0);
              static int _n=0; if(_n++<12) fprintf(stderr,"[INJQ3] injected q%u event rc=%d\n",qid,r); } }
        if (rsx_ok) {
            const unsigned long long prof_pump_t0 = rsx_prof ? prof_now() : 0;
            if ((null_rsx ? rsx_null_backend_pump_messages()
                          : rsx_d3d12_backend_pump_messages()) != 0) {
                /* ppu_run has no cancellation point, so only stopping RSX
                 * leaves the guest and its workers alive after the last
                 * window is gone. This executable harness owns the process:
                 * closing its renderer is the application exit request. */
                fprintf(stderr, "[boot] game window closed; exiting process\n");
                fflush(stderr);
                ExitProcess(0);
            }
            prof_record(PROF_PUMP, prof_pump_t0);
            if (getenv("YDKJ_PACETRACE")) {
                static ULONGLONG s_win=0; static int s_pf=0, s_pres=0; static ULONGLONG s_presms=0;
                s_pf += fired; s_pres++;
                ULONGLONG t0=GetTickCount64();
                if (!null_rsx) rsx_d3d12_backend_present();
                ULONGLONG t1=GetTickCount64();
                s_presms += (t1-t0);
                if (s_win==0) s_win=now;
                if (now - s_win >= 1000) {
                    fprintf(stderr,"[PACE] process_fifo=%d/s  present=%d/s  present_total=%llums/s (avg %llums)\n",
                            s_pf, s_pres, s_presms, s_pres? s_presms/s_pres:0);
                    s_pf=0; s_pres=0; s_presms=0; s_win=now;
                }
            } else {
                /* Present only on a guest flip (frame boundary). A fixed-clock
                 * present can catch the drain mid-frame -- notably while the
                 * guest is blocked in the FIFO-wrap recycle callback -- and
                 * flash a partial frame (clear + a few draws). Before the
                 * first flip present freely so the window isn't stuck white
                 * during boot. */
                unsigned fc = cellGcm_flip_request_count();
                if (fc != last_flip || fc == 0) {
                    if (!null_rsx) rsx_d3d12_backend_present();
                    last_flip = fc;
                }
            }
        }
        if (rsx_prof && now - prof_window >= 1000) {
            unsigned flip_now = cellGcm_flip_request_count();
            double elapsed = (double)(now - prof_window);
            fprintf(stderr,
                    "[TICKPROF] loops=%.1f/s ticks=%.1f/s flips=%.1f/s catchups=%u max_gap=%llums\n",
                    prof_loops * 1000.0 / elapsed, prof_ticks * 1000.0 / elapsed,
                    (flip_now - prof_flip_start) * 1000.0 / elapsed,
                    prof_catchups, (unsigned long long)prof_max_gap);
            static const char* stage_names[PROF_STAGE_COUNT] = {
                "sleep", "callbacks", "present", "fifo", "pump"
            };
            fprintf(stderr, "[TICKSTAGE]");
            for (int i = 0; i < PROF_STAGE_COUNT; ++i) {
                const double scale = 1000.0 / (double)prof_qpc_freq.QuadPart;
                const double total_ms = prof_stage_ticks[i] * scale;
                const double avg_ms = prof_stage_calls[i]
                    ? total_ms / prof_stage_calls[i] : 0.0;
                const double max_ms = prof_stage_max[i] * scale;
                fprintf(stderr, " %s=%.1fms/s(%.3f/%.3f,%u)",
                        stage_names[i], total_ms * 1000.0 / elapsed,
                        avg_ms, max_ms, prof_stage_calls[i]);
                prof_stage_ticks[i] = prof_stage_max[i] = 0;
                prof_stage_calls[i] = 0;
            }
            fprintf(stderr, "\n");
            prof_window = now;
            prof_flip_start = flip_now;
            prof_loops = prof_ticks = prof_catchups = 0;
            prof_max_gap = 0;
        }
    }
    return 0;
}
#endif

extern "C" uint32_t    g_last_hle_nid;
extern "C" const char* g_last_hle_name;
#include <tlhelp32.h>
/* When the boot wedges, snapshot every other thread's instruction pointer as a
 * module RVA (symbolize with llvm-symbolizer) so a guest spin/wait is pinned to
 * an exact lifted function -- the HLE breadcrumb only covers HLE calls. */
/* Snapshot every other thread's RIP. For threads in the boot module (lifted
 * guest code) print the RVA (symbolizable) + a couple of stack-return RVAs;
 * for threads parked in a DLL (OS waits / FMOD) print the module name so they
 * are not mistaken for guest spins. Called twice so the caller can diff which
 * guest thread is genuinely parked (same RIP) vs. still progressing. */
static void dump_threads(const char* label, HMODULE self)
{
    fprintf(stderr, "[WATCHDOG] %s; last HLE call = 0x%08X (%s)\n",
            label, g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; te.dwSize = sizeof te;
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME,
                                   FALSE, te.th32ThreadID);
            if (!th) continue;
            SuspendThread(th);
            CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th, &ctx)) {
                HMODULE m = NULL;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)ctx.Rip, &m);
                if (m == self) {
                    fprintf(stderr, "[WATCHDOG]   tid %5lu BOOT rip rva=0x%llX\n",
                            (unsigned long)te.th32ThreadID,
                            (unsigned long long)((char*)ctx.Rip - (char*)self));
                } else {
                    char path[MAX_PATH] = "?";
                    if (m) GetModuleFileNameA(m, path, sizeof path);
                    const char* base = strrchr(path, '\\');
                    fprintf(stderr, "[WATCHDOG]   tid %5lu in %s\n",
                            (unsigned long)te.th32ThreadID, base ? base + 1 : path);
                }
                /* Scan the suspended thread's stack for boot-module return
                 * addresses to reconstruct the lifted call chain — done for ALL
                 * threads (even when RIP is parked in ntdll inside a CriticalSection
                 * call), since that's exactly where the busy-spin's lwmutex churn
                 * lands the main thread. Map RVAs -> func_ names via flow.map.
                 * (some false positives expected — these are stack-scan hits.) */
                {
                    uint64_t* sp = (uint64_t*)ctx.Rsp;
                    /* Bound the scan to the committed stack region so we never read
                     * past the guard page (VirtualQuery gives this region's end). */
                    MEMORY_BASIC_INFORMATION mbi;
                    uint64_t region_end = (uint64_t)sp + 0x8000;
                    if (VirtualQuery((LPCVOID)sp, &mbi, sizeof mbi))
                        region_end = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
                    int maxk = (int)((region_end - (uint64_t)sp) / 8);
                    if (maxk > 0x20000 / 8) maxk = 0x20000 / 8;
                    int found = 0;
                    for (int k = 0; k < maxk && found < 20; k++) {
                        uint64_t v = sp[k];
                        if (v < (uint64_t)self) continue;
                        HMODULE mm = NULL;
                        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           (LPCSTR)v, &mm);
                        if (mm == self) {
                            fprintf(stderr, "[WATCHDOG]       tid %5lu ret rva=0x%llX\n",
                                    (unsigned long)te.th32ThreadID,
                                    (unsigned long long)(v - (uint64_t)self));
                            found++;
                        }
                    }
                }
            }
            ResumeThread(th);
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);

    /* Host RIPs usually land in ntdll while a guest thread is inside an HLE
     * wait.  The LV2 thread table still owns each live ppu_context, including
     * its guest stack, so print that view as well.  This is intentionally a
     * lock-free diagnostic snapshot: a slightly torn register is preferable
     * to stopping a thread while it owns the thread-table lock. */
    sys_ppu_thread_dump_guest_stacks();
    fflush(stderr);
}

static DWORD WINAPI hang_watchdog(LPVOID)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&hang_watchdog, &self);
    Sleep(8000);
    dump_threads("8s sample", self);
    Sleep(7000);
    dump_threads("15s sample", self);
    return 0;
}
#endif

struct initial_ppu_args {
    uint32_t entry;
    uint32_t stack_top;
    int result;
    std::atomic<bool> completed{false};
};

#ifdef _WIN32
using initial_ppu_thread_handle = HANDLE;

static DWORD WINAPI initial_ppu_thread(void* opaque)
{
    initial_ppu_args* args = static_cast<initial_ppu_args*>(opaque);
    std::fprintf(stderr, "[boot] MAIN guest thread tid=%llu (256 MiB stack)\n",
                 (unsigned long long)ps3_host_thread_id());
    args->result = ppu_run(args->entry, args->stack_top);
    args->completed.store(true, std::memory_order_release);
    return 0;
}

static int start_initial_ppu(initial_ppu_args* args,
                             initial_ppu_thread_handle* thread)
{
    *thread = CreateThread(NULL, 256u * 1024 * 1024, initial_ppu_thread,
                           args, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!*thread) {
        std::fprintf(stderr, "[boot] failed to create initial PPU thread (%lu)\n",
                     (unsigned long)GetLastError());
        return -1;
    }
    return 0;
}

static void join_initial_ppu(initial_ppu_thread_handle thread)
{
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}
#else
using initial_ppu_thread_handle = pthread_t;

static void* initial_ppu_thread(void* opaque)
{
    initial_ppu_args* args = static_cast<initial_ppu_args*>(opaque);
    std::fprintf(stderr, "[boot] MAIN guest thread tid=%llu (256 MiB stack)\n",
                 (unsigned long long)ps3_host_thread_id());
    args->result = ppu_run(args->entry, args->stack_top);
    args->completed.store(true, std::memory_order_release);
    return nullptr;
}

static int start_initial_ppu(initial_ppu_args* args,
                             initial_ppu_thread_handle* thread)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u * 1024 * 1024);
    int rc = pthread_create(thread, &attr, initial_ppu_thread, args);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        std::fprintf(stderr, "[boot] failed to create initial PPU thread (%d)\n", rc);
    return rc;
}

static void join_initial_ppu(initial_ppu_thread_handle thread)
{
    pthread_join(thread, nullptr);
}
#endif

static int run_initial_ppu(uint32_t entry, uint32_t stack_top)
{
    initial_ppu_args args;
    args.entry = entry;
    args.stack_top = stack_top;
    args.result = 1;
    initial_ppu_thread_handle thread;
    if (start_initial_ppu(&args, &thread) != 0) return 1;
    join_initial_ppu(thread);
    return args.result;
}

/* The flat VM treats every address as valid RAM, so it must span every region
 * the PS3 memory map uses. The game's heap maps at 0x20000000+ and reaches
 * ~0x50000000, but sys_ppu_thread_create allocates thread stacks in the PS3
 * stack region at 0xD0000000-0xDFFFFFFF (vm.h: VM_STACK_BASE). Without covering
 * that, every spawned thread's stack access is OOB (reads 0 / writes dropped)
 * and the thread crashes. Size to include the stack region: ~3.75 GB, lazily
 * committed by the OS (only touched pages are backed). */
#define VM_SIZE    0x100010000ull /* full 32-bit guest space + 64K guard (top-edge reads), demand-committed */
#define STACK_TOP  0x0FF00000u   /* main-thread stack, below the 0x10000000 segment */

#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the full 4 GB guest space up front (no
 * commit cost) and commit each 64 KB page on first access. This makes EVERY
 * 32-bit guest offset valid -- a garbage guest pointer reads as zero instead of
 * crashing the process (essential now that the recompiled engine runs deep and
 * worker threads touch incomplete state). Out-of-arena faults fall through to
 * the crash reporter. */
/* PS3_VM_TRAP=N: report the first N demand-commits before performing them.
 *
 * The commit-and-continue policy above is what makes memory corruption in this
 * port present as "random": a wild guest store never faults, it quietly
 * materialises a fresh page and the damage surfaces thousands of instructions
 * later somewhere unrelated. It also means the stack guard pages that
 * vm_stack_allocate installs (vm.h) do nothing -- a guest thread overflowing its
 * 16 KB stack has its guard page committed for it and walks into the next
 * thread's stack in silence.
 *
 * Each line names the guest EA plus the faulting guest thread's cia/lr, which is
 * enough to point at the writing guest function. Commits into the guest stack
 * region are flagged: those are the stack overflows. Reporting only -- the page
 * is still committed, so the boot behaves exactly as before. */
extern "C" thread_local ppu_context* g_active_ctx;
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            static int trap_n = -1;
            if (trap_n < 0) { const char* e = getenv("PS3_VM_TRAP"); trap_n = e ? atoi(e) : 0; }
            /* Only commits made BY GUEST CODE are interesting. The ELF loader
             * pages in the image sequentially before execution starts, which
             * burned the whole budget on 64 KB-stride entries with no context. */
            if (trap_n > 0 && g_active_ctx) {
                static LONG seen = 0;
                if (InterlockedIncrement(&seen) <= trap_n) {
                    uint32_t ea = (uint32_t)(fault - base);
                    ppu_context* c = g_active_ctx;
                    fprintf(stderr, "[VMTRAP] commit guest 0x%08X%s  cia=0x%08X lr=0x%08X tid=%llu\n",
                            ea, (ea >= 0xD0000000u) ? " (STACK REGION -- possible overflow)" : "",
                            c ? (uint32_t)c->cia : 0, c ? (uint32_t)c->lr : 0,
                            c ? (unsigned long long)c->thread_id : 0ull);
                    fflush(stderr);
                }
            }
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE)) {
                vm_note_accessible_range((uint32_t)((uintptr_t)page - base),
                                         0x10000, 1);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char** argv)
{
    const int standalone = argc < 2;
#ifndef TAIKO_EMBED_PPU_IMAGE
    if (standalone) {
        printf("usage: %s <EBOOT.elf>\n", argv[0]);
        return 2;
    }
#endif
#ifdef _WIN32
    /* Establish release defaults before reserving/loading the guest VM. Several
     * runtime paths cache environment switches on first use, and mutating the
     * process environment after loader activity perturbs early title startup. */
    if (standalone && !configure_standalone_usrdir()) return 1;
#endif

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    timeBeginPeriod(1);   /* 1ms timer resolution: the default ~15.6ms granularity
                           * inflates every sub-15ms wait (the game's event polls,
                           * usleeps) and throttled the whole title. */
    SetUnhandledExceptionFilter(ydkj_crash_filter);
    AddVectoredExceptionHandler(0 /*last*/, [](EXCEPTION_POINTERS* ep)->LONG{
        if (ep->ExceptionRecord->ExceptionCode == 0xC00000FDu /*STACK_OVERFLOW*/) {
            fprintf(stderr,"\n[STACKOVERFLOW] infinite recursion detected; backtrace (RVAs):\n");
            HMODULE mod=GetModuleHandleA(0); void* fr[62]; USHORT n=RtlCaptureStackBackTrace(0,62,fr,0);
            for(USHORT i=0;i<n;i++) fprintf(stderr," %llX",(unsigned long long)((char*)fr[i]-(char*)mod));
            fprintf(stderr,"\n"); fflush(stderr); ExitProcess(7);
        }
        return EXCEPTION_CONTINUE_SEARCH; });
    { ULONG g=256*1024; SetThreadStackGuarantee(&g); }  /* reserve stack so the SO handler can run */
    signal(SIGABRT, ydkj_abort_handler);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: don't lose prints on kill */
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Flat VM: one host buffer, guest addr -> vm_base + addr. This maps the
     * FULL 32-bit guest space uniformly (page 0, the 0x60000000..0xD0000000
     * range, everything) -- which native-VA mapping can't on Windows, because
     * the OS reserves the low 64 KB and DLLs occupy parts of the mid range.
     * On real PS3 those addresses are RAM, and the game writes to them (its
     * null-object inits land on page 0); calloc backs them so the game runs.
     * HLE functions that take guest pointers must translate via vm_base /
     * vm_write* (which also byte-swap) -- a raw *guest_ptr would deref the host
     * buffer's offset incorrectly. */
#ifdef _WIN32
    /* Reserve the full 4 GB guest space; pages commit on first touch via the VEH. */
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;   /* full 32-bit space backed -> OOB guard unnecessary */
#else
    vm_base = (uint8_t*)ps3_host_reserve_guest_vm();
    ppu_vm_size = 0;   /* complete 32-bit guest space, followed by a host guard */
#endif
    if (!vm_base) { printf("vm alloc failed\n"); return 1; }

    uint32_t entry = standalone
#ifdef _WIN32
        ? load_embedded_ppu_image()
#else
        ? 0
#endif
        : ppu_load_elf(argv[1]);
    if (!entry) { printf("load failed\n"); return 1; }

    if (!standalone) {
        derive_vfs_root(argv[1]);
    }
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    fprintf(stderr,"[boot-dbg] before ppu_recomp_register\n"); fflush(stderr);
    ppu_recomp_register();   /* lifted function table -> address map */
    fprintf(stderr,"[boot-dbg] after ppu_recomp_register; before ps3_load_prx_modules\n"); fflush(stderr);
    ps3_load_prx_modules();  /* real system PRX (libsre) -> guest RAM + exports */
    fprintf(stderr,"[boot-dbg] after prx; before ppu_hle_init\n"); fflush(stderr);
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT (sys_initialize_tls, ...) */
    ppu_fs_register();       /* cellFs VFS over the real game directory */
    fprintf(stderr,"[boot-dbg] before lv2_init_syscalls\n"); fflush(stderr);
    lv2_init_syscalls();     /* real lv2 syscall table (semaphore/memory/fs/...) */
    fprintf(stderr,"[boot-dbg] after lv2_init_syscalls\n"); fflush(stderr);

    /* Install the guest-callback hook and start the synthetic RSX vblank driver
     * so the game's frame loop advances (it no-ops until the game registers its
     * vblank/flip handlers during init). */
    g_ps3_guest_caller = harness_guest_caller;
#ifdef PS3RECOMP_RSX_BACKEND_SDL_GPU
    const char* null_override = getenv("PS3RECOMP_NULL_RSX");
    const bool use_sdl = !(null_override && null_override[0] != '0');
#endif
    unsigned host_sdl_subsystems = 0;
#ifdef PS3RECOMP_RSX_BACKEND_SDL_GPU
    if (use_sdl)
        host_sdl_subsystems |= PS3_HOST_SDL_VIDEO | PS3_HOST_SDL_GAMEPAD;
#endif
#ifdef PS3RECOMP_INPUT_BACKEND_SDL3
    host_sdl_subsystems |= PS3_HOST_SDL_GAMEPAD;
#endif
#ifdef PS3RECOMP_AUDIO_BACKEND_SDL3
    if (!getenv("PS3RECOMP_NULL_AUDIO"))
        host_sdl_subsystems |= PS3_HOST_SDL_AUDIO;
#endif
    if (ps3_host_sdl_init(host_sdl_subsystems) != 0) return 1;
#ifdef PS3RECOMP_RSX_BACKEND_SDL_GPU
    if (use_sdl && rsx_sdl_gpu_backend_main_init(
            1280, 720, "Taiko no Tatsujin (ps3recomp)") != 0) {
        ps3_host_sdl_shutdown();
        return 1;
    }
#endif
    if (ps3recomp_start_frame_driver() != 0) {
        fprintf(stderr, "[boot] failed to start vblank/FIFO frame driver\n");
#ifdef PS3RECOMP_RSX_BACKEND_SDL_GPU
        if (use_sdl) rsx_sdl_gpu_backend_main_shutdown();
#endif
        ps3_host_sdl_shutdown();
        return 1;
    }
#ifdef _WIN32
    /* Suspending every Wine thread while querying module ownership can
     * self-deadlock if the sampled thread owns the loader lock.  Keep this
     * invasive diagnostic opt-in; it used to freeze otherwise-live titles
     * exactly eight seconds after launch. */
    if (getenv("PS3RECOMP_WATCHDOG"))
        CreateThread(NULL, 0, hang_watchdog, NULL, 0, NULL);
#endif

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n", entry, STACK_TOP);
#ifdef _WIN32
    start_ppu_sample_profiler();
#endif
    int rc = 1;
#ifdef PS3RECOMP_RSX_BACKEND_SDL_GPU
    if (use_sdl) {
        initial_ppu_args args;
        args.entry = entry;
        args.stack_top = STACK_TOP;
        args.result = 1;
        initial_ppu_thread_handle ppu_thread;
        if (start_initial_ppu(&args, &ppu_thread) != 0) {
            cellAudioHostShutdown();
            cellPadHostShutdown();
            ps3recomp_stop_frame_driver();
            ps3recomp_join_frame_driver();
            rsx_sdl_gpu_backend_main_shutdown();
            ps3_host_sdl_shutdown();
            return 1;
        }
        bool close_requested = false;
        while (!args.completed.load(std::memory_order_acquire)) {
            if (rsx_sdl_gpu_backend_main_iterate(16)) {
                close_requested = true;
                break;
            }
        }
        if (!close_requested) {
            join_initial_ppu(ppu_thread);
            rc = args.result;
        }
        cellAudioHostShutdown();
        cellPadHostShutdown();
        ps3recomp_stop_frame_driver();
        ps3recomp_join_frame_driver();
        rsx_sdl_gpu_backend_main_shutdown();
        ps3_host_sdl_shutdown();
        if (close_requested) {
            fprintf(stderr, "[boot] game window closed; exiting process\n");
            fflush(stderr);
#ifdef _WIN32
            ExitProcess(0);
#else
            _Exit(0);
#endif
        }
    } else {
        rc = run_initial_ppu(entry, STACK_TOP);
        cellAudioHostShutdown();
        cellPadHostShutdown();
        ps3recomp_stop_frame_driver();
        ps3recomp_join_frame_driver();
        ps3_host_sdl_shutdown();
    }
#else
    rc = run_initial_ppu(entry, STACK_TOP);
    cellAudioHostShutdown();
    cellPadHostShutdown();
    ps3recomp_stop_frame_driver();
    ps3recomp_join_frame_driver();
    ps3_host_sdl_shutdown();
#endif
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);
    return rc;
}

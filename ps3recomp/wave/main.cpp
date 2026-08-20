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

#ifdef _WIN32
#include <windows.h>
/* Last-chance crash reporter: vm_base accesses are bounds-guarded, so a real
 * access violation means a HOST pointer deref (e.g. a bad function pointer or a
 * runtime-struct walk). Print the faulting address and the RIP as a module
 * offset (RVA) so it can be symbolized with llvm-symbolizer against the PDB. */
extern "C" uint32_t    g_last_hle_nid;    /* ppu_hle.cpp breadcrumb */
extern "C" const char* g_last_hle_name;

extern "C" __declspec(thread) ppu_context* g_active_ctx;
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
    fprintf(stderr, "[CRASH] module=%p rva=0x%llX  (llvm-symbolizer --obj=wave.exe 0x%llX)\n",
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

/* Host-provided symbols the runtime + HLE libs need. */
extern "C" uint8_t* vm_base = nullptr;
extern "C" uint32_t ppu_vm_size;   /* defined in ppu_loader.cpp (OOB guard) */
extern "C" void lv2_init_syscalls(void);   /* runtime/syscalls/lv2_register.c */

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

static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{ ppu_guest_call(opd, a0, a1, a2, a3); }

#ifdef _WIN32
/* RSX present backend (libs/video/rsx_d3d12_backend.c). Driven on the vblank
 * thread so the D3D12 device + window message pump live on one thread. */
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
extern "C" void cellGcm_rsx_process_fifo(void);   /* cellGcmSys.c: drain get->put */
extern "C" unsigned cellGcm_flip_request_count(void);
extern "C" int cellGcm_take_flip_pending(void);

static DWORD WINAPI vblank_ticker(LPVOID)
{
    int rsx_ok = (rsx_d3d12_backend_init(1280, 720, "cellmark (ps3recomp)") == 0);
    fprintf(stderr, "[rsx] backend init %s\n", rsx_ok ? "OK -- window open" : "FAILED");
    unsigned last_flip = 0;
    ULONGLONG next_tick = GetTickCount64();
    for (;;) {
        /* Fast cadence: titles fence EVERY render pass on an RSX label the
         * drain writes; at a 16ms drain each fence cost a frame's worth of
         * time (wave: six passes -> ~7 fps). Drain at ~2ms; vblank/flip
         * ticks stay on 16ms beats. */
        Sleep(2);
        /* Flip completion at the fast cadence too: the 16ms beat is only
         * vsync emulation and throttled the whole frame loop; the guest's
         * flip-status wait resolves as soon as the frame actually finished. */
        cellGcmTickFlip();
        ULONGLONG now = GetTickCount64();
        while ((long long)(now - next_tick) >= 0) {
            cellGcmTickVBlank();
            next_tick += 16;
            if ((long long)(now - next_tick) > 1000) { next_tick = now; break; }
        }
        if (rsx_ok) {
            if (rsx_d3d12_backend_pump_messages() != 0) { rsx_ok = 0; }
            /* Present a pending flip BEFORE draining further: the flip fires
             * at a get==put boundary, so the held batch is exactly the
             * completed frame. */
            {
                if (cellGcm_take_flip_pending()) {
                    rsx_d3d12_backend_present();
                    last_flip = cellGcm_flip_request_count();
                }
            }
            cellGcm_rsx_process_fifo();      /* execute the game's GCM commands */
            /* Boot fallback: before the first flip, present freely so the
             * window isn't stuck white. */
            unsigned fc = cellGcm_flip_request_count();
            if (fc == 0) rsx_d3d12_backend_present();
        }
    }
    return 0;
}

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
                    /* Scan the suspended thread's stack for boot-module return
                     * addresses to reconstruct the call chain when there's no
                     * OOB backtrace to lean on (some false positives expected). */
                    uint64_t* sp = (uint64_t*)ctx.Rsp;
                    int found = 0;
                    for (int k = 0; k < 0x8000 / 8 && found < 16; k++) {
                        uint64_t v = sp[k];
                        if (v < (uint64_t)self) continue;
                        HMODULE mm = NULL;
                        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           (LPCSTR)v, &mm);
                        if (mm == self) {
                            fprintf(stderr, "[WATCHDOG]       ret rva=0x%llX\n",
                                    (unsigned long long)(v - (uint64_t)self));
                            found++;
                        }
                    }
                } else {
                    char path[MAX_PATH] = "?";
                    if (m) GetModuleFileNameA(m, path, sizeof path);
                    const char* base = strrchr(path, '\\');
                    fprintf(stderr, "[WATCHDOG]   tid %5lu in %s\n",
                            (unsigned long)te.th32ThreadID, base ? base + 1 : path);
                }
            }
            ResumeThread(th);
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
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
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: %s <EBOOT.elf>\n", argv[0]); return 2; }

    /* wave is camera-optional: default the virtual camera OFF so it takes the
     * file-based cat-image path (matches RPCS3, which has no camera). Set
     * PS3_CAMERA=1 to feed it the synthetic EyeToy pattern instead. */
    if (!getenv("PS3_CAMERA"))
        _putenv("PS3_CAMERA=0");

#ifdef _WIN32
    SetUnhandledExceptionFilter(ydkj_crash_filter);
    signal(SIGABRT, ydkj_abort_handler);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: don't lose prints on kill */
#endif

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
    vm_base = (uint8_t*)calloc(1, 0xE0000000u);
    ppu_vm_size = 0xE0000000u;
#endif
    if (!vm_base) { printf("vm alloc failed\n"); return 1; }

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) { printf("load failed\n"); return 1; }

    derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_load_prx_modules();  /* real system PRX (libsre) -> guest RAM + exports */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT (sys_initialize_tls, ...) */
    ppu_fs_register();       /* cellFs VFS over the real game directory */
    lv2_init_syscalls();     /* real lv2 syscall table (semaphore/memory/fs/...) */

    /* Install the guest-callback hook and start the synthetic RSX vblank driver
     * so the game's frame loop advances (it no-ops until the game registers its
     * vblank/flip handlers during init). */
    g_ps3_guest_caller = harness_guest_caller;
#ifdef _WIN32
    CreateThread(NULL, 4u * 1024 * 1024, vblank_ticker, NULL, 0, NULL);
    CreateThread(NULL, 0, hang_watchdog, NULL, 0, NULL);
#endif

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n", entry, STACK_TOP);
    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);
    return 0;
}

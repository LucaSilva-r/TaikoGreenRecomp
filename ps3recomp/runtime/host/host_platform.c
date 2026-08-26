#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ps3emu/host_platform.h>

#include <errno.h>
#include <stdint.h>

#define PS3_GUEST_VM_SIZE  UINT64_C(0x100000000)
#define PS3_GUEST_VM_GUARD UINT64_C(0x00010000)

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

uint64_t ps3_host_monotonic_ns(void)
{
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (uint64_t)(((__int128)counter.QuadPart * 1000000000u) /
                      frequency.QuadPart);
}

void ps3_host_sleep_until_ns(uint64_t deadline_ns)
{
    for (;;) {
        uint64_t now = ps3_host_monotonic_ns();
        if (now >= deadline_ns) return;
        uint64_t remaining_ms = (deadline_ns - now) / 1000000u;
        if (remaining_ms) Sleep((DWORD)remaining_ms);
        else YieldProcessor();
    }
}

uint64_t ps3_host_thread_id(void) { return GetCurrentThreadId(); }
void ps3_host_cpu_relax(void) { YieldProcessor(); }

uintptr_t ps3_host_image_base(const void* address)
{
    HMODULE module = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)address, &module))
        return 0;
    return (uintptr_t)module;
}

size_t ps3_host_capture_backtrace(void** frames, size_t capacity)
{
    if (capacity > 62) capacity = 62;
    return (size_t)RtlCaptureStackBackTrace(0, (ULONG)capacity, frames, NULL);
}

void* ps3_host_reserve_guest_vm(void)
{
    return VirtualAlloc(NULL, (SIZE_T)PS3_GUEST_VM_SIZE, MEM_RESERVE,
                        PAGE_READWRITE);
}

void ps3_host_release_guest_vm(void* base)
{
    if (base) VirtualFree(base, 0, MEM_RELEASE);
}

#else

#ifndef __ANDROID__
#include <execinfo.h>
#endif
#include <dlfcn.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

uint64_t ps3_host_monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

void ps3_host_sleep_until_ns(uint64_t deadline_ns)
{
    struct timespec deadline = {
        .tv_sec = (time_t)(deadline_ns / 1000000000u),
        .tv_nsec = (long)(deadline_ns % 1000000000u),
    };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) == EINTR)
        ;
}

uint64_t ps3_host_thread_id(void)
{
#ifdef SYS_gettid
    return (uint64_t)syscall(SYS_gettid);
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}

void ps3_host_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

uintptr_t ps3_host_image_base(const void* address)
{
    Dl_info info;
    return dladdr(address, &info) && info.dli_fbase
        ? (uintptr_t)info.dli_fbase : 0;
}

size_t ps3_host_capture_backtrace(void** frames, size_t capacity)
{
#ifdef __ANDROID__
    (void)frames;
    (void)capacity;
    return 0;
#else
    if (capacity > INT32_MAX) capacity = INT32_MAX;
    int count = backtrace(frames, (int)capacity);
    return count > 0 ? (size_t)count : 0;
#endif
}

void* ps3_host_reserve_guest_vm(void)
{
    const size_t total = (size_t)(PS3_GUEST_VM_SIZE + PS3_GUEST_VM_GUARD);
    void* base = mmap(NULL, total, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) return NULL;
    if (mprotect(base, (size_t)PS3_GUEST_VM_SIZE, PROT_READ | PROT_WRITE) != 0) {
        munmap(base, total);
        return NULL;
    }
    return base;
}

void ps3_host_release_guest_vm(void* base)
{
    if (base)
        munmap(base, (size_t)(PS3_GUEST_VM_SIZE + PS3_GUEST_VM_GUARD));
}

#endif

uint64_t ps3_host_monotonic_ms(void)
{
    return ps3_host_monotonic_ns() / 1000000u;
}

void ps3_host_sleep_ms(uint32_t milliseconds)
{
    ps3_host_sleep_until_ns(ps3_host_monotonic_ns() +
                            (uint64_t)milliseconds * 1000000u);
}

void ps3_host_sleep_us(uint64_t microseconds)
{
    ps3_host_sleep_until_ns(ps3_host_monotonic_ns() + microseconds * 1000u);
}

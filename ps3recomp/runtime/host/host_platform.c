#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ps3emu/host_platform.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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

int ps3_host_apply_thread_affinity(const char* env_name, const char* role)
{
    const char* value = env_name ? getenv(env_name) : NULL;
    if (!value || !value[0]) return 0;
    (void)role;
    fprintf(stderr,
            "[affinity] %s=%s is ignored on Windows\n", env_name, value);
    return -1;
}

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
#include <sched.h>
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

int ps3_host_apply_thread_affinity(const char* env_name, const char* role)
{
    const char* value = env_name ? getenv(env_name) : NULL;
    if (!value || !value[0]) return 0;

    cpu_set_t set;
    CPU_ZERO(&set);
    const char* cursor = value;
    unsigned count = 0;
    while (*cursor) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') ++cursor;
        if (!*cursor) break;
        char* end = NULL;
        long first = strtol(cursor, &end, 10);
        if (end == cursor || first < 0 || first >= CPU_SETSIZE) goto invalid;
        long last = first;
        cursor = end;
        if (*cursor == '-') {
            long parsed = strtol(cursor + 1, &end, 10);
            if (end == cursor + 1 || parsed < first || parsed >= CPU_SETSIZE)
                goto invalid;
            last = parsed;
            cursor = end;
        }
        for (long cpu = first; cpu <= last; ++cpu) {
            CPU_SET((int)cpu, &set);
            ++count;
        }
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor && *cursor != ',') goto invalid;
    }
    if (!count) goto invalid;
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        fprintf(stderr,
                "[affinity] failed role=%s env=%s value=%s errno=%d\n",
                role ? role : "thread", env_name, value, errno);
        return -1;
    }
    fprintf(stderr, "[affinity] role=%s tid=%llu cpus=%s\n",
            role ? role : "thread",
            (unsigned long long)ps3_host_thread_id(), value);
    return 0;

invalid:
    fprintf(stderr, "[affinity] invalid env=%s value=%s\n", env_name, value);
    return -1;
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

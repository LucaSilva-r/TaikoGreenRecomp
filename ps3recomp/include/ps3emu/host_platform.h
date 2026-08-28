#ifndef PS3EMU_HOST_PLATFORM_H
#define PS3EMU_HOST_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic host clock. Deadlines passed to sleep_until_ns use this epoch. */
uint64_t ps3_host_monotonic_ns(void);
uint64_t ps3_host_monotonic_ms(void);
void ps3_host_sleep_until_ns(uint64_t deadline_ns);
void ps3_host_sleep_ms(uint32_t milliseconds);
void ps3_host_sleep_us(uint64_t microseconds);

uint64_t ps3_host_thread_id(void);
void ps3_host_cpu_relax(void);
/* Apply a CPU-list from an environment variable to the calling thread.
 * Lists use Linux taskset syntax (for example "4" or "0-3,6"). An unset
 * variable is a no-op. Returns zero for success/no-op and -1 on error. */
int ps3_host_apply_thread_affinity(const char* env_name, const char* role);
uintptr_t ps3_host_image_base(const void* address);
size_t ps3_host_capture_backtrace(void** frames, size_t capacity);

/* Reserve a zero-filled 4 GiB flat guest arena. On POSIX, an additional
 * inaccessible 64 KiB mapping immediately follows it as an overflow guard. */
void* ps3_host_reserve_guest_vm(void);
void ps3_host_release_guest_vm(void* base);

#ifdef __cplusplus
}
#endif

#endif

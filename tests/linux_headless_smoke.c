#include <ps3emu/host_platform.h>
#include "cellAudio.h"
#include "cellPad.h"
#include "rsx_commands.h"
#include "rsx_null_backend.h"
#include "rsx_recorder.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

uint8_t* vm_base;
int g_resv_store_active;
void ppu_resv_break_store(uint64_t address) { (void)address; }

uint32_t vm_read32(uint64_t address)
{
    uint32_t value;
    __builtin_memcpy(&value, vm_base + (uint32_t)address, sizeof(value));
    return __builtin_bswap32(value);
}
void vm_write8(uint64_t address, uint8_t value) { vm_base[(uint32_t)address] = value; }
void vm_write16(uint64_t address, uint16_t value)
{
    value = __builtin_bswap16(value);
    __builtin_memcpy(vm_base + (uint32_t)address, &value, sizeof(value));
}
void vm_write32(uint64_t address, uint32_t value)
{
    value = __builtin_bswap32(value);
    __builtin_memcpy(vm_base + (uint32_t)address, &value, sizeof(value));
}
uint32_t sys_event_find_queue_by_key(uint64_t key) { (void)key; return 0; }
int sys_event_queue_push_by_id(uint32_t id, uint64_t source, uint64_t data1,
                               uint64_t data2, uint64_t data3)
{
    (void)id; (void)source; (void)data1; (void)data2; (void)data3;
    return 0;
}

static int require(int condition, const char* message)
{
    if (!condition) fprintf(stderr, "headless smoke: %s\n", message);
    return condition ? 0 : 1;
}

int main(void)
{
    uint64_t t0 = ps3_host_monotonic_ns();
    ps3_host_sleep_until_ns(t0 + 2000000u);
    if (require(ps3_host_monotonic_ns() >= t0 + 2000000u,
                "monotonic deadline returned early")) return 1;

    vm_base = ps3_host_reserve_guest_vm();
    if (require(vm_base != NULL, "4 GiB guest VM reservation failed")) return 1;
    vm_base[0] = 0x12;
    vm_base[0xffffffffu] = 0x34;
    if (require(vm_base[0] == 0x12 && vm_base[0xffffffffu] == 0x34,
                "full guest range is not writable")) return 1;

    pid_t child = fork();
    if (child == 0) {
        vm_base[UINT64_C(0x100000000)] = 1;
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    if (require(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
                "top guard did not fault")) return 1;

    if (require(rsx_null_backend_init(1280, 720, "test") == 0,
                "null RSX init failed")) return 1;
    rsx_state state;
    rsx_state_init(&state);
    const uint32_t fifo[] = {
        (1u << 18) | NV4097_SET_COLOR_CLEAR_VALUE, 0xff102030u,
        (1u << 18) | NV4097_CLEAR_SURFACE, 0xf3u,
    };
    if (require(rsx_process_command_buffer(&state, fifo, sizeof(fifo)) == 2,
                "null RSX FIFO dispatch failed")) return 1;
    if (require(rsx_recorder_flush(0, 0) == 0,
                "null RSX batch submit failed")) return 1;
    uint64_t clears = 0;
    rsx_null_backend_get_counters(&clears, NULL, NULL, NULL);
    if (require(clears == 1, "null RSX clear callback was not consumed")) return 1;
    rsx_null_backend_shutdown();

    setenv("PS3RECOMP_NULL_AUDIO", "1", 1);
    if (require(cellAudioInit() == 0, "headless cellAudio init failed")) return 1;
    if (require(cellAudioQuit() == 0, "headless cellAudio quit failed")) return 1;
    if (require(cellPadInit(1) == 0, "headless cellPad init failed")) return 1;
    if (require(cellPadEnd() == 0, "headless cellPad end failed")) return 1;

    ps3_host_release_guest_vm(vm_base);
    puts("native Linux headless smoke passed");
    return 0;
}

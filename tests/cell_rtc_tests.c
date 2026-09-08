#include "cellRtc.h"
#include "ppu_memory.h"
#include <stdio.h>
#include <stdlib.h>

static uint8_t memory[4096];
uint8_t* vm_base = memory;
int g_resv_store_active = 0;
void ppu_resv_break_store(uint64_t addr) { (void)addr; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed: %s\n", #x); abort(); } } while (0)
#define TICK(x) ((CellRtcTick*)(uintptr_t)(x))

int main(void)
{
    const uint64_t base = (62135596800ULL + 1788869358ULL) * 1000000ULL;
    vm_write64(0x100, base);
    CHECK(cellRtcTickAddTicks(TICK(0x110), TICK(0x100), 0) == CELL_OK);
    CHECK(vm_read64(0x110) == base);
    CHECK(cellRtcTickAddTicks(TICK(0x110), TICK(0x110), -1234567) == CELL_OK);
    CHECK(vm_read64(0x110) == base - 1234567);
    CHECK(cellRtcTickAddTicks(NULL, TICK(0x100), 0) == CELL_EINVAL);
    CHECK(cellRtcTickAddTicks(TICK(0x110), NULL, 0) == CELL_EINVAL);
    // Green: current tick -> timezone seconds -> raw tick adjustment -> date.
    CHECK(cellRtcTickAddSeconds(TICK(0x110), TICK(0x100), 32400) == CELL_OK);
    CHECK(cellRtcTickAddTicks(TICK(0x120), TICK(0x110), 0) == CELL_OK);
    CHECK(cellRtcSetTick((CellRtcDateTime*)(uintptr_t)0x200, TICK(0x120)) == CELL_OK);
    CHECK(vm_read16(0x200) == 2026 && vm_read16(0x202) == 9 && vm_read16(0x204) == 8);
    const uint16_t second = vm_read16(0x20a);
    vm_write64(0x100, base + 1000000);
    CHECK(cellRtcTickAddSeconds(TICK(0x110), TICK(0x100), 32400) == CELL_OK);
    CHECK(cellRtcTickAddTicks(TICK(0x120), TICK(0x110), 0) == CELL_OK);
    CHECK(cellRtcSetTick((CellRtcDateTime*)(uintptr_t)0x200, TICK(0x120)) == CELL_OK);
    CHECK(vm_read16(0x20a) == (second + 1) % 60);
}

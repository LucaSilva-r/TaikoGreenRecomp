#pragma once
#include <cstdint>
struct ppu_context { uint64_t gpr[32]{}; uint32_t thread_id = 1; };
uint32_t vm_read32(uint32_t);
uint8_t vm_read8(uint32_t);
void vm_write32(uint32_t, uint32_t);
void vm_write8(uint32_t, uint8_t);
void func_008DA500(ppu_context*);

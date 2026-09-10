#pragma once
#include <cstdint>

// Diagnostic only: decoder lifetime guards prevent menu/other-song input.
void taiko_sync_test_start(uint32_t handle, const char* source);
void taiko_sync_test_stop(uint32_t handle);
uint64_t taiko_sync_test_consume(uint64_t now_ns);


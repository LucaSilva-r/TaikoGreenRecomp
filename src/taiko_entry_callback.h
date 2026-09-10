#ifndef TAIKO_ENTRY_CALLBACK_H
#define TAIKO_ENTRY_CALLBACK_H

#include <cstdint>

namespace taiko_entry {
// Green's {name, OPD} table starts at 0x00F93B5C, not 0x00F93B60.
inline constexpr uint32_t kSetNextScene = 0x002287BCu;

// Raw AVM value, BEFORE func_00397C04 converts it to the native callback type.
// Raw integer 3 -> native integer 2; raw boolean 2 -> native boolean 1.
inline constexpr bool is_plus_marker(uint32_t count, uint32_t raw_type,
                                     uint32_t value)
{
    return count == 4 && (raw_type & 0x7fffffffu) == 3 && value == 99;
}
}

#endif

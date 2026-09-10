#pragma once

#include <cstdint>
#include <cstdlib>
#include <mutex>

// Active PPU threads already own disjoint guest stacks. Leave the caller's
// linkage/red zone untouched and put the callback below its current frame.
// Host-only callers have no guest stack; serialize their legacy scratch area.
// Nested callbacks inherit the outer callback's current SP and need no lock.
class PpuCallbackStack {
    static std::mutex& host_mutex()
    {
        static std::mutex mutex;
        return mutex;
    }
    std::unique_lock<std::mutex> host_lock_;
    uint32_t top_;
public:
    explicit PpuCallbackStack(uint64_t active_sp)
        : host_lock_(host_mutex(), std::defer_lock)
    {
        if (active_sp) {
            if (active_sp < 0x210 || active_sp > UINT32_MAX) std::abort();
            top_ = (static_cast<uint32_t>(active_sp) - 0x200u) & ~15u;
        } else {
            host_lock_.lock();
            top_ = 0xcffe0000u;
        }
    }
    uint32_t top() const { return top_; }
};

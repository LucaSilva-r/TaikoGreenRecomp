#include "ppu_callback_stack.h"
#include <array>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <thread>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "failed: %s\n", #x); std::abort(); } } while (0)

int main()
{
    // Reproduce simultaneous score serialization and GCM callback entry.
    std::array<uint32_t, 2> tops{};
    std::barrier ready(2);
    auto callback = [&](unsigned i, uint32_t caller_sp) {
        PpuCallbackStack stack(caller_sp);
        tops[i] = stack.top();
        ready.arrive_and_wait();
        CHECK(tops[0] != tops[1]);
        CHECK(stack.top() + 0x200 <= caller_sp);
        CHECK((stack.top() & 15) == 0);
        // A callback can invoke another callback after building its own frame.
        PpuCallbackStack nested(stack.top() - 0x710);
        CHECK(nested.top() + 0x200 <= stack.top() - 0x710);
    };
    std::thread a(callback, 0, 0x0feffc00);
    std::thread b(callback, 1, 0xd010fa00);
    a.join(); b.join();

    // Host-only callbacks must never concurrently own the fallback area.
    std::atomic<unsigned> inside{0};
    auto host_callback = [&] {
        for (unsigned i = 0; i < 1000; ++i) {
            PpuCallbackStack stack(0);
            CHECK(inside.fetch_add(1) == 0);
            PpuCallbackStack nested(stack.top() - 0x100);
            CHECK(nested.top() < stack.top() - 0x100);
            std::this_thread::yield();
            CHECK(inside.fetch_sub(1) == 1);
        }
    };
    std::thread c(host_callback), d(host_callback);
    c.join(); d.join();
}

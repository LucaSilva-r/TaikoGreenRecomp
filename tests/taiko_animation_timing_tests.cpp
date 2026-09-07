#include "ppu_recomp.h"
#include <ps3emu/host_platform.h>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

extern "C" void taiko_project_flip_command();
extern "C" uint32_t taiko_animation_frame_ticks();
extern "C" void taiko_lumen_scale_frame_delta(ppu_context*);

static uint64_t now = 1000000000;
static bool boot_done;
extern "C" uint64_t ps3_host_monotonic_ns() { return now; }
extern "C" int ps3_frame_boot_fast_is_done() { return boot_done; }
extern "C" void ppu_register_function(uint64_t, void (*)(ppu_context*)) {}
extern "C" void ppu_set_project_register_hooks(void (*)(void)) {}
void func_003DF910(ppu_context*) {}
extern "C" uint8_t vm_read8(uint64_t) { return 0; }
extern "C" uint32_t vm_read32(uint64_t) { return 0; }
extern "C" uint64_t vm_read64(uint64_t) { return 0; }
extern "C" void vm_write8(uint64_t, uint8_t) {}

#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::abort(); \
} } while (0)

static void reset()
{
    boot_done = false;
    taiko_project_flip_command();
    CHECK(taiko_animation_frame_ticks() == 1);
    boot_done = true;
}

static unsigned frame(uint64_t delta)
{
    now += delta;
    taiko_project_flip_command();
    const auto ticks = taiko_animation_frame_ticks();
    // Multiple consumers must receive the same tick, not consume it.
    CHECK(taiko_animation_frame_ticks() == ticks);
    ppu_context ctx{};
    ctx.fpr[1] = 1.0;
    taiko_lumen_scale_frame_delta(&ctx);
    CHECK(ctx.fpr[1] == ticks);
    return ticks;
}

int main(int argc, char**)
{
    const bool enabled = argc == 1;
    setenv("TAIKO_ANIMATION_TIMING", enabled ? "1" : "0", 1);
    for (unsigned hz : {60u, 120u, 144u, 240u}) {
        reset();
        unsigned total = 0;
        uint64_t previous = 0;
        for (unsigned i = 1; i <= hz * 2; ++i) {
            const uint64_t elapsed = uint64_t(i) * 1000000000 / hz;
            total += frame(elapsed - previous);
            previous = elapsed;
        }
        CHECK(enabled ? (total >= 119 && total <= 120) : total == hz * 2);
    }
    if (enabled) {
        // Player Entry's prompt countdown expires below zero and reloads its
        // configured period. A 180-tick fixture must not restart during its
        // first three seconds, even with four render updates per authored tick.
        for (unsigned hz : {60u, 120u, 144u, 240u}) {
            reset();
            int32_t countdown = 180;
            unsigned repeats = 0;
            uint64_t previous = 0;
            for (unsigned i = 1; i <= hz * 10; ++i) {
                const uint64_t elapsed = uint64_t(i) * 1000000000 / hz;
                countdown -= frame(elapsed - previous);
                previous = elapsed;
                if (countdown < 0) { ++repeats; countdown = 180; }
                if (i <= hz * 3) CHECK(repeats == 0);
            }
            CHECK(repeats == 3);
        }
        reset();
        unsigned total = 0;
        // A transition spanning changing render rates retains its duration.
        for (unsigned i = 0; i < 120; ++i) total += frame(8333333);
        for (unsigned i = 0; i < 240; ++i) total += frame(4166667);
        CHECK(total >= 119 && total <= 120);
        CHECK(frame(100000000) == 4); // Existing bounded catch-up policy.
        CHECK(frame(300000000) == 1); // Loading gap is not replayed wholesale.
    }
    std::puts("animation timing passed");
}

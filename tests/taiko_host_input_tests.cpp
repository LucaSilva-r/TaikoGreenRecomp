#include "taiko_host_input.h"

#include <cstdio>

static bool expect(bool condition, const char* message)
{
    if (!condition) std::fprintf(stderr, "host input test: %s\n", message);
    return condition;
}

int main()
{
    taiko_host_input_reset();
    taiko_host_input_set_active(1);
    taiko_host_input_press(0, TAIKO_ACTION_HIT_SL, 1234);
    taiko_host_input_press(0, TAIKO_ACTION_HIT_SL, 5678);
    taiko_host_input_snapshot first{};
    taiko_host_input_consume(&first);
    if (!expect(first.active && first.levels[0] == TAIKO_ACTION_HIT_SL,
                "pressed level") ||
        !expect(first.rising[0] == TAIKO_ACTION_HIT_SL,
                "one latched edge for repeated down") ||
        !expect(first.hit_timestamp_ns[0][0] == 1234,
                "first edge timestamp retained")) return 1;

    taiko_host_input_snapshot held{};
    taiko_host_input_consume(&held);
    if (!expect(held.levels[0] == TAIKO_ACTION_HIT_SL && held.rising[0] == 0,
                "held key does not relatch")) return 1;

    taiko_host_input_release(0, TAIKO_ACTION_HIT_SL);
    taiko_host_input_press(0, TAIKO_ACTION_HIT_SL, 9999);
    taiko_host_input_snapshot second{};
    taiko_host_input_consume(&second);
    if (!expect(second.rising[0] == TAIKO_ACTION_HIT_SL &&
                second.hit_timestamp_ns[0][0] == 9999,
                "release enables next press edge")) return 1;

    std::puts("portable Taiko host input tests passed");
    return 0;
}

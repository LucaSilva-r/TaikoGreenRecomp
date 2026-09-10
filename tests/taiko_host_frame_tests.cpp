#include "taiko_overlay.h"

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

int main()
{
    setenv("TAIKO_OVERLAY_FONT", "fonts/font.ttf", 1);
    taiko_overlay_show_entry_menu(0);

    HostFrameInfo info{};
    assert(taiko_host_frame_copy(&info, nullptr, 0));
    assert(info.mode == HOST_FRAME_FULLSCREEN);
    assert(info.width == 1280 && info.height == 720);
    assert(info.pitch == info.width * 4u);

    std::vector<uint8_t> first((size_t)info.pitch * info.height);
    assert(taiko_host_frame_copy(&info, first.data(), first.size()));
    assert(!taiko_host_frame_copy(&info, first.data(), first.size() - 1u));

    std::atomic<bool> stop{false};
    std::thread publisher([&] {
        for (unsigned iteration = 0; iteration < 200; ++iteration)
            taiko_overlay_show_entry_menu(iteration & 1u);
        stop.store(true, std::memory_order_release);
    });

    std::vector<uint8_t> left(first.size()), right(first.size());
    while (!stop.load(std::memory_order_acquire)) {
        HostFrameInfo a{}, b{};
        assert(taiko_host_frame_copy(&a, left.data(), left.size()));
        assert(taiko_host_frame_copy(&b, right.data(), right.size()));
        assert(a.mode == HOST_FRAME_FULLSCREEN);
        assert(a.pitch == a.width * 4u);
        if (a.version == b.version)
            assert(std::memcmp(left.data(), right.data(), left.size()) == 0);
    }
    publisher.join();

    taiko_overlay_hide_host_screen();
    assert(!taiko_host_frame_copy(&info, nullptr, 0));
    assert(info.mode == HOST_FRAME_NONE);
    return 0;
}

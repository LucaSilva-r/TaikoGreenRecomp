#include "taiko_audio_offset.h"
#include "taiko_config.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace {

constexpr int kMinimumOffsetMs = 0;
constexpr int kMaximumOffsetMs = 1000;

std::once_flag g_init_once;
std::atomic<int> g_offset_ms{0};

bool parse_offset(const char* text, int& value)
{
    if (!text || !*text || *text == '-') return false;
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(text, &end, 0);
    if (errno || end == text || (*end && *end != '\n' && *end != '\r') ||
        parsed < kMinimumOffsetMs || parsed > kMaximumOffsetMs)
        return false;
    value = static_cast<int>(parsed);
    return true;
}

void initialize()
{
    taiko_config_load();
    int value = 0;
    const char* source = "default";
    if (const char* environment = std::getenv("TAIKO_AUDIO_OFFSET_MS")) {
        if (parse_offset(environment, value)) {
            source = "environment";
        } else {
            std::fprintf(stderr,
                         "[taiko_audio_offset] invalid "
                         "TAIKO_AUDIO_OFFSET_MS='%s'; using 0\n",
                         environment);
        }
    }
    g_offset_ms.store(value, std::memory_order_release);
    std::fprintf(stderr, "[taiko_audio_offset] %d ms (%s), config=%s\n",
                 value, source, taiko_config_path());
}

bool save_offset(int value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%d", value);
    return taiko_config_set("audio", "offset_ms", text) != 0;
}

int set_offset(int value)
{
    std::call_once(g_init_once, initialize);
    value = std::clamp(value, kMinimumOffsetMs, kMaximumOffsetMs);
    g_offset_ms.store(value, std::memory_order_release);
    const bool saved = save_offset(value);
    std::fprintf(stderr, "[taiko_audio_offset] set %d ms (%s)\n", value,
                 saved ? "saved" : "not saved");
    return value;
}

} // namespace

extern "C" int taiko_audio_offset_get_ms(void)
{
    std::call_once(g_init_once, initialize);
    return g_offset_ms.load(std::memory_order_acquire);
}

extern "C" int taiko_audio_offset_adjust_ms(int delta_ms)
{
    const int current = taiko_audio_offset_get_ms();
    return set_offset(current + delta_ms);
}

extern "C" int taiko_audio_offset_set_ms(int value_ms)
{
    return set_offset(value_ms);
}

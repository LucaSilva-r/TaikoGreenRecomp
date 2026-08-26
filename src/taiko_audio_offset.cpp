#include "taiko_audio_offset.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>

namespace {

constexpr int kMinimumOffsetMs = 0;
constexpr int kMaximumOffsetMs = 1000;

std::once_flag g_init_once;
std::atomic<int> g_offset_ms{0};
std::filesystem::path g_offset_path;

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

std::filesystem::path offset_path()
{
    if (const char* explicit_path = std::getenv("TAIKO_AUDIO_OFFSET_FILE"))
        if (*explicit_path) return explicit_path;
    if (const char* config = std::getenv("XDG_CONFIG_HOME"))
        if (*config)
            return std::filesystem::path(config) / "taikorecomp" /
                   "audio_offset_ms";
    if (const char* home = std::getenv("HOME"))
        if (*home)
            return std::filesystem::path(home) / ".config" / "taikorecomp" /
                   "audio_offset_ms";
    return "taiko_audio_offset_ms.cfg";
}

void initialize()
{
    g_offset_path = offset_path();
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
    } else {
        FILE* file = std::fopen(g_offset_path.string().c_str(), "rb");
        if (file) {
            char text[64] = {};
            if (std::fgets(text, sizeof(text), file) && parse_offset(text, value))
                source = "saved";
            else
                std::fprintf(stderr,
                             "[taiko_audio_offset] invalid saved value in %s; "
                             "using 0\n",
                             g_offset_path.string().c_str());
            std::fclose(file);
        }
    }
    g_offset_ms.store(value, std::memory_order_release);
    std::fprintf(stderr, "[taiko_audio_offset] %d ms (%s), file=%s\n",
                 value, source, g_offset_path.string().c_str());
}

bool save_offset(int value)
{
    std::error_code error;
    const std::filesystem::path parent = g_offset_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            std::fprintf(stderr,
                         "[taiko_audio_offset] cannot create %s: %s\n",
                         parent.string().c_str(), error.message().c_str());
            return false;
        }
    }

    std::filesystem::path temporary = g_offset_path;
    temporary += ".tmp";
    FILE* file = std::fopen(temporary.string().c_str(), "wb");
    if (!file) {
        std::fprintf(stderr, "[taiko_audio_offset] cannot write %s: %s\n",
                     temporary.string().c_str(), std::strerror(errno));
        return false;
    }
    const bool wrote = std::fprintf(file, "%d\n", value) > 0 &&
                       std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!wrote || !closed) {
        std::fprintf(stderr, "[taiko_audio_offset] write failed for %s\n",
                     temporary.string().c_str());
        std::filesystem::remove(temporary, error);
        return false;
    }

    std::filesystem::rename(temporary, g_offset_path, error);
    if (error) {
        /* Windows rename does not replace an existing file. The temporary is
         * complete before the old value is removed, so failure remains
         * recoverable and the next rename is the only exposed gap. */
        std::error_code remove_error;
        std::filesystem::remove(g_offset_path, remove_error);
        error.clear();
        std::filesystem::rename(temporary, g_offset_path, error);
    }
    if (error) {
        std::fprintf(stderr, "[taiko_audio_offset] cannot replace %s: %s\n",
                     g_offset_path.string().c_str(), error.message().c_str());
        return false;
    }
    return true;
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

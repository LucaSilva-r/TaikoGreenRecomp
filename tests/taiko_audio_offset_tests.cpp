#include "taiko_audio_offset.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

static bool expect(bool condition, const char* message)
{
    if (!condition) std::fprintf(stderr, "audio offset test: %s\n", message);
    return condition;
}

int main()
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("taiko-audio-offset-test-" + std::to_string(getpid()));
    const std::filesystem::path file = directory / "audio_offset_ms";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    FILE* initial = std::fopen(file.string().c_str(), "wb");
    if (!expect(initial && std::fputs("7\n", initial) >= 0,
                "create saved value"))
        return 1;
    std::fclose(initial);
    setenv("TAIKO_AUDIO_OFFSET_FILE", file.string().c_str(), 1);
    unsetenv("TAIKO_AUDIO_OFFSET_MS");

    if (!expect(taiko_audio_offset_get_ms() == 7, "load saved value") ||
        !expect(taiko_audio_offset_adjust_ms(5) == 12, "positive step") ||
        !expect(taiko_audio_offset_adjust_ms(-20) == 0, "lower clamp") ||
        !expect(taiko_audio_offset_set_ms(2000) == 1000, "upper clamp") ||
        !expect(taiko_audio_offset_set_ms(65) == 65, "explicit value"))
        return 1;

    FILE* saved = std::fopen(file.string().c_str(), "rb");
    char text[32] = {};
    const bool readable = saved && std::fgets(text, sizeof(text), saved);
    if (saved) std::fclose(saved);
    if (!expect(readable && std::string(text) == "65\n",
                "saved value is durable"))
        return 1;

    std::filesystem::remove_all(directory);
    std::puts("persistent Taiko audio offset tests passed");
    return 0;
}

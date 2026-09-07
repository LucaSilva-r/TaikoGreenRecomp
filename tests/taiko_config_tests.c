#include "taiko_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int expect(int condition, const char* message)
{
    if (!condition) fprintf(stderr, "config test: %s\n", message);
    return condition;
}

int main(int argc, char** argv)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/taiko-config-test-%ld.cfg", (long)getpid());
    if (argc > 1 && !strcmp(argv[1], "create")) {
        remove(path);
        setenv("TAIKO_CONFIG", path, 1);
        taiko_config_load();
        FILE* created = fopen(path, "rb");
        char contents[4096] = {0};
        if (created) {
            fread(contents, 1, sizeof(contents) - 1, created);
            fclose(created);
        }
        const int ok = expect(strstr(contents, "config_version = 1") != NULL,
                              "create embedded default") &&
                       expect(strstr(contents, "[audio]") != NULL,
                              "created complete schema");
        remove(path);
        return ok ? 0 : 1;
    }

    FILE* file = fopen(path, "wb");
    if (!file) return 1;
    fputs("# obsolete comment\n[meta]\nconfig_version = 0\n\n"
          "[game]\nboot_fast = 0\n\n"
          "[network]\nhost = cfg.example\nport = 8443\n\n"
          "[audio]\noffset_ms = 17\n\n"
          "obsolete_key = remove-me\n\n"
          "[environment]\nRSX_FPS_LOG = 1\n", file);
    fclose(file);

    setenv("TAIKO_CONFIG", path, 1);
    setenv("TAIKO_ONLINE_HOST", "environment.example", 1);
    unsetenv("TAIKO_BOOT_FAST");
    unsetenv("TAIKO_ONLINE_PORT");
    unsetenv("TAIKO_AUDIO_OFFSET_MS");
    unsetenv("RSX_FPS_LOG");
    taiko_config_load();

    int ok =
        expect(!strcmp(getenv("TAIKO_BOOT_FAST"), "0"), "named game setting") &&
        expect(!strcmp(getenv("TAIKO_ONLINE_HOST"), "environment.example"),
               "environment precedence") &&
        expect(!strcmp(getenv("TAIKO_ONLINE_PORT"), "8443"), "network setting") &&
        expect(!strcmp(getenv("TAIKO_AUDIO_OFFSET_MS"), "17"), "audio setting") &&
        expect(!strcmp(getenv("RSX_FPS_LOG"), "1"), "environment escape hatch") &&
        expect(taiko_config_set("audio", "offset_ms", "23"), "update setting");

    file = fopen(path, "rb");
    char contents[2048] = {0};
    if (file) {
        fread(contents, 1, sizeof(contents) - 1, file);
        fclose(file);
    }
    ok = ok && expect(strstr(contents, "config_version = 1") != NULL,
                      "repair schema version") &&
         expect(strstr(contents, "obsolete_key") == NULL,
                "drop obsolete setting") &&
         expect(strstr(contents, "# TaikoRecomp configuration") != NULL,
                "install embedded schema") &&
         expect(strstr(contents, "offset_ms = 23") != NULL,
                "persist replacement");
    remove(path);
    if (!ok) return 1;
    puts("unified Taiko config tests passed");
    return 0;
}

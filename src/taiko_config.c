#include "taiko_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#define access _access
#define F_OK 0
#include <io.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TAIKO_CONFIG_NAME "taiko_config.cfg"
#define TAIKO_CONFIG_VERSION 2

extern const unsigned char taiko_config_default_data[];
extern const unsigned taiko_config_default_size;

typedef struct config_binding {
    const char* section;
    const char* key;
    const char* environment;
} config_binding;

static const config_binding k_bindings[] = {
    {"game", "boot_fast", "TAIKO_BOOT_FAST"},
    {"game", "offline_complete", "TAIKO_OFFLINE_COMPLETE"},
    {"game", "plus_standalone", "TAIKO_PLUS_STANDALONE"},
    {"game", "host_frontend", "TAIKO_HOST_FRONTEND"},

    {"songs", "custom_folder", "TAIKO_CUSTOM_SONGS"},
    {"songs", "osu_lazer", "TAIKO_OSU_LAZER"},
    {"songs", "nijiiro", "TAIKO_NIJIIRO"},
    {"songs", "nijiiro_fumen_key", "TAIKO_NIJIIRO_FUMEN_KEY"},
    {"songs", "nijiiro_datatable_key", "TAIKO_NIJIIRO_DATATABLE_KEY"},

    {"network", "host", "TAIKO_ONLINE_HOST"},
    {"network", "port", "TAIKO_ONLINE_PORT"},
    {"network", "verify", "TAIKO_ONLINE_VERIFY"},
    {"network", "cacert", "TAIKO_ONLINE_CACERT"},
    {"network", "pairing_token", "TAIKO_PAIRING_TOKEN"},
    {"network", "cabinet_id", "TAIKO_CABINET_ID"},
    {"network", "dns_loopback", "TAIKO_DNS_LOOPBACK"},

    {"audio", "offset_ms", "TAIKO_AUDIO_OFFSET_MS"},
    {"audio", "decode", "TAIKO_AUDIO_DECODE"},
    {"audio", "spu", "TAIKO_AUDIO_SPU"},
    {"audio", "lookahead_blocks", "TAIKO_AUDIO_LOOKAHEAD_BLOCKS"},
    {"audio", "prebuffer_blocks", "TAIKO_AUDIO_PREBUFFER_BLOCKS"},
    {"audio", "pcm_cache_mb", "TAIKO_AUDIO_PCM_CACHE_MB"},
    {"audio", "async_previews", "TAIKO_AUDIO_ASYNC_PREVIEWS"},
    {"audio", "alsa_direct_device", "TAIKO_AUDIO_ALSA_DIRECT_DEVICE"},

    {"video", "gpu_driver", "TAIKO_GPU_DRIVER"},
    {"video", "fullscreen", "TAIKO_FULLSCREEN"},
    {"video", "hide_cursor", "TAIKO_HIDE_CURSOR"},
    {"video", "present_mode", "TAIKO_PRESENT_MODE"},
    {"video", "frames_in_flight", "TAIKO_FRAMES_IN_FLIGHT"},
    {"video", "vblank_hz", "TAIKO_VBLANK_HZ"},
    {"video", "boot_vblank_hz", "TAIKO_BOOT_VBLANK_HZ"},
    {"video", "animation_timing", "TAIKO_ANIMATION_TIMING"},
    {"video", "perf_overlay", "TAIKO_PERF_OVERLAY"},
    {"video", "character_outline", "TAIKO_GPU_CHARACTER_OUTLINE"},
    {"video", "character_filter_scissor", "TAIKO_GPU_CHARACTER_FILTER_SCISSOR"},
    {"video", "kms_present", "TAIKO_KMS_PRESENT"},
    {"video", "kms_atomic", "TAIKO_KMS_ATOMIC"},
    {"video", "kms_zero_copy", "TAIKO_KMS_ZERO_COPY"},
    {"video", "separate_upload_submit", "TAIKO_GPU_SEPARATE_UPLOAD_SUBMIT"},
    {"video", "upload_fence_wait", "TAIKO_GPU_UPLOAD_FENCE_WAIT"},
    {"video", "output_mode", "TAIKOS_OUTPUT_MODE"},

    {"input", "hit_value", "TAIKO_HIT_VALUE"},
    {"input", "hit_hold", "TAIKO_HIT_HOLD"},

    {"runtime", "vfs_root", "PS3_VFS_ROOT"},
    {"runtime", "vfs_layout", "PS3_VFS_LAYOUT"},
    {"runtime", "toc_set", "PS3_TOC_SET"},
    {"runtime", "no_spill", "FLOW_NOSPILL"},
    {"runtime", "fs_yield", "TAIKO_FS_YIELD"},
    {"runtime", "null_rsx", "PS3RECOMP_NULL_RSX"},
    {"runtime", "null_audio", "PS3RECOMP_NULL_AUDIO"},
    {"runtime", "cpu_render_affinity", "TAIKO_CPU_RENDER_AFFINITY"},
    {"runtime", "cpu_frame_affinity", "TAIKO_CPU_FRAME_AFFINITY"},
    {"runtime", "cpu_draw_affinity", "TAIKO_CPU_DRAW_AFFINITY"},
    {"runtime", "cpu_main_affinity", "TAIKO_CPU_MAIN_AFFINITY"},
    {"runtime", "cpu_vsync_affinity", "TAIKO_CPU_VSYNC_AFFINITY"},
    {"runtime", "cpu_ppu_affinity", "TAIKO_CPU_PPU_AFFINITY"},
    {"runtime", "cpu_spu_affinity", "TAIKO_CPU_SPU_AFFINITY"},
    {"runtime", "cpu_audio_affinity", "TAIKO_CPU_AUDIO_AFFINITY"},
    {"runtime", "cpu_preview_affinity", "TAIKO_CPU_PREVIEW_AFFINITY"},
};

static int g_loaded;
static char g_path[PATH_MAX];

static int config_set_internal(const char* section_name, const char* key_name,
                               const char* value);

static char* trim(char* text)
{
    while (*text && isspace((unsigned char)*text)) ++text;
    char* end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return text;
}

static void lowercase(char* text)
{
    for (; *text; ++text) *text = (char)tolower((unsigned char)*text);
}

static int safe_environment_name(const char* name)
{
    if (!name[0] || !(isalpha((unsigned char)name[0]) || name[0] == '_'))
        return 0;
    for (const char* p = name + 1; *p; ++p)
        if (!(isalnum((unsigned char)*p) || *p == '_')) return 0;
    return 1;
}

static void set_environment_default(const char* name, const char* value)
{
    if (!value[0] || getenv(name)) return;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
}

static const char* binding_environment(const char* section, const char* key)
{
    for (size_t i = 0; i < sizeof(k_bindings) / sizeof(k_bindings[0]); ++i)
        if (!strcmp(section, k_bindings[i].section) &&
            !strcmp(key, k_bindings[i].key))
            return k_bindings[i].environment;
    return NULL;
}

static int known_setting(const char* section, const char* key)
{
    return binding_environment(section, key) != NULL ||
           (!strcmp(section, "environment") && safe_environment_name(key));
}

static int executable_directory(char* output, size_t size)
{
#ifdef _WIN32
    DWORD length = GetModuleFileNameA(NULL, output, (DWORD)size);
    if (!length || length >= size) return 0;
#elif defined(__linux__) && !defined(__ANDROID__)
    ssize_t length = readlink("/proc/self/exe", output, size - 1);
    if (length <= 0 || (size_t)length >= size) return 0;
    output[length] = '\0';
#else
    (void)output;
    (void)size;
    return 0;
#endif
    char* slash = strrchr(output, '/');
#ifdef _WIN32
    char* backslash = strrchr(output, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
#endif
    if (!slash) return 0;
    *slash = '\0';
    return 1;
}

#if defined(__linux__) && !defined(__ANDROID__)
static int game_argument_directory(char* output, size_t size)
{
    FILE* file = fopen("/proc/self/cmdline", "rb");
    if (!file) return 0;
    size_t used = fread(output, 1, size - 1, file);
    fclose(file);
    if (!used) return 0;
    output[used] = '\0';
    size_t first = strnlen(output, used);
    if (first >= used || first + 1 >= used) return 0;
    char* argument = output + first + 1;
    size_t remaining = used - first - 1;
    if (strnlen(argument, remaining) >= remaining) return 0;
    memmove(output, argument, strlen(argument) + 1);
    char* slash = strrchr(output, '/');
    if (!slash) return 0;
    *slash = '\0';
    return 1;
}
#endif

static void select_path(void)
{
    const char* explicit_path = getenv("TAIKO_CONFIG");
    if (explicit_path && explicit_path[0]) {
        snprintf(g_path, sizeof(g_path), "%s", explicit_path);
        return;
    }

    char directory[PATH_MAX];
    char executable_path[PATH_MAX] = "";
    char game_path[PATH_MAX] = "";
    if (executable_directory(directory, sizeof(directory))) {
        snprintf(executable_path, sizeof(executable_path), "%s/%s", directory,
                 TAIKO_CONFIG_NAME);
        if (access(executable_path, F_OK) == 0) {
            snprintf(g_path, sizeof(g_path), "%s", executable_path);
            return;
        }
    }
#if defined(__linux__) && !defined(__ANDROID__)
    /* Developer builds commonly live outside the dump. Accept the config in
     * the EBOOT/USRDIR directory without requiring a path environment value. */
    if (game_argument_directory(directory, sizeof(directory))) {
        snprintf(game_path, sizeof(game_path), "%s/%s", directory,
                 TAIKO_CONFIG_NAME);
        if (access(game_path, F_OK) == 0) {
            snprintf(g_path, sizeof(g_path), "%s", game_path);
            return;
        }
    }
#endif
    if (access(TAIKO_CONFIG_NAME, F_OK) == 0) {
        snprintf(g_path, sizeof(g_path), "%s", TAIKO_CONFIG_NAME);
    } else if (game_path[0]) {
        snprintf(g_path, sizeof(g_path), "%s", game_path);
    } else if (executable_path[0]) {
        snprintf(g_path, sizeof(g_path), "%s", executable_path);
    } else {
        snprintf(g_path, sizeof(g_path), "%s", TAIKO_CONFIG_NAME);
    }
}

static int disabled_path(void)
{
#ifdef _WIN32
    return !_stricmp(g_path, "NUL") || !_stricmp(g_path, "NUL:");
#else
    return !strcmp(g_path, "/dev/null");
#endif
}

static int replace_with_embedded_default(void)
{
    char temporary[PATH_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", g_path) >=
        (int)sizeof(temporary)) return 0;
    FILE* output = fopen(temporary, "wb");
    if (!output) return 0;
#ifdef _WIN32
    _chmod(temporary, _S_IREAD | _S_IWRITE);
#else
    chmod(temporary, 0600);
#endif
    const int complete =
        fwrite(taiko_config_default_data, 1, taiko_config_default_size, output) ==
        taiko_config_default_size;
    const int flushed = fflush(output) == 0;
    const int closed = fclose(output) == 0;
    const int wrote = complete && flushed && closed;
    if (!wrote) {
        remove(temporary);
        return 0;
    }
#ifdef _WIN32
    remove(g_path);
#endif
    if (rename(temporary, g_path) != 0) {
        remove(temporary);
        return 0;
    }
    return 1;
}

typedef struct saved_setting {
    char section[64];
    char key[128];
    char value[1024];
} saved_setting;

static unsigned read_saved_settings(FILE* file, saved_setting* saved,
                                    unsigned capacity, int* version)
{
    char section[64] = "";
    char line[2048];
    unsigned count = 0;
    *version = -1;
    rewind(file);
    while (fgets(line, sizeof(line), file)) {
        char* hash = strchr(line, '#');
        char* semicolon = strchr(line, ';');
        if (hash && (!semicolon || hash < semicolon)) *hash = '\0';
        else if (semicolon) *semicolon = '\0';
        char* content = trim(line);
        if (!content[0]) continue;
        if (content[0] == '[') {
            char* close = strchr(content + 1, ']');
            if (!close) continue;
            *close = '\0';
            snprintf(section, sizeof(section), "%s", trim(content + 1));
            lowercase(section);
            continue;
        }
        char* equals = strchr(content, '=');
        if (!equals) continue;
        *equals = '\0';
        char* key = trim(content);
        char* value = trim(equals + 1);
        if (strcmp(section, "environment")) lowercase(key);
        if (!strcmp(section, "meta") && !strcmp(key, "config_version")) {
            *version = atoi(value);
            continue;
        }
        if (!known_setting(section, key) || count == capacity) continue;
        snprintf(saved[count].section, sizeof(saved[count].section), "%s", section);
        snprintf(saved[count].key, sizeof(saved[count].key), "%s", key);
        snprintf(saved[count].value, sizeof(saved[count].value), "%s", value);
        ++count;
    }
    return count;
}

static void import_legacy_files(void)
{
    char directory[PATH_MAX];
    snprintf(directory, sizeof(directory), "%s", g_path);
    char* slash = strrchr(directory, '/');
#ifdef _WIN32
    char* backslash = strrchr(directory, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
#endif
    if (slash) *slash = '\0';
    else snprintf(directory, sizeof(directory), ".");

    char legacy[PATH_MAX];
    if (snprintf(legacy, sizeof(legacy), "%s/taiko_online.cfg", directory) <
        (int)sizeof(legacy)) {
        FILE* file = fopen(legacy, "rb");
        if (file) {
            char line[2048];
            int imported = 1;
            while (fgets(line, sizeof(line), file)) {
                char* hash = strchr(line, '#');
                if (hash) *hash = '\0';
                char* equals = strchr(line, '=');
                if (!equals) continue;
                *equals = '\0';
                char* key = trim(line);
                char* value = trim(equals + 1);
                const char* section = !strcmp(key, "boot_fast") ? "game" : "network";
                if ((!strcmp(section, "game") && !strcmp(key, "boot_fast")) ||
                    (!strcmp(section, "network") && binding_environment(section, key)))
                    imported = config_set_internal(section, key, value) && imported;
            }
            fclose(file);
            if (imported && remove(legacy) == 0)
                fprintf(stderr, "[taiko_config] imported and removed %s\n", legacy);
            else if (imported)
                fprintf(stderr, "[taiko_config] imported %s\n", legacy);
        }
    }
    if (snprintf(legacy, sizeof(legacy), "%s/taiko_audio_offset_ms.cfg", directory) <
        (int)sizeof(legacy)) {
        FILE* file = fopen(legacy, "rb");
        if (file) {
            char value[64];
            int imported = 0;
            if (fgets(value, sizeof(value), file)) {
                char* parsed_end = NULL;
                char* text = trim(value);
                long offset = strtol(text, &parsed_end, 10);
                if (parsed_end != text && !*parsed_end && offset >= 0 &&
                    offset <= 1000)
                    imported = config_set_internal("audio", "offset_ms", text);
            }
            fclose(file);
            if (imported && remove(legacy) == 0)
                fprintf(stderr, "[taiko_config] imported and removed %s\n", legacy);
            else if (imported)
                fprintf(stderr, "[taiko_config] imported %s\n", legacy);
        }
    }
}

static FILE* create_or_repair(void)
{
    FILE* file = fopen(g_path, "rb");
    if (!file) {
        if (!replace_with_embedded_default()) return NULL;
        import_legacy_files();
        fprintf(stderr, "[taiko_config] created %s\n", g_path);
        return fopen(g_path, "rb");
    }

    saved_setting saved[128];
    int version;
    unsigned count = read_saved_settings(file, saved, 128, &version);
    if (version == TAIKO_CONFIG_VERSION) {
        rewind(file);
        return file;
    }
    fclose(file);
    if (!replace_with_embedded_default()) return fopen(g_path, "rb");
    for (unsigned i = 0; i < count; ++i)
        config_set_internal(saved[i].section, saved[i].key, saved[i].value);
    fprintf(stderr, "[taiko_config] repaired %s (version %d -> %d)\n",
            g_path, version, TAIKO_CONFIG_VERSION);
    return fopen(g_path, "rb");
}

void taiko_config_load(void)
{
    if (g_loaded) return;
    g_loaded = 1;
    select_path();

    if (disabled_path()) return;
    FILE* file = create_or_repair();
    if (!file) {
        if (getenv("TAIKO_CONFIG"))
            fprintf(stderr, "[taiko_config] cannot open %s: %s\n", g_path,
                    strerror(errno));
        return;
    }

    char section[64] = "";
    char line[2048];
    unsigned line_number = 0;
    while (fgets(line, sizeof(line), file)) {
        ++line_number;
        char* hash = strchr(line, '#');
        char* semicolon = strchr(line, ';');
        if (hash && (!semicolon || hash < semicolon)) *hash = '\0';
        else if (semicolon) *semicolon = '\0';
        char* content = trim(line);
        if (!content[0]) continue;
        if (content[0] == '[') {
            char* close = strchr(content + 1, ']');
            if (!close) continue;
            *close = '\0';
            snprintf(section, sizeof(section), "%s", trim(content + 1));
            lowercase(section);
            continue;
        }
        char* equals = strchr(content, '=');
        if (!equals) continue;
        *equals = '\0';
        char* key = trim(content);
        char* value = trim(equals + 1);
        if (strcmp(section, "environment")) lowercase(key);
        if (!strcmp(section, "meta") && !strcmp(key, "config_version"))
            continue;
        if (!strcmp(section, "environment")) {
            if (safe_environment_name(key)) set_environment_default(key, value);
            else fprintf(stderr, "[taiko_config] invalid environment name at %s:%u\n",
                         g_path, line_number);
            continue;
        }
        const char* environment = binding_environment(section, key);
        if (environment) set_environment_default(environment, value);
        else fprintf(stderr, "[taiko_config] unknown setting %s.%s at %s:%u\n",
                     section, key, g_path, line_number);
    }
    fclose(file);
    fprintf(stderr, "[taiko_config] loaded %s\n", g_path);
}

const char* taiko_config_path(void)
{
    taiko_config_load();
    return g_path;
}

static int section_line(const char* line, const char* wanted)
{
    char copy[256];
    snprintf(copy, sizeof(copy), "%s", line);
    char* content = trim(copy);
    if (*content++ != '[') return 0;
    char* close = strchr(content, ']');
    if (!close) return 0;
    *close = '\0';
    lowercase(content);
    return !strcmp(trim(content), wanted);
}

static int key_line(const char* line, const char* wanted)
{
    char copy[2048];
    snprintf(copy, sizeof(copy), "%s", line);
    char* content = trim(copy);
    if (!content[0] || content[0] == '#' || content[0] == ';' ||
        content[0] == '[') return 0;
    char* equals = strchr(content, '=');
    if (!equals) return 0;
    *equals = '\0';
    content = trim(content);
    lowercase(content);
    return !strcmp(content, wanted);
}

static int config_set_internal(const char* section_name, const char* key_name,
                               const char* value)
{
    if (!section_name || !key_name || !value || !section_name[0] ||
        !key_name[0] || strchr(section_name, '\n') || strchr(key_name, '\n') ||
        strchr(value, '\n')) return 0;

    char section[64];
    char key[128];
    snprintf(section, sizeof(section), "%s", section_name);
    snprintf(key, sizeof(key), "%s", key_name);
    lowercase(section);
    if (strcmp(section, "environment")) lowercase(key);

    char temporary[PATH_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", g_path) >=
        (int)sizeof(temporary)) return 0;
    FILE* input = fopen(g_path, "rb");
    FILE* output = fopen(temporary, "wb");
    if (!output) return 0;
#ifdef _WIN32
    _chmod(temporary, _S_IREAD | _S_IWRITE);
#else
    chmod(temporary, 0600);
#endif

    int in_section = 0;
    int found_section = 0;
    int replaced = 0;
    int last_was_newline = 1;
    char line[2048];
    while (input && fgets(line, sizeof(line), input)) {
        char section_probe[2048];
        snprintf(section_probe, sizeof(section_probe), "%s", line);
        if (trim(section_probe)[0] == '[') {
            const int next_is_section = section_line(line, section);
            if (in_section && !replaced) {
                fprintf(output, "%s = %s\n", key, value);
                replaced = 1;
            }
            in_section = next_is_section;
            if (in_section) found_section = 1;
        }
        if (in_section && key_line(line, key)) {
            fprintf(output, "%s = %s\n", key, value);
            replaced = 1;
        } else {
            fputs(line, output);
        }
        size_t length = strlen(line);
        last_was_newline = length && line[length - 1] == '\n';
    }
    if (input) fclose(input);
    if (!replaced) {
        if (!last_was_newline) fputc('\n', output);
        if (!found_section) fprintf(output, "\n[%s]\n", section);
        fprintf(output, "%s = %s\n", key, value);
    }
    const int wrote = fflush(output) == 0 && fclose(output) == 0;
    if (!wrote) {
        remove(temporary);
        return 0;
    }
#ifdef _WIN32
    remove(g_path);
#endif
    if (rename(temporary, g_path) != 0) {
        remove(temporary);
        return 0;
    }
    return 1;
}

int taiko_config_set(const char* section_name, const char* key_name,
                     const char* value)
{
    taiko_config_load();
    return config_set_internal(section_name, key_name, value);
}

/* Configuration must precede title constructors that cache getenv() values. */
#ifndef TAIKO_CONFIG_NO_CONSTRUCTOR
__attribute__((constructor(101)))
static void taiko_config_constructor(void)
{
    taiko_config_load();
}
#endif

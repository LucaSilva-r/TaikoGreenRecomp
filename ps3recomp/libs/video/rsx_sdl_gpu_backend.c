#include "rsx_sdl_gpu_backend.h"

#ifdef PS3RECOMP_RSX_BACKEND_SDL_GPU

#include "rsx_recorder.h"
#include "rsx_render_batch.h"
#include "rsx_vp_decompiler.h"
#include "rsx_fp_decompiler.h"
#ifdef RSX_SDL_KMS_PRESENT
#include "rsx_kms_present.h"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3_shadercross/SDL_shadercross.h>

#ifdef RSX_SDL_KMS_PRESENT
/* Local SDL 3.4.10 Vulkan extension applied by the Pi dependency build. */
extern bool SDL_GPUVulkanExportTextureDMABUF(
    SDL_GPUDevice* device, SDL_GPUTexture* texture, int* fd, Uint32* pitch,
    Uint64* offset, Uint64* modifier)
#if defined(__GNUC__)
    __attribute__((weak))
#endif
    ;
#endif

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#define SDL_RSX_WIDTH 1280u
#define SDL_RSX_HEIGHT 720u
#define SDL_RSX_QUEUE_DEPTH 4u
#define SDL_RSX_MAX_SURFACES 256u
#define SDL_RSX_MAX_SHADERS 512u
#define SDL_RSX_MAX_PIPELINES 1024u
#define SDL_RSX_MAX_TEXTURES 1024u
#define SDL_RSX_MAX_SAMPLERS 64u
#define SDL_RSX_KMS_MODIFIERS 64u
#define SDL_RSX_SHADER_DIALECT_VERSION 3u
#define SDL_RSX_PACE_SAMPLES 256u
#define FPS_OVERLAY_WIDTH 128
#define FPS_OVERLAY_HEIGHT 40
#define FPS_GLYPH_SCALE 4
#define SDL_RSX_CHARACTER_OUTLINE_FP UINT64_C(0x5DCB6858EEF22438)
#define SDL_RSX_CHARACTER_MESH_OUTLINE_SKINNED_FP \
    UINT64_C(0x5B1C86BF4A00F902)
#define SDL_RSX_CHARACTER_MESH_OUTLINE_RIGID_FP \
    UINT64_C(0xC319DC6592122177)
#define SDL_RSX_CHARACTER_COMPOSITE_VP UINT64_C(0x4D27AA0BFB40F830)
#define SDL_RSX_CHARACTER_COMPOSITE_FP UINT64_C(0x19BF731FEF02629F)
#define SDL_RSX_DIRECT_COPY_FP UINT64_C(0x1533A97E809FFB4A)
static const u8 s_direct_copy_fp[] = {
    0x9e, 0x01, 0x17, 0x00, 0xc8, 0x01, 0x1c, 0x9d,
    0xc8, 0x00, 0x00, 0x01, 0xc8, 0x00, 0x3f, 0xe1,
};
/* Four, not three: with three, the slot chosen for presentation is the same
 * slot the next batch renders into, so a frame that runs long lets the next
 * clear and draws land in the texture the present blit is still reading. */
#define SDL_RSX_PRESENT_SLOTS 4u

typedef struct shader_entry {
    u64 hash;
    unsigned stage;
    unsigned flags;
    SDL_GPUShader* shader;
    SDL_ShaderCross_GraphicsShaderResourceInfo resources;
    u8 sampler_units[4]; /* dense SDL slot -> sparse RSX texture unit */
    u8 sampler_unit_count;
    /* Dense storage-buffer slot -> guest vp_c[] slot. Address-indexed vertex
     * programs retain the identity mapping for the complete bank. */
    u16 vertex_constant_units[RSX_BATCH_VP_CONSTANTS];
    u16 vertex_constant_count;
} shader_entry;

typedef struct pipeline_entry {
    rsx_pipeline_key key;
    SDL_GPUGraphicsPipeline* pipeline;
} pipeline_entry;

typedef struct texture_entry {
    u64 hash;
    u32 width;
    u32 height;
    rsx_texture_format format;
    SDL_GPUTexture* texture;
    u64 last_used;
} texture_entry;

typedef struct sampler_entry {
    u32 address;
    SDL_GPUSampler* sampler;
} sampler_entry;

/* Title-owned input API. Keeping this C boundary here lets the generic RSX
 * recorder remain independent of SDL and of Taiko's guest-facing USIO HLE. */
extern void taiko_host_input_set_active(int active);
extern void taiko_host_input_press(unsigned player, unsigned actions,
                                   unsigned long long timestamp_ns);
extern void taiko_host_input_release(unsigned player, unsigned actions);

enum {
    TAIKO_HIT_SL = 1u << 0, TAIKO_HIT_CL = 1u << 1,
    TAIKO_HIT_CR = 1u << 2, TAIKO_HIT_SR = 1u << 3,
    TAIKO_ENTER = 1u << 4, TAIKO_SERVICE = 1u << 5,
    TAIKO_TEST = 1u << 6, TAIKO_COIN = 1u << 7,
    TAIKO_UP = 1u << 8, TAIKO_DOWN = 1u << 9
};

typedef struct queued_batch {
    rsx_render_batch batch;
    Uint64 enqueue_ns;
    int occupied;
} queued_batch;

typedef struct pace_sample {
    Uint64 submit_interval_ns;
    Uint64 present_interval_ns;
    Uint64 queue_wait_ns;
    Uint64 execute_ns;
    Uint64 present_cpu_ns;
} pace_sample;

typedef struct gpu_surface {
    rsx_surface_ref ref;
    SDL_GPUTexture* texture;
} gpu_surface;

typedef struct gamepad_slot {
    SDL_Gamepad* handle;
    SDL_JoystickID id;
    unsigned levels;
} gamepad_slot;

typedef struct dynamic_upload {
    SDL_GPUBuffer* buffer;
    SDL_GPUTransferBuffer* transfer;
    u32 capacity;
} dynamic_upload;

typedef struct sdl_rsx_state {
    SDL_Window* window;
    SDL_GPUDevice* device;
    SDL_Mutex* queue_mutex;
    SDL_Condition* queue_space;
    SDL_Condition* snapshot_done;
    queued_batch queue[SDL_RSX_QUEUE_DEPTH];
    unsigned queue_limit;   /* 0 until init; queue_depth() treats that as full depth */
    unsigned queue_read;
    unsigned queue_write;
    unsigned queue_count;
    Uint32 wake_event;
    gpu_surface surfaces[SDL_RSX_MAX_SURFACES];
    unsigned surface_count;
    SDL_GPUTexture* display;
    SDL_GPUTexture* presentation_override;
    int fence_present;
    SDL_GPUTexture* presentation_slots[SDL_RSX_PRESENT_SLOTS];
    SDL_GPUFence* presentation_fences[SDL_RSX_PRESENT_SLOTS];
    unsigned presentation_write;
    unsigned presentation_last;
    unsigned presentation_submitted;
    int presentation_valid;
    int presentation_pipeline_disabled;
    gamepad_slot gamepads[2];
#if defined(__linux__)
    int evdev_keyboards[16];
    int evdev_indices[16];          /* /dev/input/eventN, to skip duplicates */
    unsigned evdev_keyboard_count;
    long evdev_dir_mtime;           /* /dev/input mtime; rescan only on change */
    Uint64 evdev_rescan_ns;
#endif
    shader_entry shaders[SDL_RSX_MAX_SHADERS];
    unsigned shader_count;
    pipeline_entry pipelines[SDL_RSX_MAX_PIPELINES];
    unsigned pipeline_count;
    texture_entry textures[SDL_RSX_MAX_TEXTURES];
    unsigned texture_count;
    unsigned texture_limit;
    u64 texture_use_clock;
    u64 texture_evictions;
    SDL_GPUTexture* white_texture;
    SDL_GPUSampler* default_sampler;
    dynamic_upload vertex_constants_upload;
    dynamic_upload vertices_upload;
    sampler_entry samplers[SDL_RSX_MAX_SAMPLERS];
    unsigned sampler_count;
    unsigned errors;
    char base_title[160];
    Uint64 fps_window_start_ns;
    Uint64 fps_window_frames;
    Uint64 perf_batches;
    Uint64 perf_prepare_ns;
    Uint64 perf_constants_ns;
    Uint64 perf_vertex_bytes;
    Uint64 perf_constant_bytes;
    int kms_present;
    int kms_zero_copy;
    uint64_t kms_modifiers[SDL_RSX_KMS_MODIFIERS];
    unsigned kms_modifier_count;
    Uint64 perf_kms_render_ns;
    Uint64 perf_kms_download_ns;
    Uint64 perf_kms_wait_ns;
    Uint64 perf_kms_scanout_wait_ns;
    Uint64 perf_kms_copy_ns;
    Uint64 perf_kms_flip_ns;
    Uint64 perf_kms_frames;
    int kms_gpu_idle;
    /* Presentation through KMS costs a GPU download and a CPU copy. Neither
     * belongs on the critical path: the download is waited for one frame late,
     * and the copy runs on a worker thread while the next frame renders. */
    struct kms_slot {
        SDL_GPUTransferBuffer* transfer;
        SDL_GPUFence* fence;
        SDL_GPUTexture* source;
        Uint32 width;
        Uint32 height;
        Uint32 pitch;
        Uint32 bytes;
        uint32_t fps_overlay[FPS_OVERLAY_WIDTH * FPS_OVERLAY_HEIGHT];
        int fps_overlay_enabled;
        int busy;
    } kms_slots[3];
    unsigned kms_write;
    /* Display targets for the KMS path. get_surface() aliases both guest
     * display buffers onto one texture, so with a download in flight the next
     * frame would render straight into the texture being read. Rotate, and
     * never hand back a target whose download has not been consumed. */
    SDL_GPUTexture* kms_display[4];
    int kms_display_reader[4];
    unsigned kms_display_index;
    unsigned kms_display_count;
    int kms_pending[3];
    unsigned kms_pending_count;
    SDL_Thread* kms_thread;
    SDL_Mutex* kms_mutex;
    SDL_Condition* kms_work;
    SDL_Condition* kms_free;
    int kms_stop;
    Uint64 perf_vertices_ns;
    Uint64 perf_render_ns;
    Uint64 perf_present_ns;
    Uint64 perf_acquire_ns;
    Uint64 perf_blit_ns;
    Uint64 perf_fence_ns;
    Uint64 perf_render_passes;
    Uint64 perf_blits;
    Uint64 perf_copy_passes;
    Uint64 perf_submits;
    unsigned last_batch_draws;
    int fps_log;
    int perf_overlay;
    int pace_trace;
    Uint64 pace_window_start_ns;
    Uint64 pace_last_enqueue_ns;
    Uint64 pace_last_present_ns;
    pace_sample pace_samples[SDL_RSX_PACE_SAMPLES];
    unsigned pace_sample_count;
    rsx_render_batch* snapshot_target;
    int snapshot_pending;
    int snapshot_result;
    int initialized;
    int stopping;
} sdl_rsx_state;

static sdl_rsx_state s_sdl;

typedef struct pace_stats {
    double mean_ms;
    double p50_ms;
    double p95_ms;
    double max_ms;
} pace_stats;

static int compare_u64(const void* left, const void* right)
{
    Uint64 a = *(const Uint64*)left;
    Uint64 b = *(const Uint64*)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static pace_stats summarize_pace(const pace_sample* samples, unsigned count,
                                 size_t field_offset)
{
    Uint64 sorted[SDL_RSX_PACE_SAMPLES];
    long double sum = 0.0;
    if (count > SDL_RSX_PACE_SAMPLES) count = SDL_RSX_PACE_SAMPLES;
    for (unsigned i = 0; i < count; ++i) {
        const Uint64* value = (const Uint64*)((const u8*)&samples[i] + field_offset);
        sorted[i] = *value;
        sum += (long double)*value;
    }
    qsort(sorted, count, sizeof(sorted[0]), compare_u64);
    pace_stats result = {0};
    if (!count) return result;
    result.mean_ms = (double)(sum / count) / 1000000.0;
    result.p50_ms = (double)sorted[(count - 1u) * 50u / 100u] / 1000000.0;
    result.p95_ms = (double)sorted[(count - 1u) * 95u / 100u] / 1000000.0;
    result.max_ms = (double)sorted[count - 1u] / 1000000.0;
    return result;
}

static void trace_frame_pacing(Uint64 enqueue_ns, Uint64 execute_start_ns,
                               Uint64 present_start_ns, Uint64 present_end_ns)
{
    if (!s_sdl.pace_trace) return;
    if (!s_sdl.pace_window_start_ns)
        s_sdl.pace_window_start_ns = present_end_ns;

    if (s_sdl.pace_last_enqueue_ns && s_sdl.pace_last_present_ns &&
        s_sdl.pace_sample_count < SDL_RSX_PACE_SAMPLES) {
        pace_sample* sample = &s_sdl.pace_samples[s_sdl.pace_sample_count++];
        sample->submit_interval_ns = enqueue_ns - s_sdl.pace_last_enqueue_ns;
        sample->present_interval_ns = present_end_ns - s_sdl.pace_last_present_ns;
        sample->queue_wait_ns = execute_start_ns >= enqueue_ns
            ? execute_start_ns - enqueue_ns : 0;
        sample->execute_ns = present_end_ns - execute_start_ns;
        sample->present_cpu_ns = present_end_ns - present_start_ns;
    }
    s_sdl.pace_last_enqueue_ns = enqueue_ns;
    s_sdl.pace_last_present_ns = present_end_ns;

    if (present_end_ns - s_sdl.pace_window_start_ns < SDL_NS_PER_SECOND)
        return;

    const unsigned count = s_sdl.pace_sample_count;
    pace_stats submit = summarize_pace(s_sdl.pace_samples, count,
        offsetof(pace_sample, submit_interval_ns));
    pace_stats present = summarize_pace(s_sdl.pace_samples, count,
        offsetof(pace_sample, present_interval_ns));
    pace_stats queue = summarize_pace(s_sdl.pace_samples, count,
        offsetof(pace_sample, queue_wait_ns));
    pace_stats execute = summarize_pace(s_sdl.pace_samples, count,
        offsetof(pace_sample, execute_ns));
    pace_stats present_cpu = summarize_pace(s_sdl.pace_samples, count,
        offsetof(pace_sample, present_cpu_ns));
    unsigned early = 0, late = 0;
    for (unsigned i = 0; i < count; ++i) {
        early += s_sdl.pace_samples[i].present_interval_ns < 12000000u;
        late += s_sdl.pace_samples[i].present_interval_ns > 20000000u;
    }
    fprintf(stderr,
            "[PRESENTPACE] n=%u "
            "submit(mean/p50/p95/max)=%.2f/%.2f/%.2f/%.2fms "
            "present=%.2f/%.2f/%.2f/%.2fms early<12=%u late>20=%u "
            "queue=%.2f/%.2f/%.2f/%.2fms "
            "cpu=%.2f/%.2f/%.2f/%.2fms "
            "present_cpu=%.2f/%.2f/%.2f/%.2fms\n",
            count,
            submit.mean_ms, submit.p50_ms, submit.p95_ms, submit.max_ms,
            present.mean_ms, present.p50_ms, present.p95_ms, present.max_ms,
            early, late,
            queue.mean_ms, queue.p50_ms, queue.p95_ms, queue.max_ms,
            execute.mean_ms, execute.p50_ms, execute.p95_ms, execute.max_ms,
            present_cpu.mean_ms, present_cpu.p50_ms, present_cpu.p95_ms,
            present_cpu.max_ms);
    s_sdl.pace_window_start_ns = present_end_ns;
    s_sdl.pace_sample_count = 0;
}

static SDL_GPUTextureFormat portable_format(rsx_portable_format format)
{
    switch (format) {
    case RSX_FORMAT_RGBA8: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    case RSX_FORMAT_RGBA16F: return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    case RSX_FORMAT_RGBA32F: return SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    case RSX_FORMAT_R32F: return SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    case RSX_FORMAT_D24S8: return SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    case RSX_FORMAT_D32F: return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    default: return SDL_GPU_TEXTUREFORMAT_INVALID;
    }
}

static int same_surface(const rsx_surface_ref* a, const rsx_surface_ref* b)
{
    return a->resolved_offset == b->resolved_offset &&
           a->width == b->width && a->height == b->height &&
           a->format == b->format && a->is_depth == b->is_depth &&
           a->is_display == b->is_display;
}

static SDL_GPUTexture* create_surface_texture(const rsx_surface_ref* ref)
{
    SDL_GPUTextureFormat format = portable_format(ref->format);
    SDL_GPUTextureUsageFlags usage = ref->is_depth
        ? SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET
        : SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (format == SDL_GPU_TEXTUREFORMAT_INVALID ||
        !SDL_GPUTextureSupportsFormat(s_sdl.device, format,
                                      SDL_GPU_TEXTURETYPE_2D, usage)) {
        fprintf(stderr, "[SDL_GPU] unsupported surface format %u usage=0x%x\n",
                (unsigned)ref->format, (unsigned)usage);
        ++s_sdl.errors;
        return NULL;
    }
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = ref->width ? ref->width : SDL_RSX_WIDTH;
    info.height = ref->height ? ref->height : SDL_RSX_HEIGHT;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* result = SDL_CreateGPUTexture(s_sdl.device, &info);
    if (!result) {
        fprintf(stderr, "[SDL_GPU] surface creation failed: %s\n", SDL_GetError());
        ++s_sdl.errors;
    }
    return result;
}

static SDL_GPUTexture* get_surface(const rsx_surface_ref* ref)
{
    if (!ref || !ref->width || !ref->height ||
        ref->format == RSX_FORMAT_INVALID)
        return NULL;
    /* Match the D3D12 oracle's frame ownership: guest display buffers are
     * aliases for one fixed host output. The game can draw through display
     * offset A and request a flip for display ID B while a display-clear-owned
     * batch is still accumulating. Treating those offsets as independent GPU
     * textures presented stale seed data (the characteristic upper-right
     * quarter-screen), even though the current batch rendered correctly. */
    if (ref->is_display && !ref->is_depth && s_sdl.display)
        return s_sdl.display;
    for (unsigned i = 0; i < s_sdl.surface_count; ++i)
        if (same_surface(&s_sdl.surfaces[i].ref, ref))
            return s_sdl.surfaces[i].texture;
    if (s_sdl.surface_count == SDL_RSX_MAX_SURFACES) {
        fprintf(stderr, "[SDL_GPU] persistent surface table exhausted\n");
        ++s_sdl.errors;
        return NULL;
    }
    gpu_surface* entry = &s_sdl.surfaces[s_sdl.surface_count++];
    entry->ref = *ref;
    entry->texture = create_surface_texture(ref);
    if (entry->texture && ref->is_display)
        s_sdl.display = entry->texture;
    return entry->texture;
}

static int submit_commands(SDL_GPUCommandBuffer* commands)
{
    __atomic_fetch_add(&s_sdl.perf_submits, 1u, __ATOMIC_RELAXED);
    if (!commands) return -1;
    if (!SDL_SubmitGPUCommandBuffer(commands)) {
        fprintf(stderr, "[SDL_GPU] command submission failed: %s\n", SDL_GetError());
        ++s_sdl.errors;
        return -1;
    }
    return 0;
}

static int submit_commands_and_wait(SDL_GPUCommandBuffer* commands)
{
    if (!commands) return -1;
    __atomic_fetch_add(&s_sdl.perf_submits, 1u, __ATOMIC_RELAXED);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!fence) {
        fprintf(stderr, "[SDL_GPU] fenced command submission failed: %s\n",
                SDL_GetError());
        ++s_sdl.errors;
        return -1;
    }
    SDL_GPUFence* fences[] = { fence };
    int result = 0;
    if (!SDL_WaitForGPUFences(s_sdl.device, true, fences, 1)) {
        fprintf(stderr, "[SDL_GPU] render/present fence wait failed: %s\n",
                SDL_GetError());
        ++s_sdl.errors;
        result = -1;
    }
    SDL_ReleaseGPUFence(s_sdl.device, fence);
    return result;
}

static int submit_kms_render_and_wait(SDL_GPUCommandBuffer* commands)
{
    if (submit_commands_and_wait(commands) != 0) return -1;
    if (!s_sdl.kms_gpu_idle) return 0;

    static int announced;
    if (!announced++)
        fprintf(stderr,
                "[SDL_GPU] KMS render/download boundary uses full GPU idle\n");
    if (!SDL_WaitForGPUIdle(s_sdl.device)) {
        fprintf(stderr, "[SDL_GPU] KMS GPU idle wait failed: %s\n",
                SDL_GetError());
        ++s_sdl.errors;
        return -1;
    }
    return 0;
}

static int wait_and_release_fence(SDL_GPUFence** fence_ptr)
{
    if (!fence_ptr || !*fence_ptr) return 0;
    SDL_GPUFence* fences[] = { *fence_ptr };
    int result = 0;
    if (!SDL_WaitForGPUFences(s_sdl.device, true, fences, 1)) {
        fprintf(stderr, "[SDL_GPU] pipelined presentation fence failed: %s\n",
                SDL_GetError());
        ++s_sdl.errors;
        result = -1;
    }
    SDL_ReleaseGPUFence(s_sdl.device, *fence_ptr);
    *fence_ptr = NULL;
    return result;
}

static SDL_GPUTexture* create_presentation_slot(void)
{
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    if (!s_sdl.kms_zero_copy)
        info.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = SDL_RSX_WIDTH;
    info.height = SDL_RSX_HEIGHT;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_PropertiesID props = 0;
    if (s_sdl.kms_zero_copy) {
        props = SDL_CreateProperties();
        if (!props) return NULL;
        SDL_SetBooleanProperty(
            props, "SDL.gpu.texture.create.vulkan.dmabuf", true);
        if (s_sdl.kms_modifier_count > 0 &&
            getenv("TAIKO_KMS_ZERO_COPY_LINEAR") == NULL) {
            SDL_SetPointerProperty(
                props, "SDL.gpu.texture.create.vulkan.dmabuf.modifiers",
                s_sdl.kms_modifiers);
            SDL_SetNumberProperty(
                props, "SDL.gpu.texture.create.vulkan.dmabuf.modifier_count",
                s_sdl.kms_modifier_count);
        }
        info.props = props;
    }
    SDL_GPUTexture* result = SDL_CreateGPUTexture(s_sdl.device, &info);
    if (props) SDL_DestroyProperties(props);
    return result;
}

/* Rotate complete display render targets. Frame N renders into one target
 * while the previous completed target is presented, avoiding both the unsafe
 * render/sample overlap and a driver-problematic texture-copy operation. */
static int submit_pipelined_display(SDL_GPUCommandBuffer* commands)
{
    if (s_sdl.presentation_pipeline_disabled) return -1;
    if (!s_sdl.presentation_slots[0]) {
        s_sdl.presentation_slots[0] = s_sdl.display;
        for (unsigned i = 1; i < SDL_RSX_PRESENT_SLOTS; ++i) {
            s_sdl.presentation_slots[i] = create_presentation_slot();
            if (!s_sdl.presentation_slots[i]) {
                fprintf(stderr,
                        "[SDL_GPU] presentation target creation failed: %s\n",
                        SDL_GetError());
                ++s_sdl.errors;
                s_sdl.presentation_pipeline_disabled = 1;
                return -1;
            }
        }
        s_sdl.presentation_write = 0;
    }
    const unsigned slot = s_sdl.presentation_write;
    if (wait_and_release_fence(&s_sdl.presentation_fences[slot]) != 0) {
        s_sdl.presentation_pipeline_disabled = 1;
        return -1;
    }

    __atomic_fetch_add(&s_sdl.perf_submits, 1u, __ATOMIC_RELAXED);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!fence) {
        fprintf(stderr, "[SDL_GPU] pipelined display submission failed: %s\n",
                SDL_GetError());
        ++s_sdl.errors;
        s_sdl.presentation_pipeline_disabled = 1;
        return -1;
    }
    s_sdl.presentation_fences[slot] = fence;

    unsigned completed;
    if (!s_sdl.presentation_valid) {
        completed = slot;
    } else if (s_sdl.presentation_submitted < 2u) {
        completed = s_sdl.presentation_last;
    } else {
        /* (slot + 2) is the frame submitted two turns earlier: its GPU work
         * has had two full CPU preparation windows. The write cursor advances
         * by one, so this slot is not reused as a render target until two more
         * frames have passed and its presentation blit is long finished. */
        completed = (slot + 2u) % SDL_RSX_PRESENT_SLOTS;
    }
    if (wait_and_release_fence(&s_sdl.presentation_fences[completed]) != 0) {
        /* The render command buffer was consumed already. Keep presenting the
         * previous complete target, then use the fully serialized fallback on
         * subsequent frames. */
        s_sdl.presentation_pipeline_disabled = 1;
        return 1;
    }
    s_sdl.presentation_override = s_sdl.presentation_slots[completed];
    s_sdl.presentation_last = completed;
    s_sdl.presentation_write = (slot + 1u) % SDL_RSX_PRESENT_SLOTS;
    s_sdl.display = s_sdl.presentation_slots[s_sdl.presentation_write];
    ++s_sdl.presentation_submitted;
    s_sdl.presentation_valid = 1;
    return 0;
}

static void execute_clear(SDL_GPUCommandBuffer* commands,
                          const rsx_render_op* op)
{
    SDL_GPUTexture* colors[4] = {NULL, NULL, NULL, NULL};
    SDL_GPUColorTargetInfo color_info[4];
    SDL_zero(color_info);
    unsigned color_count = 0;
    for (unsigned i = 0; i < op->color_count && i < 4; ++i) {
        colors[color_count] = get_surface(&op->color[i]);
        if (!colors[color_count]) continue;
        color_info[color_count].texture = colors[color_count];
        color_info[color_count].clear_color.r = op->data.clear.color[0];
        color_info[color_count].clear_color.g = op->data.clear.color[1];
        color_info[color_count].clear_color.b = op->data.clear.color[2];
        color_info[color_count].clear_color.a = op->data.clear.color[3];
        color_info[color_count].load_op = (op->data.clear.flags & 0xf0u)
            ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        color_info[color_count].store_op = SDL_GPU_STOREOP_STORE;
        ++color_count;
    }
    SDL_GPUDepthStencilTargetInfo depth_info;
    SDL_zero(depth_info);
    SDL_GPUDepthStencilTargetInfo* depth_ptr = NULL;
    SDL_GPUTexture* depth = get_surface(&op->depth);
    if (depth) {
        depth_info.texture = depth;
        depth_info.clear_depth = op->data.clear.depth;
        depth_info.clear_stencil = op->data.clear.stencil;
        depth_info.load_op = (op->data.clear.flags & 1u)
            ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        depth_info.store_op = SDL_GPU_STOREOP_STORE;
        depth_info.stencil_load_op = (op->data.clear.flags & 2u)
            ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        depth_info.stencil_store_op = SDL_GPU_STOREOP_STORE;
        depth_ptr = &depth_info;
    }
    if (!color_count && !depth_ptr) return;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(
        commands, color_info, color_count, depth_ptr);
    if (!pass) {
        fprintf(stderr, "[SDL_GPU] clear pass failed: %s\n", SDL_GetError());
        ++s_sdl.errors;
    } else {
        SDL_EndGPURenderPass(pass);
    }
}

static void replace_all(char* text, size_t capacity,
                        const char* from, const char* to)
{
    const size_t from_size = strlen(from), to_size = strlen(to);
    if (!from_size) return;
    for (char* at = strstr(text, from); at; at = strstr(at + to_size, from)) {
        size_t tail = strlen(at + from_size);
        size_t used = (size_t)(at - text) + tail + to_size + 1;
        if (used > capacity) return;
        memmove(at + to_size, at + from_size, tail + 1);
        memcpy(at, to, to_size);
    }
}

static bool shader_cache_path(char* path, size_t path_size,
                              const char* extension,
                              SDL_ShaderCross_ShaderStage stage,
                              u64 source_hash)
{
    const char* directory = getenv("TAIKO_SHADER_CACHE");
    if (!directory || !directory[0]) return false;
    const int length = SDL_snprintf(
        path, path_size, "%s/v%u-%c-%016llx.%s", directory,
        SDL_RSX_SHADER_DIALECT_VERSION,
        stage == SDL_SHADERCROSS_SHADERSTAGE_VERTEX ? 'v' : 'f',
        (unsigned long long)source_hash, extension);
    return length > 0 && (size_t)length < path_size;
}

static bool shader_cache_valid_spirv(const void* data, size_t size)
{
    u32 magic = 0;
    if (!data || size < 20 || size > 4u * 1024u * 1024u || (size & 3u))
        return false;
    memcpy(&magic, data, sizeof(magic));
    return magic == UINT32_C(0x07230203);
}

static void shader_cache_save_source(const char* source,
                                     SDL_ShaderCross_ShaderStage stage,
                                     u64 source_hash)
{
    char path[1024];
    const char* directory = getenv("TAIKO_SHADER_CACHE");
    if (!directory || !directory[0] ||
        !shader_cache_path(path, sizeof(path), "hlsl", stage, source_hash))
        return;
    SDL_CreateDirectory(directory);
    SDL_SaveFile(path, source, strlen(source));
}

static SDL_GPUShader* compile_hlsl(const char* source,
                                   SDL_ShaderCross_ShaderStage stage,
                                   SDL_ShaderCross_GraphicsShaderResourceInfo* resources,
                                   u64 hash)
{
    if (getenv("SDL_GPU_DUMP_SHADERS"))
        fprintf(stderr, "[SDL_GPU] HLSL %016llx stage=%u\n%s\n",
                (unsigned long long)hash, (unsigned)stage, source);
    SDL_ShaderCross_HLSL_Info hlsl;
    SDL_zero(hlsl);
    hlsl.source = source;
    hlsl.entrypoint = "main";
    hlsl.shader_stage = stage;
    size_t spirv_size = 0;
    u64 source_hash = rsx_blob_hash64(source, (u64)strlen(source));
    source_hash ^= (u64)stage;
    source_hash *= UINT64_C(1099511628211);
    char cache_path[1024];
    Uint8* spirv = NULL;
    if (shader_cache_path(cache_path, sizeof(cache_path), "spv", stage,
                          source_hash)) {
        spirv = (Uint8*)SDL_LoadFile(cache_path, &spirv_size);
        if (spirv && !shader_cache_valid_spirv(spirv, spirv_size)) {
            fprintf(stderr, "[SDL_GPU] ignoring invalid shader cache %s\n",
                    cache_path);
            SDL_free(spirv);
            spirv = NULL;
            spirv_size = 0;
        }
        if (spirv) {
            fprintf(stderr, "[SDL_GPU] shader cache hit %016llx stage=%u bytes=%zu\n",
                    (unsigned long long)hash, (unsigned)stage, spirv_size);
        }
    }
    if (!spirv) {
        shader_cache_save_source(source, stage, source_hash);
        const char* readonly = getenv("TAIKO_SHADER_CACHE_READONLY");
        if (readonly && readonly[0] && strcmp(readonly, "0") != 0) {
            fprintf(stderr,
                    "[SDL_GPU] shader cache miss %016llx stage=%u source=%016llx (cache only)\n",
                    (unsigned long long)hash, (unsigned)stage,
                    (unsigned long long)source_hash);
            ++s_sdl.errors;
            return NULL;
        }
        const Uint64 shader_compile_start_ns = SDL_GetTicksNS();
        fprintf(stderr, "[SDL_GPU] shader compile begin %016llx stage=%u\n",
                (unsigned long long)hash, (unsigned)stage);
        spirv = SDL_ShaderCross_CompileSPIRVFromHLSL(&hlsl, &spirv_size);
        fprintf(stderr,
                "[SDL_GPU] shader compile end %016llx stage=%u %.2f ms bytes=%zu\n",
                (unsigned long long)hash, (unsigned)stage,
                (double)(SDL_GetTicksNS() - shader_compile_start_ns) / 1000000.0,
                spirv_size);
        if (spirv && shader_cache_path(cache_path, sizeof(cache_path), "spv",
                                       stage, source_hash)) {
            const char* directory = getenv("TAIKO_SHADER_CACHE");
            SDL_CreateDirectory(directory);
            if (!SDL_SaveFile(cache_path, spirv, spirv_size))
                fprintf(stderr, "[SDL_GPU] shader cache save failed %s: %s\n",
                        cache_path, SDL_GetError());
        }
    }
    if (!spirv) {
        fprintf(stderr, "[SDL_GPU] HLSL compile failed (%016llx): %s\n%s\n",
                (unsigned long long)hash, SDL_GetError(), source);
        ++s_sdl.errors;
        return NULL;
    }
    SDL_ShaderCross_GraphicsShaderMetadata* metadata =
        SDL_ShaderCross_ReflectGraphicsSPIRV(spirv, spirv_size, 0);
    SDL_GPUShader* result = NULL;
    if (metadata) {
        SDL_ShaderCross_SPIRV_Info info;
        SDL_zero(info);
        info.bytecode = spirv;
        info.bytecode_size = spirv_size;
        info.entrypoint = "main";
        info.shader_stage = stage;
        *resources = metadata->resource_info;
        if (getenv("SDL_GPU_DUMP_SHADERS"))
            fprintf(stderr, "[SDL_GPU] resources samplers=%u storage_tex=%u storage_buf=%u uniforms=%u\n",
                    resources->num_samplers, resources->num_storage_textures,
                    resources->num_storage_buffers, resources->num_uniform_buffers);
        result = SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(
            s_sdl.device, &info, resources, 0);
        SDL_free(metadata);
    }
    SDL_free(spirv);
    if (!result) {
        fprintf(stderr, "[SDL_GPU] device shader creation failed (%016llx): %s\n%s\n",
                (unsigned long long)hash, SDL_GetError(), source);
        ++s_sdl.errors;
    }
    return result;
}

static u32 vertex_input_mask(const rsx_draw_op_data* draw)
{
    /* Layouts older than PACKED carry every slot; a PACKED draw carries
     * exactly what its vertex program reads, which is derived from the same
     * ucode the producer used. */
    if (draw->pipeline.vertex_layout != RSX_VERTEX_LAYOUT_PACKED ||
        !draw->vertex_shader.size)
        return 0xFFFFu;
    return rsx_vp_input_mask(draw->vertex_shader.data,
                             (u32)draw->vertex_shader.size);
}

static unsigned vertex_slot_count(u32 mask)
{
    unsigned count = 0;
    for (unsigned i = 0; i < 16; ++i) if (mask & (1u << i)) ++count;
    return count;
}

/* Taiko builds each player character in a centered 600x600 surface, then runs
 * a nine-sample outline filter over a full-target quad. Most of that quad is
 * guaranteed transparent background. Keep this opt-in and narrowly keyed to
 * the observed filter/target shape: costumes can be larger than the default
 * character, so the service chooses a conservative margin and live validation
 * remains the final oracle. */
static int character_scissor_margin(void)
{
    static int initialized;
    static int margin = -1;
    if (!initialized) {
        const char* value = getenv("TAIKO_GPU_CHARACTER_FILTER_SCISSOR");
        if (value && value[0]) {
            char* end = NULL;
            long parsed = strtol(value, &end, 0);
            if (end && !*end && parsed >= 0 && parsed <= 256)
                margin = (int)parsed;
        }
        initialized = 1;
    }
    return margin;
}

static int is_character_outline_filter(const rsx_render_op* op)
{
    return op && op->type == RSX_RENDER_OP_DRAW &&
        op->color_count == 1 && !op->color[0].is_display &&
        op->color[0].width == 600 && op->color[0].height == 600 &&
        op->data.draw.vertex_count == 6 &&
        op->data.draw.pipeline.fragment_shader_hash ==
            SDL_RSX_CHARACTER_OUTLINE_FP &&
        op->data.draw.texture_count &&
        op->data.draw.textures[0].width == 600 &&
        op->data.draw.textures[0].height == 600;
}

static int character_outline_enabled(void)
{
    static int initialized;
    static int enabled = 1;
    if (!initialized) {
        const char* value = getenv("TAIKO_GPU_CHARACTER_OUTLINE");
        enabled = !(value && strcmp(value, "0") == 0);
        initialized = 1;
    }
    return enabled;
}

static int is_character_mesh_outline(const rsx_render_op* op)
{
    /* Lumen's model renderer expands the same character meshes behind the
     * normal pass with reversed culling and two outline-only programs. This
     * is the inner red/black shell visible beneath the later 600x600 outline
     * filter. Keep the optional removal tied to both program identities and
     * the title-specific character-target shape. */
    if (!op || op->type != RSX_RENDER_OP_DRAW || op->color_count != 1 ||
        op->color[0].is_display || op->color[0].width != 600 ||
        op->color[0].height != 600)
        return 0;
    const rsx_pipeline_key* pipeline = &op->data.draw.pipeline;
    const u64 fragment_hash = pipeline->fragment_shader_hash;
    return !pipeline->blend_enable && pipeline->cull_enable &&
        (pipeline->cull_face & 0xffffu) == 0x0404u &&
        (fragment_hash == SDL_RSX_CHARACTER_MESH_OUTLINE_SKINNED_FP ||
         fragment_hash == SDL_RSX_CHARACTER_MESH_OUTLINE_RIGID_FP);
}

static int skip_character_mesh_outline(const rsx_render_op* op)
{
    static int announced;
    if (character_outline_enabled() || !is_character_mesh_outline(op))
        return 0;
    if (!announced) {
        announced = 1;
        fprintf(stderr,
                "[SDL_GPU] character expanded-mesh outline disabled\n");
    }
    return 1;
}

static int bypass_character_outline(const rsx_render_op* op)
{
    static int announced;
    if (character_outline_enabled() || !is_character_outline_filter(op))
        return 0;
    if (!announced) {
        announced = 1;
        fprintf(stderr,
                "[SDL_GPU] character outer-outline filter bypassed\n");
    }
    return 1;
}

static void intersect_scissor(SDL_Rect* scissor, int x, int y, int w, int h)
{
    const int scissor_end_x = scissor->x + scissor->w;
    const int scissor_end_y = scissor->y + scissor->h;
    const int end_x = x + w;
    const int end_y = y + h;
    const int clipped_x = scissor->x > x ? scissor->x : x;
    const int clipped_y = scissor->y > y ? scissor->y : y;
    const int clipped_end_x = scissor_end_x < end_x
        ? scissor_end_x : end_x;
    const int clipped_end_y = scissor_end_y < end_y
        ? scissor_end_y : end_y;
    scissor->x = clipped_x;
    scissor->y = clipped_y;
    scissor->w = clipped_end_x > clipped_x
        ? clipped_end_x - clipped_x : 0;
    scissor->h = clipped_end_y > clipped_y
        ? clipped_end_y - clipped_y : 0;
}

/* The filtered 600x600 surface is then drawn to the display by one known
 * affine vertex program. Transform the same central source rectangle into
 * display coordinates so its expensive five-sample composite does not shade
 * the transparent border a second time. */
static int character_composite_scissor(const rsx_render_op* op, int margin,
                                       SDL_Rect* result)
{
    const rsx_draw_op_data* draw = op ? &op->data.draw : NULL;
    if (!op || op->type != RSX_RENDER_OP_DRAW || op->color_count != 1 ||
        !op->color[0].is_display || op->color[0].width != SDL_RSX_WIDTH ||
        op->color[0].height != SDL_RSX_HEIGHT || !draw ||
        !op->viewport[2] || !op->viewport[3] ||
        draw->vertex_count != 6 ||
        draw->pipeline.vertex_layout != RSX_VERTEX_LAYOUT_PACKED ||
        draw->pipeline.vertex_shader_hash != SDL_RSX_CHARACTER_COMPOSITE_VP ||
        draw->pipeline.fragment_shader_hash != SDL_RSX_CHARACTER_COMPOSITE_FP ||
        !draw->texture_count || draw->textures[0].width != 600 ||
        draw->textures[0].height != 600 || draw->vertex_data.size != 96 ||
        draw->vertex_constants.size <
            (u64)RSX_BATCH_VP_CONSTANTS * 4u * sizeof(float))
        return 0;

    const float* vertices = (const float*)draw->vertex_data.data;
    for (u32 i = 0; i < 6; ++i) {
        const float x = vertices[i * 4u];
        const float y = vertices[i * 4u + 1u];
        if ((x != -0.5f && x != 0.5f) ||
            (y != -0.5f && y != 0.5f) ||
            vertices[i * 4u + 2u] != 0.0f ||
            vertices[i * 4u + 3u] != 1.0f)
            return 0;
    }

    const float* constants = (const float*)draw->vertex_constants.data;
    const float* c256 = constants + 256u * 4u;
    const float* c257 = constants + 257u * 4u;
    const float* c259 = constants + 259u * 4u;
    const float* c467 = constants + 467u * 4u;
    const float* c512 = constants + 512u * 4u;
    const float* c513 = constants + 513u * 4u;
    const float uv_min = (float)margin / 600.0f;
    const float uv_max = 1.0f - uv_min;
    const float vx[2] = {uv_min - 0.5f, uv_max - 0.5f};
    const float vy[2] = {0.5f - uv_min, 0.5f - uv_max};
    float min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;
    for (u32 iy = 0; iy < 2; ++iy) for (u32 ix = 0; ix < 2; ++ix) {
        float p[4];
        for (u32 component = 0; component < 4; ++component)
            p[component] = vx[ix] * c256[component] +
                vy[iy] * c257[component] + c259[component] + c467[0];
        if (p[3] > -0.000001f && p[3] < 0.000001f) return 0;
        const float ndc_x = (p[0] * c512[0] + p[3] * c513[0]) / p[3];
        const float ndc_y = (p[1] * c512[1] + p[3] * c513[1]) / p[3];
        const float screen_x = (float)op->viewport[0] +
            (ndc_x + 1.0f) * 0.5f * (float)op->viewport[2];
        const float screen_y = (float)op->viewport[1] +
            (1.0f - ndc_y) * 0.5f * (float)op->viewport[3];
        if (screen_x != screen_x || screen_y != screen_y ||
            screen_x < -32768.0f || screen_x > 32768.0f ||
            screen_y < -32768.0f || screen_y > 32768.0f)
            return 0;
        if (!ix && !iy) min_x = max_x = screen_x, min_y = max_y = screen_y;
        else {
            if (screen_x < min_x) min_x = screen_x;
            if (screen_x > max_x) max_x = screen_x;
            if (screen_y < min_y) min_y = screen_y;
            if (screen_y > max_y) max_y = screen_y;
        }
    }
    int x = (int)min_x;
    int y = (int)min_y;
    int end_x = (int)max_x;
    int end_y = (int)max_y;
    if ((float)x > min_x) --x;
    if ((float)y > min_y) --y;
    if ((float)end_x < max_x) ++end_x;
    if ((float)end_y < max_y) ++end_y;
    /* One conservative raster pixel covers float rounding and edge sampling. */
    --x; --y; ++end_x; ++end_y;
    result->x = x;
    result->y = y;
    result->w = end_x - x;
    result->h = end_y - y;
    return result->w > 0 && result->h > 0;
}

static shader_entry* get_shader(const rsx_render_op* op,
                                SDL_ShaderCross_ShaderStage stage)
{
    const rsx_draw_op_data* draw = &op->data.draw;
    const rsx_owned_blob* blob = stage == SDL_SHADERCROSS_SHADERSTAGE_VERTEX
        ? &draw->vertex_shader : &draw->fragment_shader;
    const int outline_bypass =
        stage == SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT &&
        bypass_character_outline(op);
    u64 hash = outline_bypass ? SDL_RSX_DIRECT_COPY_FP :
        stage == SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT && blob->size
        ? rsx_fp_program_structure_hash(blob->data, (u32)blob->size)
        : blob->hash;
    unsigned flags = stage == SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT
        ? draw->pipeline.fragment_32bit_exports |
              (draw->pipeline.color_target_count << 1)
        : draw->pipeline.vertex_layout;
    flags |= SDL_RSX_SHADER_DIALECT_VERSION << 16;
    for (unsigned i = 0; i < s_sdl.shader_count; ++i)
        if (s_sdl.shaders[i].hash == hash && s_sdl.shaders[i].stage == (unsigned)stage &&
            s_sdl.shaders[i].flags == flags)
            return &s_sdl.shaders[i];
    if (s_sdl.shader_count == SDL_RSX_MAX_SHADERS) {
        fprintf(stderr, "[SDL_GPU] shader cache exhausted\n");
        ++s_sdl.errors;
        return NULL;
    }
    char* source = (char*)malloc(512u * 1024u);
    if (!source) return NULL;
    int result;
    u8 sampler_units[4] = {0, 0, 0, 0};
    unsigned sampler_unit_count = 0;
    u16 vertex_constant_units[RSX_BATCH_VP_CONSTANTS];
    unsigned vertex_constant_count = 0;
    if (stage == SDL_SHADERCROSS_SHADERSTAGE_VERTEX && blob->size) {
        result = rsx_vp_decompile(blob->data, (u32)blob->size,
                                  source, 512u * 1024u,
                                  vertex_input_mask(draw));
        for (unsigned i = 0; i < 16; ++i) {
            char from[16], to[20];
            snprintf(from, sizeof(from), "ATTR%u", i);
            snprintf(to, sizeof(to), "TEXCOORD%u", i);
            replace_all(source, 512u * 1024u, from, to);
        }
        /* The RSX bank exceeds portable push-uniform limits. Each batch packs
         * the immutable 514-float4 snapshots into one read-only storage
         * buffer; a small uniform selects this draw's base element. */
        replace_all(source, 512u * 1024u,
                    "cbuffer VPConst : register(b0) {\n"
                    "    float4 vp_c[512];\n"
                    "    float4 vp_posscale;\n"
                    "    float4 vp_posoffset;\n"
                    "};",
                    "StructuredBuffer<float4> rsx_vp_constants : register(t0, space0);\n"
                    "cbuffer VPBase : register(b0, space1) { uint rsx_vp_base; uint3 rsx_vp_pad; };");
        u8 used_constants[512];
        const int indexed_constants = rsx_vp_constant_usage(
            blob->data, (u32)blob->size, used_constants);
        if (indexed_constants != 0) {
            /* Invalid scans are conservatively equivalent to indexed reads. */
            for (unsigned i = 0; i < RSX_BATCH_VP_CONSTANTS; ++i)
                vertex_constant_units[vertex_constant_count++] = (u16)i;
            replace_all(source, 512u * 1024u, "vp_c[",
                        "rsx_vp_constants[rsx_vp_base+");
            replace_all(source, 512u * 1024u, "vp_posscale",
                        "rsx_vp_constants[rsx_vp_base+512]");
            replace_all(source, 512u * 1024u, "vp_posoffset",
                        "rsx_vp_constants[rsx_vp_base+513]");
        } else {
            /* Static shaders usually touch fewer than ten of the 512 guest
             * constants. Rewrite those references to dense slots and append
             * the two viewport epilogue vectors. */
            for (unsigned guest = 0; guest < 512; ++guest) {
                if (!used_constants[guest]) continue;
                char from[24], to[64];
                snprintf(from, sizeof(from), "vp_c[%u]", guest);
                snprintf(to, sizeof(to),
                         "rsx_vp_constants[rsx_vp_base+%u]",
                         vertex_constant_count);
                replace_all(source, 512u * 1024u, from, to);
                vertex_constant_units[vertex_constant_count++] = (u16)guest;
            }
            const unsigned scale_slot = vertex_constant_count;
            vertex_constant_units[vertex_constant_count++] = 512;
            const unsigned offset_slot = vertex_constant_count;
            vertex_constant_units[vertex_constant_count++] = 513;
            char scale[64], offset[64];
            snprintf(scale, sizeof(scale),
                     "rsx_vp_constants[rsx_vp_base+%u]", scale_slot);
            snprintf(offset, sizeof(offset),
                     "rsx_vp_constants[rsx_vp_base+%u]", offset_slot);
            replace_all(source, 512u * 1024u, "vp_posscale", scale);
            replace_all(source, 512u * 1024u, "vp_posoffset", offset);
        }
        /* SDL_GPU reserves set/space 1 for vertex-stage uniforms. This also
         * handles an unexpected older decompiler declaration defensively. */
        replace_all(source, 512u * 1024u,
                    "cbuffer VPConst : register(b0)",
                    "cbuffer VPConst : register(b0, space1)");
    } else if (stage == SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT && blob->size) {
        const u8* fragment_data = outline_bypass
            ? s_direct_copy_fp : blob->data;
        const u32 fragment_size = outline_bypass
            ? (u32)sizeof(s_direct_copy_fp) : (u32)blob->size;
        result = rsx_fp_decompile_dynamic(
            fragment_data, fragment_size, source, 512u * 1024u,
            draw->pipeline.fragment_32bit_exports,
            draw->pipeline.color_target_count ? draw->pipeline.color_target_count
                                              : 1u);
        if (result >= 0)
            replace_all(source, 512u * 1024u, "struct PSInput",
                        "#define RSX_SHADER_REMAP 1\nstruct PSInput");
        /* SDL_GPU requires sampled resources to occupy dense slots. DXC
         * removes unused declarations while preserving (for example) a lone
         * rsx_tex1 at binding 1; ShaderCross then reports one sampler, and
         * binding guest unit 0 to SDL slot 0 leaves unit 1 unbound. Don-chan's
         * face shader is the minimal reproducer. Detect the sampled RSX units,
         * remap their resource declarations to a dense array at t0, and retain
         * the inverse map in shader_entry for draw-time texture selection. */
        for (unsigned i = 0; i < 4; ++i) {
            char needle[24];
            snprintf(needle, sizeof(needle), "rsx_tex%u.Sample", i);
            if (strstr(source, needle))
                sampler_units[sampler_unit_count++] = (u8)i;
        }
        char texture_declaration[64] = {0};
        char sampler_declaration[64] = {0};
        if (sampler_unit_count) {
            snprintf(texture_declaration, sizeof(texture_declaration),
                     "Texture2D rsx_tex[%u] : register(t0);",
                     sampler_unit_count);
            snprintf(sampler_declaration, sizeof(sampler_declaration),
                     "SamplerState rsx_samp[%u] : register(s0);",
                     sampler_unit_count);
        }
        replace_all(source, 512u * 1024u,
                    "Texture2D    rsx_tex0 : register(t0); Texture2D rsx_tex1 : register(t1);\n"
                    "Texture2D    rsx_tex2 : register(t2); Texture2D rsx_tex3 : register(t3);",
                    texture_declaration);
        replace_all(source, 512u * 1024u,
                    "SamplerState rsx_samp0 : register(s0); SamplerState rsx_samp1 : register(s1);\n"
                    "SamplerState rsx_samp2 : register(s2); SamplerState rsx_samp3 : register(s3);",
                    sampler_declaration);
        for (unsigned dense = 0; dense < sampler_unit_count; ++dense) {
            unsigned unit = sampler_units[dense];
            char texture_from[16], texture_to[16];
            char sampler_from[16], sampler_to[16];
            snprintf(texture_from, sizeof(texture_from), "rsx_tex%u", unit);
            snprintf(texture_to, sizeof(texture_to), "rsx_tex[%u]", dense);
            snprintf(sampler_from, sizeof(sampler_from), "rsx_samp%u", unit);
            snprintf(sampler_to, sizeof(sampler_to), "rsx_samp[%u]", dense);
            replace_all(source, 512u * 1024u, texture_from, texture_to);
            replace_all(source, 512u * 1024u, sampler_from, sampler_to);
        }
        replace_all(source, 512u * 1024u,
                    "cbuffer FPTex : register(b1)",
                    "cbuffer FPTex : register(b0, space3)");
        replace_all(source, 512u * 1024u,
                    "cbuffer FPConstants : register(b2)",
                    "cbuffer FPConstants : register(b1, space3)");
        /* Fragment sampled textures/samplers occupy set/space 2. Uniform
         * buffers occupy set/space 3 and are numbered independently. */
        for (unsigned i = 0; i < 4; ++i) {
            char texture_from[16], texture_to[28];
            char sampler_from[16], sampler_to[28];
            snprintf(texture_from, sizeof(texture_from), "register(t%u)", i);
            snprintf(texture_to, sizeof(texture_to),
                     "register(t%u, space2)", i);
            snprintf(sampler_from, sizeof(sampler_from), "register(s%u)", i);
            snprintf(sampler_to, sizeof(sampler_to),
                     "register(s%u, space2)", i);
            replace_all(source, 512u * 1024u, texture_from, texture_to);
            replace_all(source, 512u * 1024u, sampler_from, sampler_to);
        }
    } else if (stage == SDL_SHADERCROSS_SHADERSTAGE_VERTEX) {
        result = snprintf(source, 512u * 1024u,
            "struct I{float3 p:TEXCOORD0;float4 c:TEXCOORD1;float2 t:TEXCOORD2;};"
            "struct O{float4 p:SV_Position;float4 c:COLOR0;float4 c1:COLOR1;float4 f:FOG;"
            "float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;"
            "float4 t4:TEXCOORD4;float4 t5:TEXCOORD5;float4 t6:TEXCOORD6;float4 t7:TEXCOORD7;};"
            "O main(I i){O o=(O)0;o.p=float4(i.p,1);o.c=i.c;o.t0=float4(i.t,0,1);return o;}");
    } else {
        result = snprintf(source, 512u * 1024u,
            "struct I{float4 p:SV_Position;float4 c:COLOR0;float4 c1:COLOR1;float4 f:FOG;"
            "float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;"
            "float4 t4:TEXCOORD4;float4 t5:TEXCOORD5;float4 t6:TEXCOORD6;float4 t7:TEXCOORD7;};"
            "float4 main(I i):SV_Target0{return i.c;}");
    }
    if (result < 0 || result >= 512 * 1024) {
        fprintf(stderr, "[SDL_GPU] RSX shader decompile failed (%016llx)\n",
                (unsigned long long)hash);
        free(source); ++s_sdl.errors; return NULL;
    }
    shader_entry* entry = &s_sdl.shaders[s_sdl.shader_count];
    SDL_zero(*entry);
    entry->hash = hash;
    entry->stage = (unsigned)stage;
    entry->flags = flags;
    entry->shader = compile_hlsl(source, stage, &entry->resources, hash);
    entry->sampler_unit_count = (u8)sampler_unit_count;
    memcpy(entry->sampler_units, sampler_units, sizeof(entry->sampler_units));
    entry->vertex_constant_count = (u16)vertex_constant_count;
    memcpy(entry->vertex_constant_units, vertex_constant_units,
           vertex_constant_count * sizeof(vertex_constant_units[0]));
    free(source);
    if (!entry->shader) return NULL;
    ++s_sdl.shader_count;
    return entry;
}

static SDL_GPUCompareOp compare_op(u32 value)
{
    static const SDL_GPUCompareOp ops[8] = {
        SDL_GPU_COMPAREOP_NEVER, SDL_GPU_COMPAREOP_LESS,
        SDL_GPU_COMPAREOP_EQUAL, SDL_GPU_COMPAREOP_LESS_OR_EQUAL,
        SDL_GPU_COMPAREOP_GREATER, SDL_GPU_COMPAREOP_NOT_EQUAL,
        SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, SDL_GPU_COMPAREOP_ALWAYS
    };
    return ops[value & 7u];
}

static SDL_GPUStencilOp stencil_op(u32 value)
{
    switch (value & 0xffffu) {
    case 0x0000: return SDL_GPU_STENCILOP_ZERO;
    case 0x1e01: return SDL_GPU_STENCILOP_REPLACE;
    case 0x1e02: return SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP;
    case 0x1e03: return SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP;
    case 0x150a: return SDL_GPU_STENCILOP_INVERT;
    case 0x8507: return SDL_GPU_STENCILOP_INCREMENT_AND_WRAP;
    case 0x8508: return SDL_GPU_STENCILOP_DECREMENT_AND_WRAP;
    default: return SDL_GPU_STENCILOP_KEEP;
    }
}

static SDL_GPUBlendFactor blend_factor(u32 value, int alpha)
{
    switch (value & 0xffffu) {
    case 0x0000: return SDL_GPU_BLENDFACTOR_ZERO;
    case 0x0001: return SDL_GPU_BLENDFACTOR_ONE;
    case 0x0300: return alpha ? SDL_GPU_BLENDFACTOR_SRC_ALPHA : SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case 0x0301: return alpha ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case 0x0303: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case 0x0305: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return alpha ? SDL_GPU_BLENDFACTOR_DST_ALPHA : SDL_GPU_BLENDFACTOR_DST_COLOR;
    case 0x0307: return alpha ? SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA : SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return alpha ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
    case 0x8003: return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
    case 0x8004: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
    default: return SDL_GPU_BLENDFACTOR_ONE;
    }
}

static SDL_GPUBlendOp blend_op(u32 value)
{
    switch (value & 0xffffu) {
    case 0x800a: return SDL_GPU_BLENDOP_SUBTRACT;
    case 0xf005: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    case 0x8007: return SDL_GPU_BLENDOP_MIN;
    case 0x8008: return SDL_GPU_BLENDOP_MAX;
    default: return SDL_GPU_BLENDOP_ADD;
    }
}

static SDL_GPUGraphicsPipeline* get_pipeline(const rsx_render_op* op)
{
    rsx_pipeline_key canonical = op->data.draw.pipeline;
    if (bypass_character_outline(op))
        canonical.fragment_shader_hash = SDL_RSX_DIRECT_COPY_FP;
    else if (op->data.draw.fragment_shader.size)
        canonical.fragment_shader_hash = rsx_fp_program_structure_hash(
            op->data.draw.fragment_shader.data,
            (u32)op->data.draw.fragment_shader.size);
    const rsx_pipeline_key* key = &canonical;
    for (unsigned i = 0; i < s_sdl.pipeline_count; ++i)
        if (memcmp(&s_sdl.pipelines[i].key, key, sizeof(*key)) == 0)
            return s_sdl.pipelines[i].pipeline;
    if (s_sdl.pipeline_count == SDL_RSX_MAX_PIPELINES) {
        ++s_sdl.errors; return NULL;
    }
    shader_entry* vertex = get_shader(op, SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
    shader_entry* fragment = get_shader(op, SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
    if (!vertex || !fragment) return NULL;
    const unsigned packed_slots =
        vertex_slot_count(vertex_input_mask(&op->data.draw));
    {   /* The producer packed the vertex from its own reading of this ucode;
         * if the two disagree every vertex is fetched at the wrong stride. */
        const rsx_draw_op_data* draw = &op->data.draw;
        u64 expected = (u64)draw->vertex_count * packed_slots * 16u;
        if (draw->pipeline.vertex_layout == RSX_VERTEX_LAYOUT_PACKED &&
            draw->vertex_count && draw->vertex_data.size != expected) {
            static unsigned reported;
            if (reported++ < 8)
                fprintf(stderr, "[SDL_GPU] packed vertex stride mismatch: "
                        "vp=%016llx slots=%u verts=%u bytes=%llu expected=%llu\n",
                        (unsigned long long)draw->pipeline.vertex_shader_hash,
                        packed_slots, draw->vertex_count,
                        (unsigned long long)draw->vertex_data.size,
                        (unsigned long long)expected);
        }
    }
    SDL_GPUVertexBufferDescription buffer_description;
    SDL_zero(buffer_description);
    buffer_description.slot = 0;
    buffer_description.pitch =
        key->vertex_layout == RSX_VERTEX_LAYOUT_FALLBACK_36
            ? 9u * sizeof(float) : packed_slots * 4u * (unsigned)sizeof(float);
    buffer_description.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attributes[16];
    SDL_zero(attributes);
    unsigned attribute_count =
        key->vertex_layout == RSX_VERTEX_LAYOUT_FALLBACK_36 ? 3 : packed_slots;
    for (unsigned i = 0; i < attribute_count; ++i) {
        attributes[i].location = i;
        attributes[i].buffer_slot = 0;
        attributes[i].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attributes[i].offset = i * 4u * sizeof(float);
    }
    if (key->vertex_layout == RSX_VERTEX_LAYOUT_FALLBACK_36) {
        attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attributes[0].offset = 0;
        attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attributes[1].offset = 3u * sizeof(float);
        attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attributes[2].offset = 7u * sizeof(float);
    }
    SDL_GPUColorTargetDescription targets[4];
    SDL_zero(targets);
    unsigned color_count = op->color_count ? op->color_count : 1;
    for (unsigned i = 0; i < color_count; ++i) {
        targets[i].format = portable_format(op->color[i].format);
        targets[i].blend_state.enable_blend = key->blend_enable != 0;
        targets[i].blend_state.src_color_blendfactor = blend_factor(key->blend_sfactor, 0);
        targets[i].blend_state.dst_color_blendfactor = blend_factor(key->blend_dfactor, 0);
        targets[i].blend_state.color_blend_op = blend_op(key->blend_equation);
        targets[i].blend_state.src_alpha_blendfactor = blend_factor(key->blend_sfactor >> 16, 1);
        targets[i].blend_state.dst_alpha_blendfactor = blend_factor(key->blend_dfactor >> 16, 1);
        targets[i].blend_state.alpha_blend_op = blend_op(key->blend_equation >> 16);
        targets[i].blend_state.enable_color_write_mask = true;
        targets[i].blend_state.color_write_mask = key->color_write_mask
            ? (Uint8)(key->color_write_mask & 0xfu) : 0xfu;
    }
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(info);
    info.vertex_shader = vertex->shader;
    info.fragment_shader = fragment->shader;
    info.vertex_input_state.vertex_buffer_descriptions = &buffer_description;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = attribute_count;
    info.primitive_type = key->topology == RSX_TOPOLOGY_POINT_LIST
        ? SDL_GPU_PRIMITIVETYPE_POINTLIST : key->topology == RSX_TOPOLOGY_LINE_LIST
        ? SDL_GPU_PRIMITIVETYPE_LINELIST : SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = !key->cull_enable ? SDL_GPU_CULLMODE_NONE
        : ((key->cull_face & 0xfffu) == 0x404u ? SDL_GPU_CULLMODE_FRONT : SDL_GPU_CULLMODE_BACK);
    info.rasterizer_state.front_face = (key->front_face & 0xfffu) == 0x900u
        ? SDL_GPU_FRONTFACE_CLOCKWISE : SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.rasterizer_state.enable_depth_clip = true;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.depth_stencil_state.compare_op = compare_op(key->depth_func);
    info.depth_stencil_state.enable_depth_test = key->depth_test_enable != 0;
    info.depth_stencil_state.enable_depth_write = key->depth_write_enable != 0;
    SDL_GPUStencilOpState stencil;
    SDL_zero(stencil);
    stencil.compare_op = compare_op(key->stencil_func);
    stencil.fail_op = stencil_op(key->stencil_fail);
    stencil.depth_fail_op = stencil_op(key->stencil_zfail);
    stencil.pass_op = stencil_op(key->stencil_zpass);
    info.depth_stencil_state.front_stencil_state = stencil;
    info.depth_stencil_state.back_stencil_state = stencil;
    info.depth_stencil_state.compare_mask = (Uint8)(key->stencil_mask & 0xffu);
    /* SET_STENCIL_MASK (the write mask) is not yet represented by the
     * portable key. Its RSX reset value, and all observed Taiko masks, are
     * 0xff. Keep comparison masking independent from writes. */
    info.depth_stencil_state.write_mask = 0xffu;
    info.depth_stencil_state.enable_stencil_test =
        key->stencil_test_enable != 0;
    info.target_info.color_target_descriptions = targets;
    info.target_info.num_color_targets = color_count;
    if (op->depth.width && op->depth.height) {
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = portable_format(op->depth.format);
    }
    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(s_sdl.device, &info);
    if (!pipeline) {
        fprintf(stderr, "[SDL_GPU] pipeline creation failed vp=%016llx fp=%016llx: %s\n",
                (unsigned long long)key->vertex_shader_hash,
                (unsigned long long)key->fragment_shader_hash, SDL_GetError());
        ++s_sdl.errors; return NULL;
    }
    pipeline_entry* entry = &s_sdl.pipelines[s_sdl.pipeline_count++];
    entry->key = *key;
    entry->pipeline = pipeline;
    return pipeline;
}

static void release_dynamic_upload(dynamic_upload* upload)
{
    if (upload->transfer)
        SDL_ReleaseGPUTransferBuffer(s_sdl.device, upload->transfer);
    if (upload->buffer)
        SDL_ReleaseGPUBuffer(s_sdl.device, upload->buffer);
    memset(upload, 0, sizeof(*upload));
}

static void* map_dynamic_upload(dynamic_upload* upload, u64 size,
                                SDL_GPUBufferUsageFlags usage)
{
    if (!upload || !size || size > UINT32_MAX)
        return NULL;
    if (upload->capacity < size) {
        u32 capacity = 65536u;
        while (capacity < size && capacity <= UINT32_MAX / 2u)
            capacity *= 2u;
        if (capacity < size) capacity = (u32)size;
        release_dynamic_upload(upload);

        SDL_GPUBufferCreateInfo buffer_info;
        SDL_zero(buffer_info);
        buffer_info.usage = usage;
        buffer_info.size = capacity;
        upload->buffer = SDL_CreateGPUBuffer(s_sdl.device, &buffer_info);

        SDL_GPUTransferBufferCreateInfo transfer_info;
        SDL_zero(transfer_info);
        transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transfer_info.size = capacity;
        upload->transfer = SDL_CreateGPUTransferBuffer(s_sdl.device,
                                                       &transfer_info);
        if (!upload->buffer || !upload->transfer) {
            release_dynamic_upload(upload);
            goto fail;
        }
        upload->capacity = capacity;
    }
    void* mapped = SDL_MapGPUTransferBuffer(s_sdl.device, upload->transfer, true);
    if (mapped) return mapped;
fail:
    fprintf(stderr, "[SDL_GPU] buffer map failed: %s\n", SDL_GetError());
    ++s_sdl.errors;
    return NULL;
}

static SDL_GPUBuffer* commit_dynamic_upload(SDL_GPUCommandBuffer* commands,
                                            dynamic_upload* upload, u64 size)
{
    if (!commands || !upload || !upload->buffer || !upload->transfer ||
        !size || size > upload->capacity)
        return NULL;
    SDL_UnmapGPUTransferBuffer(s_sdl.device, upload->transfer);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    ++s_sdl.perf_copy_passes;
    if (!copy) goto fail;
    SDL_GPUTransferBufferLocation source = {upload->transfer, 0};
    SDL_GPUBufferRegion destination = {upload->buffer, 0, (u32)size};
    SDL_UploadToGPUBuffer(copy, &source, &destination, true);
    SDL_EndGPUCopyPass(copy);
    return upload->buffer;
fail:
    fprintf(stderr, "[SDL_GPU] buffer upload failed: %s\n", SDL_GetError());
    ++s_sdl.errors;
    return NULL;
}

static SDL_GPUTextureFormat sampled_format(rsx_texture_format format)
{
    switch (format) {
    case RSX_TEXTURE_R8: return SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    case RSX_TEXTURE_RGBA8: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    case RSX_TEXTURE_BC1: return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
    case RSX_TEXTURE_BC2: return SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM;
    case RSX_TEXTURE_BC3: return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
    default: return SDL_GPU_TEXTUREFORMAT_INVALID;
    }
}

static SDL_GPUTexture* get_sample_texture(const rsx_texture_source* source)
{
    u8* decoded = NULL;
    if (!source)
        return s_sdl.white_texture;
    /* Render-to-texture content lives only in the persistent GPU surface;
     * never replace it with a stale guest-memory snapshot. Every draw uses a
     * separate pass, so sampling also naturally ends the attachment pass. */
    for (unsigned i = 0; i < s_sdl.surface_count; ++i) {
        const gpu_surface* surface = &s_sdl.surfaces[i];
        if (!surface->ref.is_depth && surface->texture &&
            surface->ref.resolved_offset == source->resolved_offset &&
            surface->ref.width == source->width &&
            surface->ref.height == source->height)
            return surface->texture;
    }
    if (!source->payload.data || !source->payload.size)
        return s_sdl.white_texture;
    for (unsigned i = 0; i < s_sdl.texture_count; ++i) {
        texture_entry* entry = &s_sdl.textures[i];
        if (entry->hash == source->payload.hash && entry->width == source->width &&
            entry->height == source->height && entry->format == source->format) {
            entry->last_used = ++s_sdl.texture_use_clock;
            return entry->texture;
        }
    }
    if (getenv("SDL_GPU_DUMP_TEXTURES"))
        fprintf(stderr,
                "[SDL_GPU] texture %016llx format=%u %ux%u bytes=%llu head=%02x%02x%02x%02x\n",
                (unsigned long long)source->payload.hash, (unsigned)source->format,
                source->width, source->height,
                (unsigned long long)source->payload.size,
                source->payload.size > 0 ? source->payload.data[0] : 0,
                source->payload.size > 1 ? source->payload.data[1] : 0,
                source->payload.size > 2 ? source->payload.data[2] : 0,
                source->payload.size > 3 ? source->payload.data[3] : 0);
    SDL_GPUTextureFormat format = sampled_format(source->format);
    const void* upload_data = source->payload.data;
    u64 upload_size = source->payload.size;
    int supported = format != SDL_GPU_TEXTUREFORMAT_INVALID &&
        SDL_GPUTextureSupportsFormat(s_sdl.device, format,
                                     SDL_GPU_TEXTURETYPE_2D,
                                     SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!supported && source->format >= RSX_TEXTURE_BC1 &&
        source->format <= RSX_TEXTURE_BC3) {
        upload_size = (u64)source->width * source->height * 4u;
        if (upload_size > UINT32_MAX || !(decoded = (u8*)malloc((size_t)upload_size)) ||
            rsx_decode_bc_texture(source->format, source->payload.data,
                                  source->payload.size, source->pitch,
                                  source->width, source->height,
                                  decoded, upload_size) != 0) {
            free(decoded);
            fprintf(stderr, "[SDL_GPU] BC%u CPU decode failed\n",
                    (unsigned)source->format - RSX_TEXTURE_BC1 + 1u);
            ++s_sdl.errors;
            return s_sdl.white_texture;
        }
        static unsigned logged_bc_fallback = 0;
        if (!logged_bc_fallback++)
            fprintf(stderr, "[SDL_GPU] compressed textures unsupported; using RGBA8 CPU decode\n");
        format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        upload_data = decoded;
        supported = SDL_GPUTextureSupportsFormat(
            s_sdl.device, format, SDL_GPU_TEXTURETYPE_2D,
            SDL_GPU_TEXTUREUSAGE_SAMPLER);
    }
    if (!supported || upload_size > UINT32_MAX) {
        free(decoded);
        fprintf(stderr, "[SDL_GPU] sampled format %u unsupported\n",
                (unsigned)source->format);
        ++s_sdl.errors;
        return s_sdl.white_texture;
    }
    SDL_GPUTextureCreateInfo texture_info;
    SDL_zero(texture_info);
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = format;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture_info.width = source->width;
    texture_info.height = source->height;
    texture_info.layer_count_or_depth = 1;
    texture_info.num_levels = 1;
    texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(s_sdl.device, &texture_info);
    SDL_GPUTransferBufferCreateInfo transfer_info;
    SDL_zero(transfer_info);
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = (u32)upload_size;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(
        s_sdl.device, &transfer_info);
    if (!texture || !transfer) goto fail;
    void* mapped = SDL_MapGPUTransferBuffer(s_sdl.device, transfer, false);
    if (!mapped) goto fail;
    memcpy(mapped, upload_data, (size_t)upload_size);
    SDL_UnmapGPUTransferBuffer(s_sdl.device, transfer);
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(s_sdl.device);
    if (!commands) goto fail;
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    ++s_sdl.perf_copy_passes;
    if (!copy) { SDL_CancelGPUCommandBuffer(commands); goto fail; }
    SDL_GPUTextureTransferInfo upload;
    SDL_zero(upload);
    upload.transfer_buffer = transfer;
    upload.pixels_per_row = source->width;
    upload.rows_per_layer = source->height;
    SDL_GPUTextureRegion destination;
    SDL_zero(destination);
    destination.texture = texture;
    destination.w = source->width;
    destination.h = source->height;
    destination.d = 1;
    SDL_UploadToGPUTexture(copy, &upload, &destination, false);
    SDL_EndGPUCopyPass(copy);
    if (submit_commands(commands) != 0) goto fail;
    SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer);
    free(decoded);
    texture_entry* entry;
    if (s_sdl.texture_count < s_sdl.texture_limit) {
        entry = &s_sdl.textures[s_sdl.texture_count++];
    } else {
        unsigned oldest = 0;
        for (unsigned i = 1; i < s_sdl.texture_count; ++i)
            if (s_sdl.textures[i].last_used < s_sdl.textures[oldest].last_used)
                oldest = i;
        entry = &s_sdl.textures[oldest];
        /* SDL defers destruction until the GPU no longer references the
         * texture, so replacing an LRU entry does not require an idle wait. */
        SDL_ReleaseGPUTexture(s_sdl.device, entry->texture);
        if (s_sdl.texture_evictions++ == 0)
            fprintf(stderr, "[SDL_GPU] texture cache full; using LRU eviction\n");
    }
    entry->hash = source->payload.hash;
    entry->width = source->width;
    entry->height = source->height;
    entry->format = source->format;
    entry->texture = texture;
    entry->last_used = ++s_sdl.texture_use_clock;
    return texture;
fail:
    free(decoded);
    if (transfer) SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer);
    if (texture) SDL_ReleaseGPUTexture(s_sdl.device, texture);
    fprintf(stderr, "[SDL_GPU] texture upload failed: %s\n", SDL_GetError());
    ++s_sdl.errors;
    return s_sdl.white_texture;
}

static SDL_GPUSamplerAddressMode sampler_address(u32 mode)
{
    static unsigned warned_border = 0, warned_mirror_once = 0;
    switch (mode & 0xfu) {
    case 1: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    case 2: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    case 4:
        if (!warned_border++)
            fprintf(stderr, "[SDL_GPU] border address mode falls back to clamp\n");
        ++s_sdl.errors;
        return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    case 6: case 7: case 8:
        if (!warned_mirror_once++)
            fprintf(stderr, "[SDL_GPU] mirror-once address mode falls back to clamp\n");
        ++s_sdl.errors;
        return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    default: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    }
}

static SDL_GPUSampler* get_sampler(u32 address)
{
    address &= 0x000f0f0fu;
    for (unsigned i = 0; i < s_sdl.sampler_count; ++i)
        if (s_sdl.samplers[i].address == address)
            return s_sdl.samplers[i].sampler;
    if (!address) return s_sdl.default_sampler;
    if (s_sdl.sampler_count == SDL_RSX_MAX_SAMPLERS) {
        ++s_sdl.errors; return s_sdl.default_sampler;
    }
    SDL_GPUSamplerCreateInfo info;
    SDL_zero(info);
    info.min_filter = info.mag_filter = SDL_GPU_FILTER_LINEAR;
    info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    info.address_mode_u = sampler_address(address);
    info.address_mode_v = sampler_address(address >> 8);
    info.address_mode_w = sampler_address(address >> 16);
    info.max_lod = 1000.0f;
    SDL_GPUSampler* sampler = SDL_CreateGPUSampler(s_sdl.device, &info);
    if (!sampler) {
        fprintf(stderr, "[SDL_GPU] sampler creation failed: %s\n", SDL_GetError());
        ++s_sdl.errors; return s_sdl.default_sampler;
    }
    sampler_entry* entry = &s_sdl.samplers[s_sdl.sampler_count++];
    entry->address = address;
    entry->sampler = sampler;
    return sampler;
}

static void execute_diagnostic_fallback(SDL_GPUCommandBuffer* commands,
                                        const rsx_render_op* op)
{
    SDL_GPUColorTargetInfo targets[4];
    SDL_zero(targets);
    unsigned count = 0;
    for (unsigned i = 0; i < op->color_count && i < 4; ++i) {
        SDL_GPUTexture* texture = get_surface(&op->color[i]);
        if (!texture) continue;
        targets[count].texture = texture;
        targets[count].clear_color.r = 1.0f;
        targets[count].clear_color.b = 1.0f;
        targets[count].clear_color.a = 1.0f;
        targets[count].load_op = SDL_GPU_LOADOP_CLEAR;
        targets[count].store_op = SDL_GPU_STOREOP_STORE;
        ++count;
    }
    SDL_GPURenderPass* pass = count
        ? SDL_BeginGPURenderPass(commands, targets, count, NULL) : NULL;
    if (!pass) return;
    SDL_EndGPURenderPass(pass);
}

/* `clear` is a guest clear of the same attachments that immediately precedes
 * this pass. Folding it in as a colour LOADOP_CLEAR is exactly equivalent --
 * execute_clear() clears the whole attachment, with no scissor -- and saves a
 * whole render pass: on a tiler that is a full tile-buffer store plus the load
 * this pass would otherwise have done. Half of the passes in a Taiko frame are
 * clear-then-draw pairs on the same target. */
#ifdef RSX_SDL_REPLAY_STANDALONE
void (*g_rsx_replay_key_hook)(int scancode);
#endif

static SDL_GPURenderPass* begin_draw_pass(SDL_GPUCommandBuffer* commands,
                                          const rsx_render_op* op,
                                          const rsx_render_op* clear)
{
    SDL_GPUColorTargetInfo colors[4];
    SDL_zero(colors);
    unsigned color_count = 0;
    const int clear_color = clear && (clear->data.clear.flags & 0xf0u);
    for (unsigned i = 0; i < op->color_count && i < 4; ++i) {
        SDL_GPUTexture* texture = get_surface(&op->color[i]);
        if (!texture) continue;
        colors[color_count].texture = texture;
        colors[color_count].load_op = clear_color ? SDL_GPU_LOADOP_CLEAR
                                                  : SDL_GPU_LOADOP_LOAD;
        if (clear_color) {
            colors[color_count].clear_color.r = clear->data.clear.color[0];
            colors[color_count].clear_color.g = clear->data.clear.color[1];
            colors[color_count].clear_color.b = clear->data.clear.color[2];
            colors[color_count].clear_color.a = clear->data.clear.color[3];
        }
        colors[color_count].store_op = SDL_GPU_STOREOP_STORE;
        ++color_count;
    }
    SDL_GPUDepthStencilTargetInfo depth;
    SDL_zero(depth);
    SDL_GPUDepthStencilTargetInfo* depth_ptr = NULL;
    SDL_GPUTexture* depth_texture = get_surface(&op->depth);
    if (depth_texture) {
        depth.texture = depth_texture;
        /* Match the established D3D12 renderer: its depth buffer is shared
         * and reset whenever the color-target chain changes. Loading the
         * newly-created/previous target's depth here leaves undefined or
         * stale values that reject every fragment after the boot checks. */
        depth.clear_depth = 1.0f;
        depth.clear_stencil = 0;
        depth.load_op = SDL_GPU_LOADOP_CLEAR;
        depth.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
        /* Every pass clears depth/stencil on entry and nothing samples the
         * buffer, so its contents are never read back. Storing it anyway costs
         * a full tile write-out per pass, which a tiler like the Pi's V3D pays
         * in raw memory bandwidth: ten passes a frame at 1280x720 is ~36 MB/f. */
        depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
        depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        depth_ptr = &depth;
    }
    if (!color_count) return NULL;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(
        commands, colors, color_count, depth_ptr);
    if (!pass) {
        fprintf(stderr, "[SDL_GPU] draw pass failed: %s\n", SDL_GetError());
        ++s_sdl.errors;
    }
    return pass;
}

static int same_draw_attachments(const rsx_render_op* a,
                                 const rsx_render_op* b)
{
    unsigned a_count = a->color_count < 4 ? a->color_count : 4;
    unsigned b_count = b->color_count < 4 ? b->color_count : 4;
    if (a_count != b_count) return 0;
    for (unsigned i = 0; i < a_count; ++i)
        if (get_surface(&a->color[i]) != get_surface(&b->color[i]))
            return 0;
    return get_surface(&a->depth) == get_surface(&b->depth);
}

static void execute_draw(SDL_GPUCommandBuffer* commands,
                         SDL_GPURenderPass* pass,
                         const rsx_render_op* op,
                         SDL_GPUBuffer* vertices,
                         u32 vertex_offset,
                         SDL_GPUBuffer* vertex_constants,
                         u32 vertex_constant_base)
{
    SDL_GPUGraphicsPipeline* pipeline = get_pipeline(op);
    if (!pipeline || !pass || !vertices || vertex_offset == UINT32_MAX) return;
    shader_entry* vertex_shader = get_shader(op, SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
    shader_entry* fragment_shader = get_shader(op, SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
    SDL_GPUTextureSamplerBinding bindings[4];
    unsigned sampler_count = fragment_shader
        ? fragment_shader->sampler_unit_count : 0;
    if (sampler_count > 4) sampler_count = 4;
    for (unsigned slot = 0; slot < sampler_count; ++slot) {
        unsigned unit = fragment_shader->sampler_units[slot];
        bindings[slot].texture = unit < op->data.draw.texture_count
            ? get_sample_texture(&op->data.draw.textures[unit])
            : s_sdl.white_texture;
        bindings[slot].sampler = unit < op->data.draw.texture_count
            ? get_sampler(op->data.draw.textures[unit].address)
            : s_sdl.default_sampler;
    }
    if (!vertex_shader || !fragment_shader) return;
    if (vertex_shader->resources.num_uniform_buffers) {
        u32 base_uniform[4] = {vertex_constant_base, 0, 0, 0};
        SDL_PushGPUVertexUniformData(commands, 0, base_uniform,
                                     sizeof(base_uniform));
    }
    float fragment_uniforms[9][4] = {
        {1,1,1,1}, {1,1,1,1}, {1,1,1,1}, {1,1,1,1},
        {op->data.draw.pipeline.alpha_test_enable ? 1.0f : 0.0f,
         (float)(op->data.draw.pipeline.alpha_ref & 0xffu) / 255.0f,
         (float)(op->data.draw.pipeline.alpha_func & 7u), 0.0f},
        {0,1,2,3}, {0,1,2,3}, {0,1,2,3}, {0,1,2,3}
    };
    for (unsigned i = 0; i < op->data.draw.texture_count && i < 4; ++i) {
        const rsx_texture_source* texture = &op->data.draw.textures[i];
        if ((texture->flags & RSX_TEXTURE_FLAG_UNNORMALIZED_COORDS) &&
            texture->width && texture->height) {
            fragment_uniforms[i][0] = 1.0f / (float)texture->width;
            fragment_uniforms[i][1] = 1.0f / (float)texture->height;
        }
        u8 remap[4];
        rsx_texture_component_remap(texture->control1, remap);
        for (unsigned channel = 0; channel < 4; ++channel)
            fragment_uniforms[5 + i][channel] = (float)remap[channel];
    }
    if (fragment_shader->resources.num_uniform_buffers)
        SDL_PushGPUFragmentUniformData(commands, 0, fragment_uniforms,
                                       sizeof(fragment_uniforms));
    if (fragment_shader->resources.num_uniform_buffers > 1) {
        float fp_constants[RSX_FP_MAX_INLINE_CONSTANTS][4] = {{0}};
        rsx_fp_extract_constants(
            op->data.draw.fragment_shader.data,
            (u32)op->data.draw.fragment_shader.size,
            &fp_constants[0][0], RSX_FP_MAX_INLINE_CONSTANTS);
        SDL_PushGPUFragmentUniformData(commands, 1, fp_constants,
                                       sizeof(fp_constants));
    }
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    if (op->data.draw.pipeline.stencil_test_enable)
        SDL_SetGPUStencilReference(
            pass, (Uint8)(op->data.draw.pipeline.stencil_ref & 0xffu));
    SDL_GPUBufferBinding vertex_binding = {vertices, vertex_offset};
    SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
    if (vertex_shader->resources.num_storage_buffers && vertex_constants)
        SDL_BindGPUVertexStorageBuffers(pass, 0, &vertex_constants, 1);
    if (sampler_count) {
        SDL_BindGPUFragmentSamplers(pass, 0, bindings, sampler_count);
    }
    SDL_GPUViewport viewport = {
        (float)op->viewport[0], (float)op->viewport[1],
        (float)(op->viewport[2] ? op->viewport[2] : op->color[0].width),
        (float)(op->viewport[3] ? op->viewport[3] : op->color[0].height),
        0.0f, 1.0f
    };
    SDL_SetGPUViewport(pass, &viewport);
    SDL_Rect scissor = {
        (int)op->scissor[0], (int)op->scissor[1],
        (int)(op->scissor[2] ? op->scissor[2] : op->color[0].width),
        (int)(op->scissor[3] ? op->scissor[3] : op->color[0].height)
    };
    const int character_margin = character_scissor_margin();
    if (character_margin >= 0 && is_character_outline_filter(op)) {
        intersect_scissor(&scissor, character_margin, character_margin,
                          600 - 2 * character_margin,
                          600 - 2 * character_margin);
        static int outline_announced;
        if (!outline_announced) {
            outline_announced = 1;
            fprintf(stderr,
                    "[SDL_GPU] character outline filter scissor "
                    "%d,%d %dx%d\n", scissor.x, scissor.y,
                    scissor.w, scissor.h);
        }
    } else if (character_margin >= 0) {
        SDL_Rect composite_scissor;
        if (character_composite_scissor(op, character_margin,
                                        &composite_scissor)) {
            intersect_scissor(&scissor, composite_scissor.x,
                              composite_scissor.y, composite_scissor.w,
                              composite_scissor.h);
            static int composite_announced;
            if (!composite_announced) {
                composite_announced = 1;
                fprintf(stderr,
                        "[SDL_GPU] character display composite scissor "
                        "%d,%d %dx%d\n", scissor.x, scissor.y,
                        scissor.w, scissor.h);
            }
        }
    }
    SDL_SetGPUScissor(pass, &scissor);
    SDL_FColor blend = {
        ((op->data.draw.pipeline.blend_color >> 16) & 255u) / 255.0f,
        ((op->data.draw.pipeline.blend_color >> 8) & 255u) / 255.0f,
        (op->data.draw.pipeline.blend_color & 255u) / 255.0f,
        ((op->data.draw.pipeline.blend_color >> 24) & 255u) / 255.0f
    };
    SDL_SetGPUBlendConstants(pass, blend);
    SDL_SetGPUStencilReference(pass,
        (Uint8)op->data.draw.pipeline.stencil_ref);
    SDL_DrawGPUPrimitives(pass, op->data.draw.vertex_count, 1, 0, 0);
}

static SDL_GPUTexture* presentation_texture(Uint32* source_width,
                                            Uint32* source_height)
{
    SDL_GPUTexture* result = s_sdl.presentation_override
        ? s_sdl.presentation_override : s_sdl.display;
    *source_width = SDL_RSX_WIDTH;
    *source_height = SDL_RSX_HEIGHT;
    const char* requested = getenv("SDL_GPU_VIEW_SURFACE");
    if (!requested || !requested[0]) return result;
    char* end = NULL;
    unsigned long offset = strtoul(requested, &end, 0);
    if (!end || *end || offset > UINT32_MAX) return result;
    for (unsigned i = 0; i < s_sdl.surface_count; ++i) {
        const gpu_surface* surface = &s_sdl.surfaces[i];
        if (!surface->ref.is_depth && surface->texture &&
            surface->ref.format == RSX_FORMAT_RGBA8 &&
            (surface->ref.raw_offset == (u32)offset ||
             surface->ref.resolved_offset == (u32)offset)) {
            *source_width = surface->ref.width;
            *source_height = surface->ref.height;
            return surface->texture;
        }
    }
    return result;
}

/* Optional overlay, supplied by the title layer (src/taiko_overlay.c): a
 * straight-alpha RGBA image drawn over the presented frame. Windowed output
 * uses the GPU quad below; direct KMS blends it during the scanout CPU copy.
 * A build without the title hook simply draws nothing. */
const uint32_t* (*g_rsx_overlay_frame)(int* width, int* height,
                                      uint32_t* version);

static struct {
    SDL_GPUTexture* texture;
    int width, height;
    uint32_t version;
    int uploaded;
    SDL_GPUGraphicsPipeline* pipeline;
    SDL_GPUSampler* sampler;
    SDL_GPUTextureFormat pipeline_format;
} s_overlay;

static struct {
    SDL_GPUTexture* texture;
    uint32_t pixels[FPS_OVERLAY_WIDTH * FPS_OVERLAY_HEIGHT];
    uint32_t cpu_version;
    uint32_t gpu_version;
    char text[8];
} s_fps_overlay;

/* Five pixels wide, seven high. Only digits are needed; the decimal point is
 * drawn separately. Keeping this here avoids FreeType work and a large font
 * dependency in the once-per-second counter update. */
static const uint8_t k_fps_digits[10][7] = {
    {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e},
    {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},
    {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f},
    {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e},
    {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02},
    {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e},
    {0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e},
    {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e},
    {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e},
};

/* A textured quad with straight alpha blending -- the overlay artwork has
 * rounded, transparent ends, and a blit would punch them into the frame as
 * holes. Four vertices from SV_VertexID, so there is no vertex buffer. */
static const char* k_overlay_vertex_hlsl =
    "cbuffer Rect : register(b0, space1) { float4 rect; };\n"
    "struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };\n"
    "Output main(uint id : SV_VertexID) {\n"
    "    float2 corner = float2(id & 1u, (id >> 1u) & 1u);\n"
    "    Output output;\n"
    "    output.position = float4(rect.xy + corner * rect.zw, 0.0f, 1.0f);\n"
    "    output.uv = corner;\n"
    "    return output;\n"
    "}\n";

static const char* k_overlay_fragment_hlsl =
    "Texture2D source : register(t0, space2);\n"
    "SamplerState state : register(s0, space2);\n"
    "float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "    return source.Sample(state, uv);\n"
    "}\n";

static int overlay_pipeline_ready(SDL_GPUTextureFormat format)
{
    if (s_overlay.pipeline && s_overlay.pipeline_format == format)
        return 1;
    if (s_overlay.pipeline) {
        SDL_ReleaseGPUGraphicsPipeline(s_sdl.device, s_overlay.pipeline);
        s_overlay.pipeline = NULL;
    }

    SDL_ShaderCross_GraphicsShaderResourceInfo vertex_resources;
    SDL_ShaderCross_GraphicsShaderResourceInfo fragment_resources;
    SDL_zero(vertex_resources);
    SDL_zero(fragment_resources);
    SDL_GPUShader* vertex = compile_hlsl(k_overlay_vertex_hlsl,
        SDL_SHADERCROSS_SHADERSTAGE_VERTEX, &vertex_resources, 0);
    SDL_GPUShader* fragment = compile_hlsl(k_overlay_fragment_hlsl,
        SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT, &fragment_resources, 1);
    if (!vertex || !fragment) {
        if (vertex) SDL_ReleaseGPUShader(s_sdl.device, vertex);
        if (fragment) SDL_ReleaseGPUShader(s_sdl.device, fragment);
        return 0;
    }

    SDL_GPUColorTargetBlendState blend;
    SDL_zero(blend);
    blend.enable_blend = true;
    blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
    blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    blend.color_write_mask = 0xF;

    SDL_GPUColorTargetDescription target;
    SDL_zero(target);
    target.format = format;
    target.blend_state = blend;

    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(info);
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.num_color_targets = 1;
    info.target_info.color_target_descriptions = &target;
    s_overlay.pipeline = SDL_CreateGPUGraphicsPipeline(s_sdl.device, &info);
    SDL_ReleaseGPUShader(s_sdl.device, vertex);
    SDL_ReleaseGPUShader(s_sdl.device, fragment);
    if (!s_overlay.pipeline) {
        fprintf(stderr, "[SDL_GPU] overlay pipeline failed: %s\n", SDL_GetError());
        ++s_sdl.errors;
        return 0;
    }
    s_overlay.pipeline_format = format;

    if (!s_overlay.sampler) {
        SDL_GPUSamplerCreateInfo sampler;
        SDL_zero(sampler);
        sampler.min_filter = SDL_GPU_FILTER_LINEAR;
        sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
        sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        s_overlay.sampler = SDL_CreateGPUSampler(s_sdl.device, &sampler);
    }
    return s_overlay.sampler != NULL;
}

static void fps_overlay_set_value(double fps)
{
    char text[8];
    if (fps < 0.0) fps = 0.0;
    if (fps > 999.0) fps = 999.0;
    if (fps >= 99.95)
        snprintf(text, sizeof(text), "%.0f", fps);
    else
        snprintf(text, sizeof(text), "%.1f", fps);
    if (strcmp(text, s_fps_overlay.text) == 0) return;
    snprintf(s_fps_overlay.text, sizeof(s_fps_overlay.text), "%s", text);

    for (unsigned i = 0; i < SDL_arraysize(s_fps_overlay.pixels); ++i)
        s_fps_overlay.pixels[i] = 0xff000000u;

    int text_width = 0;
    for (const char* c = text; *c; ++c)
        text_width += (*c == '.' ? 2 : 6) * FPS_GLYPH_SCALE;
    if (text_width) text_width -= FPS_GLYPH_SCALE;
    int pen_x = (FPS_OVERLAY_WIDTH - text_width) / 2;
    const int top = (FPS_OVERLAY_HEIGHT - 7 * FPS_GLYPH_SCALE) / 2;
    for (const char* c = text; *c; ++c) {
        if (*c == '.') {
            for (int y = 0; y < FPS_GLYPH_SCALE; ++y)
                for (int x = 0; x < FPS_GLYPH_SCALE; ++x)
                    s_fps_overlay.pixels[
                        (top + 6 * FPS_GLYPH_SCALE + y) * FPS_OVERLAY_WIDTH +
                        pen_x + x] = 0xffffffffu;
            pen_x += 2 * FPS_GLYPH_SCALE;
            continue;
        }
        if (*c < '0' || *c > '9') continue;
        const uint8_t* glyph = k_fps_digits[*c - '0'];
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 5; ++column) {
                if (!(glyph[row] & (1u << (4 - column)))) continue;
                for (int y = 0; y < FPS_GLYPH_SCALE; ++y)
                    for (int x = 0; x < FPS_GLYPH_SCALE; ++x)
                        s_fps_overlay.pixels[
                            (top + row * FPS_GLYPH_SCALE + y) *
                                FPS_OVERLAY_WIDTH +
                            pen_x + column * FPS_GLYPH_SCALE + x] =
                                0xffffffffu;
            }
        }
        pen_x += 6 * FPS_GLYPH_SCALE;
    }
    ++s_fps_overlay.cpu_version;
}

/* Record the tiny upload alongside the frame's existing dynamic-buffer
 * uploads. The Pi's normal upload fence therefore covers it too, without an
 * extra submission or a same-command-buffer visibility gamble. */
static int upload_fps_overlay(SDL_GPUCommandBuffer* commands)
{
    if (s_sdl.kms_present || !s_sdl.perf_overlay ||
        !s_fps_overlay.cpu_version ||
        s_fps_overlay.gpu_version == s_fps_overlay.cpu_version)
        return 0;
    if (!s_fps_overlay.texture) {
        SDL_GPUTextureCreateInfo info;
        SDL_zero(info);
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = FPS_OVERLAY_WIDTH;
        info.height = FPS_OVERLAY_HEIGHT;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        s_fps_overlay.texture = SDL_CreateGPUTexture(s_sdl.device, &info);
        if (!s_fps_overlay.texture) return 0;
    }

    SDL_GPUTransferBufferCreateInfo transfer_info;
    SDL_zero(transfer_info);
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = sizeof(s_fps_overlay.pixels);
    SDL_GPUTransferBuffer* transfer =
        SDL_CreateGPUTransferBuffer(s_sdl.device, &transfer_info);
    if (!transfer) return 0;
    void* mapped = SDL_MapGPUTransferBuffer(s_sdl.device, transfer, false);
    if (!mapped) {
        SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer);
        return 0;
    }
    memcpy(mapped, s_fps_overlay.pixels, sizeof(s_fps_overlay.pixels));
    SDL_UnmapGPUTransferBuffer(s_sdl.device, transfer);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer);
        return 0;
    }
    ++s_sdl.perf_copy_passes;
    SDL_GPUTextureTransferInfo upload;
    SDL_zero(upload);
    upload.transfer_buffer = transfer;
    upload.pixels_per_row = FPS_OVERLAY_WIDTH;
    upload.rows_per_layer = FPS_OVERLAY_HEIGHT;
    SDL_GPUTextureRegion destination;
    SDL_zero(destination);
    destination.texture = s_fps_overlay.texture;
    destination.w = FPS_OVERLAY_WIDTH;
    destination.h = FPS_OVERLAY_HEIGHT;
    destination.d = 1;
    SDL_UploadToGPUTexture(copy, &upload, &destination, false);
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer);
    s_fps_overlay.gpu_version = s_fps_overlay.cpu_version;
    return 1;
}

static int draw_fps_overlay(SDL_GPUCommandBuffer* commands,
                            SDL_GPUTexture* target_texture,
                            Uint32 target_width, Uint32 target_height,
                            Uint32 frame_x, Uint32 frame_y,
                            Uint32 frame_width, Uint32 frame_height,
                            SDL_GPUTextureFormat target_format)
{
    if (!s_sdl.perf_overlay || !s_fps_overlay.texture ||
        !s_fps_overlay.gpu_version || !overlay_pipeline_ready(target_format))
        return 0;
    const float scale = (float)frame_width / (float)SDL_RSX_WIDTH;
    const float draw_w = FPS_OVERLAY_WIDTH * scale;
    const float draw_h = FPS_OVERLAY_HEIGHT * scale;
    const float x = (float)frame_x + 12.0f * scale;
    const float y = (float)frame_y + 12.0f * scale;
    const float rect[4] = {
        x / (float)target_width * 2.0f - 1.0f,
        1.0f - y / (float)target_height * 2.0f,
        draw_w / (float)target_width * 2.0f,
        -draw_h / (float)target_height * 2.0f,
    };
    SDL_GPUColorTargetInfo target;
    SDL_zero(target);
    target.texture = target_texture;
    target.load_op = SDL_GPU_LOADOP_LOAD;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &target, 1, NULL);
    if (!pass) return 0;
    SDL_BindGPUGraphicsPipeline(pass, s_overlay.pipeline);
    SDL_GPUTextureSamplerBinding binding;
    SDL_zero(binding);
    binding.texture = s_fps_overlay.texture;
    binding.sampler = s_overlay.sampler;
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_PushGPUVertexUniformData(commands, 0, rect, sizeof(rect));
    SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    return 1;
}

/* Upload when the pixels changed, then blit into the frame's top-left. */
static void draw_overlay(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* swapchain,
                         Uint32 swapchain_width, Uint32 swapchain_height,
                         Uint32 frame_x, Uint32 frame_y,
                         Uint32 frame_width, Uint32 frame_height,
                         SDL_GPUTextureFormat target_format)
{
    int width = 0, height = 0;
    uint32_t version = 0;
    const uint32_t* pixels =
        g_rsx_overlay_frame ? g_rsx_overlay_frame(&width, &height, &version) : NULL;
    if (!pixels || width <= 0 || height <= 0) return;

    if (s_overlay.texture &&
        (s_overlay.width != width || s_overlay.height != height)) {
        SDL_ReleaseGPUTexture(s_sdl.device, s_overlay.texture);
        s_overlay.texture = NULL;
    }
    if (!s_overlay.texture) {
        SDL_GPUTextureCreateInfo info;
        SDL_zero(info);
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = (Uint32)width;
        info.height = (Uint32)height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        s_overlay.texture = SDL_CreateGPUTexture(s_sdl.device, &info);
        if (!s_overlay.texture) return;
        s_overlay.width = width;
        s_overlay.height = height;
        s_overlay.uploaded = 0;
    }
    if (!s_overlay.uploaded || s_overlay.version != version) {
        const Uint32 size = (Uint32)width * (Uint32)height * 4u;
        SDL_GPUTransferBufferCreateInfo transfer_info;
        SDL_zero(transfer_info);
        transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transfer_info.size = size;
        SDL_GPUTransferBuffer* transfer =
            SDL_CreateGPUTransferBuffer(s_sdl.device, &transfer_info);
        if (!transfer) return;
        void* mapped = SDL_MapGPUTransferBuffer(s_sdl.device, transfer, false);
        if (!mapped) {
            SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer);
            return;
        }
        memcpy(mapped, pixels, size);
        SDL_UnmapGPUTransferBuffer(s_sdl.device, transfer);

        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        ++s_sdl.perf_copy_passes;
        if (copy) {
            SDL_GPUTextureTransferInfo upload;
            SDL_zero(upload);
            upload.transfer_buffer = transfer;
            upload.pixels_per_row = (Uint32)width;
            upload.rows_per_layer = (Uint32)height;
            SDL_GPUTextureRegion destination;
            SDL_zero(destination);
            destination.texture = s_overlay.texture;
            destination.w = (Uint32)width;
            destination.h = (Uint32)height;
            destination.d = 1;
            SDL_UploadToGPUTexture(copy, &upload, &destination, false);
            SDL_EndGPUCopyPass(copy);
            s_overlay.version = version;
            s_overlay.uploaded = 1;
        }
        SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer);
    }
    if (!s_overlay.uploaded || !overlay_pipeline_ready(target_format)) return;

    /* Sized and placed against the *presented frame*, not the window: the
     * frame is letterboxed inside it, and an overlay measured in window pixels
     * drifts outside the picture. 0.22 of the frame width is what the artwork
     * covers on the cabinet's own 1280-wide screen. */
    float draw_w = (float)frame_width * 0.22f;
    float draw_h = draw_w * (float)height / (float)width;
    const float x = (float)frame_x + ((float)frame_width - draw_w) * 0.5f;
    const float y = (float)frame_y + (float)frame_height * 0.04f;

    /* Pixels -> normalised device coordinates (y grows downwards on screen). */
    const float rect[4] = {
        x / (float)swapchain_width * 2.0f - 1.0f,
        1.0f - y / (float)swapchain_height * 2.0f,
        draw_w / (float)swapchain_width * 2.0f,
        -draw_h / (float)swapchain_height * 2.0f,
    };

    SDL_GPUColorTargetInfo target;
    SDL_zero(target);
    target.texture = swapchain;
    target.load_op = SDL_GPU_LOADOP_LOAD;    /* keep the frame underneath */
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &target, 1, NULL);
    if (!pass) return;
    SDL_BindGPUGraphicsPipeline(pass, s_overlay.pipeline);
    SDL_GPUTextureSamplerBinding binding;
    SDL_zero(binding);
    binding.texture = s_overlay.texture;
    binding.sampler = s_overlay.sampler;
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_PushGPUVertexUniformData(commands, 0, rect, sizeof(rect));
    SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

#ifdef RSX_SDL_KMS_PRESENT
/* Getting a rendered frame onto a scanout buffer costs a GPU download and a
 * CPU copy. The download is collected by this worker, and the CPU copy runs
 * here while the next frame renders. Keep the fence wait, copy and KMS update
 * timings separate: earlier aggregate measurements could not identify which
 * stage owned the cost. */
static int kms_submit_download(struct kms_slot* slot,
                               SDL_GPUCommandBuffer* commands)
{
    const Uint64 issue_start_ns = SDL_GetTicksNS();
    SDL_GPUCommandBuffer* copy_commands = commands;
    if (!copy_commands)
        copy_commands = SDL_AcquireGPUCommandBuffer(s_sdl.device);
    if (!copy_commands) return -1;

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(copy_commands);
    if (!copy) {
        SDL_CancelGPUCommandBuffer(copy_commands);
        return -1;
    }
    SDL_GPUTextureRegion region;
    SDL_zero(region);
    region.texture = slot->source;
    region.w = slot->width;
    region.h = slot->height;
    region.d = 1;
    SDL_GPUTextureTransferInfo destination;
    SDL_zero(destination);
    destination.transfer_buffer = slot->transfer;
    destination.pixels_per_row = slot->width;
    destination.rows_per_layer = slot->height;
    SDL_DownloadFromGPUTexture(copy, &region, &destination);
    SDL_EndGPUCopyPass(copy);
    __atomic_fetch_add(&s_sdl.perf_submits, 1u, __ATOMIC_RELAXED);
    slot->fence = SDL_SubmitGPUCommandBufferAndAcquireFence(copy_commands);
    if (!slot->fence) return -1;
    __atomic_fetch_add(&s_sdl.perf_kms_download_ns,
                       SDL_GetTicksNS() - issue_start_ns, __ATOMIC_RELAXED);
    return 0;
}

static int kms_copy_worker(void* userdata)
{
    (void)userdata;
    for (;;) {
        SDL_LockMutex(s_sdl.kms_mutex);
        while (!s_sdl.kms_pending_count && !s_sdl.kms_stop)
            SDL_WaitCondition(s_sdl.kms_work, s_sdl.kms_mutex);
        if (s_sdl.kms_stop && !s_sdl.kms_pending_count) {
            SDL_UnlockMutex(s_sdl.kms_mutex);
            return 0;
        }
        const int index = s_sdl.kms_pending[0];
        for (unsigned i = 1; i < s_sdl.kms_pending_count; ++i)
            s_sdl.kms_pending[i - 1] = s_sdl.kms_pending[i];
        --s_sdl.kms_pending_count;
        SDL_BroadcastCondition(s_sdl.kms_free);
        SDL_UnlockMutex(s_sdl.kms_mutex);

        struct kms_slot* slot = &s_sdl.kms_slots[index];
        const Uint64 wait_start_ns = SDL_GetTicksNS();
        int download_ready = 1;
        if (slot->fence) {
            SDL_GPUFence* fences[1] = {slot->fence};
            if (!SDL_WaitForGPUFences(s_sdl.device, true, fences, 1)) {
                fprintf(stderr, "[SDL_GPU] KMS download fence wait failed: %s\n",
                        SDL_GetError());
                ++s_sdl.errors;
                download_ready = 0;
            }
            SDL_ReleaseGPUFence(s_sdl.device, slot->fence);
            slot->fence = NULL;
        }
        const Uint64 wait_end_ns = SDL_GetTicksNS();
        Uint64 scanout_wait_ns = 0, copy_ns = 0, flip_ns = 0;
        void* mapped = download_ready
            ? SDL_MapGPUTransferBuffer(s_sdl.device, slot->transfer, false)
            : NULL;
        if (mapped) {
            rsx_kms_present_frame(
                mapped, slot->pitch,
                slot->fps_overlay_enabled ? slot->fps_overlay : NULL,
                FPS_OVERLAY_WIDTH * 4u, 12u, 12u,
                FPS_OVERLAY_WIDTH, FPS_OVERLAY_HEIGHT,
                &scanout_wait_ns, &copy_ns, &flip_ns);
            SDL_UnmapGPUTransferBuffer(s_sdl.device, slot->transfer);
        } else if (download_ready) {
            fprintf(stderr, "[SDL_GPU] KMS transfer-buffer map failed: %s\n",
                    SDL_GetError());
            ++s_sdl.errors;
        }
        __atomic_fetch_add(&s_sdl.perf_kms_wait_ns,
                           wait_end_ns - wait_start_ns, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_sdl.perf_kms_scanout_wait_ns, scanout_wait_ns,
                           __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_sdl.perf_kms_copy_ns, copy_ns,
                           __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_sdl.perf_kms_flip_ns, flip_ns,
                           __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_sdl.perf_kms_frames, 1u,
                           __ATOMIC_RELAXED);

        SDL_LockMutex(s_sdl.kms_mutex);
        slot->busy = 0;
        SDL_BroadcastCondition(s_sdl.kms_free);
        SDL_UnlockMutex(s_sdl.kms_mutex);
    }
}

static int kms_start_worker(void)
{
    if (s_sdl.kms_thread) return 0;
    s_sdl.kms_mutex = SDL_CreateMutex();
    s_sdl.kms_work = SDL_CreateCondition();
    s_sdl.kms_free = SDL_CreateCondition();
    if (!s_sdl.kms_mutex || !s_sdl.kms_work || !s_sdl.kms_free) return -1;
    s_sdl.kms_thread = SDL_CreateThread(kms_copy_worker, "taiko-kms", NULL);
    return s_sdl.kms_thread ? 0 : -1;
}

static void kms_stop_worker(void)
{
    if (s_sdl.kms_thread) {
        SDL_LockMutex(s_sdl.kms_mutex);
        s_sdl.kms_stop = 1;
        SDL_BroadcastCondition(s_sdl.kms_work);
        SDL_UnlockMutex(s_sdl.kms_mutex);
        SDL_WaitThread(s_sdl.kms_thread, NULL);
        s_sdl.kms_thread = NULL;
    }
    for (unsigned i = 0; i < SDL_arraysize(s_sdl.kms_slots); ++i) {
        if (s_sdl.kms_slots[i].fence)
            SDL_ReleaseGPUFence(s_sdl.device, s_sdl.kms_slots[i].fence);
        if (s_sdl.kms_slots[i].transfer)
            SDL_ReleaseGPUTransferBuffer(s_sdl.device, s_sdl.kms_slots[i].transfer);
        SDL_zero(s_sdl.kms_slots[i]);
    }
    if (s_sdl.kms_work) { SDL_DestroyCondition(s_sdl.kms_work); s_sdl.kms_work = NULL; }
    if (s_sdl.kms_free) { SDL_DestroyCondition(s_sdl.kms_free); s_sdl.kms_free = NULL; }
    if (s_sdl.kms_mutex) { SDL_DestroyMutex(s_sdl.kms_mutex); s_sdl.kms_mutex = NULL; }
}

static int present_display_kms(SDL_GPUCommandBuffer* commands)
{
    /* A download must cross a completed render fence on V3DV. Independent
     * submissions raced outright, and even putting the copy after the render
     * passes in one SDL command buffer produced partial frames under the
     * heavier Player Entry load. Keep this CPU-visible completion boundary. */
    if (commands) {
        const Uint64 render_start_ns = SDL_GetTicksNS();
        if (submit_kms_render_and_wait(commands) != 0) return -1;
        s_sdl.perf_kms_render_ns += SDL_GetTicksNS() - render_start_ns;
        commands = NULL;
    }

    if (s_sdl.kms_zero_copy) {
        const unsigned current = s_sdl.kms_display_index;
        const void *overlay = s_sdl.perf_overlay &&
            s_fps_overlay.cpu_version != 0 ? s_fps_overlay.pixels : NULL;
        Uint64 copy_ns = 0, flip_ns = 0;
        if (rsx_kms_present_dmabuf(
                current, overlay, FPS_OVERLAY_WIDTH * 4u, 12u, 12u,
                FPS_OVERLAY_WIDTH, FPS_OVERLAY_HEIGHT,
                &copy_ns, &flip_ns) != 0) {
            fprintf(stderr, "[SDL_GPU] KMS dma-buf present failed\n");
            ++s_sdl.errors;
            return -1;
        }
        const unsigned next = (current + 1u) % s_sdl.kms_display_count;
        Uint64 acquire_ns = 0;
        if (rsx_kms_present_acquire_dmabuf(next, &acquire_ns) != 0) {
            fprintf(stderr, "[SDL_GPU] KMS dma-buf acquire failed\n");
            ++s_sdl.errors;
            return -1;
        }
        __atomic_fetch_add(&s_sdl.perf_kms_scanout_wait_ns, acquire_ns,
                           __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_sdl.perf_kms_copy_ns, copy_ns,
                           __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_sdl.perf_kms_flip_ns, flip_ns,
                           __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_sdl.perf_kms_frames, 1u,
                           __ATOMIC_RELAXED);
        s_sdl.kms_display_index = next;
        s_sdl.display = s_sdl.kms_display[next];
        return 0;
    }

    Uint32 width = 0, height = 0;
    SDL_GPUTexture* source = presentation_texture(&width, &height);
    if (!source || !width || !height) return -1;
    const Uint32 pitch = width * 4u;
    const Uint32 bytes = pitch * height;
    if (kms_start_worker() != 0) { ++s_sdl.errors; return -1; }

    /* Wait only when every slot is still in flight; that is the backpressure
     * that keeps the worker at most a couple of frames behind. */
    static unsigned depth;
    if (!depth) {
        const char* text = getenv("TAIKO_KMS_PIPELINE_DEPTH");
        depth = text ? (unsigned)strtoul(text, NULL, 0) : 2u;
        if (depth < 1u) depth = 1u;
        if (depth > SDL_arraysize(s_sdl.kms_slots)) depth = SDL_arraysize(s_sdl.kms_slots);
        fprintf(stderr, "[KMS] pipeline depth %u\n", depth);
    }
    SDL_LockMutex(s_sdl.kms_mutex);
    unsigned index = s_sdl.kms_write % depth;
    while (s_sdl.kms_slots[index].busy || s_sdl.kms_pending_count >= depth)
        SDL_WaitCondition(s_sdl.kms_free, s_sdl.kms_mutex);
    s_sdl.kms_write = (s_sdl.kms_write + 1u) % depth;
    SDL_UnlockMutex(s_sdl.kms_mutex);
    struct kms_slot* slot = &s_sdl.kms_slots[index];

    if (slot->transfer && slot->bytes < bytes) {
        SDL_ReleaseGPUTransferBuffer(s_sdl.device, slot->transfer);
        slot->transfer = NULL;
    }
    if (!slot->transfer) {
        SDL_GPUTransferBufferCreateInfo info;
        SDL_zero(info);
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        info.size = bytes;
        slot->transfer = SDL_CreateGPUTransferBuffer(s_sdl.device, &info);
        slot->bytes = bytes;
        if (!slot->transfer) { ++s_sdl.errors; return -1; }
    }
    slot->source = source;
    slot->width = width;
    slot->height = height;
    slot->pitch = pitch;
    slot->fps_overlay_enabled = s_sdl.perf_overlay &&
        s_fps_overlay.cpu_version != 0;
    if (slot->fps_overlay_enabled)
        memcpy(slot->fps_overlay, s_fps_overlay.pixels,
               sizeof(slot->fps_overlay));

    if (kms_submit_download(slot, commands) != 0) {
        ++s_sdl.errors;
        return -1;
    }

    SDL_LockMutex(s_sdl.kms_mutex);
    slot->busy = 1;
    s_sdl.kms_pending[s_sdl.kms_pending_count++] = (int)index;
    SDL_SignalCondition(s_sdl.kms_work);
    SDL_UnlockMutex(s_sdl.kms_mutex);

    /* Hand the next frame a target nobody is still reading. */
    if (s_sdl.kms_display_count) {
        const unsigned current = s_sdl.kms_display_index;
        s_sdl.kms_display_reader[current] = (int)index;
        const unsigned next = (current + 1u) % s_sdl.kms_display_count;
        const int reader = s_sdl.kms_display_reader[next];
        if (reader >= 0) {
            SDL_LockMutex(s_sdl.kms_mutex);
            while (s_sdl.kms_slots[reader].busy)
                SDL_WaitCondition(s_sdl.kms_free, s_sdl.kms_mutex);
            SDL_UnlockMutex(s_sdl.kms_mutex);
        }
        s_sdl.kms_display_index = next;
        s_sdl.display = s_sdl.kms_display[next];
    }
    return 0;
}
#endif

static int present_display(SDL_GPUCommandBuffer* commands)
{
#ifdef RSX_SDL_KMS_PRESENT
    if (s_sdl.kms_present) return present_display_kms(commands);
#endif
    if (!s_sdl.display || !s_sdl.window) return -1;
    Uint32 source_width, source_height;
    SDL_GPUTexture* source_texture = presentation_texture(
        &source_width, &source_height);
    /* Keep the display render and its presentation blit in one command buffer.
     * Besides avoiding a needless submit boundary, this gives drivers an
     * explicit render-target-to-sampled-texture dependency.  V3DV on the Pi 5
     * otherwise occasionally sampled an incomplete display texture; waiting
     * on a fence fixed that but serialized every frame and cost 10-20 FPS. */
    if (!commands)
        commands = SDL_AcquireGPUCommandBuffer(s_sdl.device);
    if (!commands) { ++s_sdl.errors; return -1; }
    SDL_GPUTexture* swapchain = NULL;
    Uint32 width = 0, height = 0;
    const Uint64 acquire_start_ns = SDL_GetTicksNS();
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, s_sdl.window,
                                               &swapchain, &width, &height)) {
        fprintf(stderr, "[SDL_GPU] swapchain acquisition failed: %s\n", SDL_GetError());
        ++s_sdl.errors;
        submit_commands(commands);
        return -1;
    }
    const Uint64 acquire_end_ns = SDL_GetTicksNS();
    s_sdl.perf_acquire_ns += acquire_end_ns - acquire_start_ns;
    {   /* The swapchain size decides who upscales: a compositor can only
         * scan out a buffer that already matches the output. */
        static Uint32 announced_w, announced_h;
        if (width != announced_w || height != announced_h) {
            announced_w = width; announced_h = height;
            fprintf(stderr, "[SDL_GPU] swapchain %ux%u (source %ux%u)\n",
                    width, height, source_width, source_height);
        }
    }
    if (swapchain && width && height) {
        Uint32 draw_w = width;
        Uint32 draw_h = (Uint32)(((Uint64)width * source_height) / source_width);
        if (draw_h > height) {
            draw_h = height;
            draw_w = (Uint32)(((Uint64)height * source_width) / source_height);
        }
        SDL_GPUBlitInfo blit;
        SDL_zero(blit);
        blit.source.texture = source_texture;
        blit.source.w = source_width;
        blit.source.h = source_height;
        blit.destination.texture = swapchain;
        blit.destination.x = (width - draw_w) / 2;
        blit.destination.y = (height - draw_h) / 2;
        blit.destination.w = draw_w;
        blit.destination.h = draw_h;
        blit.load_op = SDL_GPU_LOADOP_CLEAR;
        blit.clear_color.a = 1.0f;
        blit.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(commands, &blit);
        ++s_sdl.perf_blits;
        draw_overlay(commands, swapchain, width, height,
                     blit.destination.x, blit.destination.y, draw_w, draw_h,
                     SDL_GetGPUSwapchainTextureFormat(s_sdl.device,
                                                      s_sdl.window));
        draw_fps_overlay(commands, swapchain, width, height,
                         blit.destination.x, blit.destination.y, draw_w, draw_h,
                         SDL_GetGPUSwapchainTextureFormat(s_sdl.device,
                                                          s_sdl.window));
    }
    /* V3DV presents a swapchain image whose GPU work is not finished: the
     * frame shows a diagonal band of completed 128x64 tiles -- V3D's tile size
     * and supertile order -- while the rest keeps the previous frame or the
     * blit's clear. Fencing the presentation submission is what removes it.
     *
     * Measured on a Pi 5 over 200 s samples of the compositor output, scoring
     * each frame's edge energy on the tile grid against off-grid: unfenced
     * reproduces the artifact repeatedly, and merely bounding run-ahead to one
     * frame (waiting on the *previous* present) does not help -- only the full
     * wait does. TAIKO_GPU_UNFENCED_PRESENT opts out on drivers that behave. */
    const Uint64 blit_end_ns = SDL_GetTicksNS();
    s_sdl.perf_blit_ns += blit_end_ns - acquire_end_ns;
    const int result = s_sdl.fence_present ? submit_commands_and_wait(commands)
                                           : submit_commands(commands);
    s_sdl.perf_fence_ns += SDL_GetTicksNS() - blit_end_ns;
    return result;
}

static void update_window_title(void)
{
    Uint64 now = SDL_GetTicksNS();
    if (!s_sdl.fps_window_start_ns) {
        s_sdl.fps_window_start_ns = now;
        return;
    }
    Uint64 elapsed = now - s_sdl.fps_window_start_ns;
    if (elapsed < SDL_NS_PER_SECOND) return;
    double fps = (double)s_sdl.fps_window_frames *
                 (double)SDL_NS_PER_SECOND / (double)elapsed;
    char title[256];
    snprintf(title, sizeof(title),
             "%s | FPS: %.2f | draws: %u | GPU errors: %u",
             s_sdl.base_title, fps, s_sdl.last_batch_draws, s_sdl.errors);
    if (s_sdl.window) SDL_SetWindowTitle(s_sdl.window, title);
    const Uint64 perf_submits = __atomic_exchange_n(
        &s_sdl.perf_submits, 0, __ATOMIC_RELAXED);
    const Uint64 kms_render_ns = __atomic_exchange_n(
        &s_sdl.perf_kms_render_ns, 0, __ATOMIC_RELAXED);
    const Uint64 kms_issue_ns = __atomic_exchange_n(
        &s_sdl.perf_kms_download_ns, 0, __ATOMIC_RELAXED);
    const Uint64 kms_frames = __atomic_exchange_n(
        &s_sdl.perf_kms_frames, 0, __ATOMIC_RELAXED);
    const Uint64 kms_wait_ns = __atomic_exchange_n(
        &s_sdl.perf_kms_wait_ns, 0, __ATOMIC_RELAXED);
    const Uint64 kms_scanout_wait_ns = __atomic_exchange_n(
        &s_sdl.perf_kms_scanout_wait_ns, 0, __ATOMIC_RELAXED);
    const Uint64 kms_copy_ns = __atomic_exchange_n(
        &s_sdl.perf_kms_copy_ns, 0, __ATOMIC_RELAXED);
    const Uint64 kms_flip_ns = __atomic_exchange_n(
        &s_sdl.perf_kms_flip_ns, 0, __ATOMIC_RELAXED);
    fps_overlay_set_value(fps);
    if (s_sdl.fps_log)
        fprintf(stderr,
                "[RSXFPS] %.2f fps draws=%u errors=%u "
                "cpu_ms(prep=%.2f const=%.2f vert=%.2f render=%.2f present=%.2f"
                "[acq=%.2f blit=%.2f fence=%.2f]) passes=%.1f "
                "const_kib=%.0f vert_kib=%.0f "
                "gpu(blits=%.1f copies=%.1f submits=%.1f) "
                "kms(render_wait=%.2f issue=%.2f fence_wait=%.2f "
                "scanout_wait=%.2f copy=%.2f flip=%.2f frames=%llu)\n",
                fps, s_sdl.last_batch_draws, s_sdl.errors,
                s_sdl.perf_batches ? (double)s_sdl.perf_prepare_ns /
                    (double)s_sdl.perf_batches / 1000000.0 : 0.0,
                s_sdl.perf_batches ? (double)s_sdl.perf_constants_ns /
                    (double)s_sdl.perf_batches / 1000000.0 : 0.0,
                s_sdl.perf_batches ? (double)s_sdl.perf_vertices_ns /
                    (double)s_sdl.perf_batches / 1000000.0 : 0.0,
                s_sdl.perf_batches ? (double)s_sdl.perf_render_ns /
                    (double)s_sdl.perf_batches / 1000000.0 : 0.0,
                s_sdl.perf_batches ? (double)s_sdl.perf_present_ns /
                    (double)s_sdl.perf_batches / 1000000.0 : 0.0,
                s_sdl.perf_batches ? (double)s_sdl.perf_acquire_ns /
                    (double)s_sdl.perf_batches / 1000000.0 : 0.0,
                s_sdl.perf_batches ? (double)s_sdl.perf_blit_ns /
                    (double)s_sdl.perf_batches / 1000000.0 : 0.0,
                s_sdl.perf_batches ? (double)s_sdl.perf_fence_ns /
                    (double)s_sdl.perf_batches / 1000000.0 : 0.0,
                s_sdl.perf_batches ? (double)s_sdl.perf_render_passes /
                    (double)s_sdl.perf_batches : 0.0,
                s_sdl.perf_batches ? (double)s_sdl.perf_constant_bytes /
                    (double)s_sdl.perf_batches / 1024.0 : 0.0,
                s_sdl.perf_batches ? (double)s_sdl.perf_vertex_bytes /
                    (double)s_sdl.perf_batches / 1024.0 : 0.0,
                s_sdl.perf_batches ? (double)s_sdl.perf_blits /
                    (double)s_sdl.perf_batches : 0.0,
                s_sdl.perf_batches ? (double)s_sdl.perf_copy_passes /
                    (double)s_sdl.perf_batches : 0.0,
                s_sdl.perf_batches ? (double)perf_submits /
                    (double)s_sdl.perf_batches : 0.0,
                s_sdl.perf_batches ? (double)kms_render_ns /
                    (double)s_sdl.perf_batches / 1000000.0 : 0.0,
                s_sdl.perf_batches ? (double)kms_issue_ns /
                    (double)s_sdl.perf_batches / 1000000.0 : 0.0,
                kms_frames ? (double)kms_wait_ns /
                    (double)kms_frames / 1000000.0 : 0.0,
                kms_frames ? (double)kms_scanout_wait_ns /
                    (double)kms_frames / 1000000.0 : 0.0,
                kms_frames ? (double)kms_copy_ns /
                    (double)kms_frames / 1000000.0 : 0.0,
                kms_frames ? (double)kms_flip_ns /
                    (double)kms_frames / 1000000.0 : 0.0,
                (unsigned long long)kms_frames);
    s_sdl.fps_window_start_ns = now;
    s_sdl.fps_window_frames = 0;
    s_sdl.perf_batches = 0;
    s_sdl.perf_prepare_ns = 0;
    s_sdl.perf_constants_ns = 0;
    s_sdl.perf_vertices_ns = 0;
    s_sdl.perf_render_ns = 0;
    s_sdl.perf_present_ns = 0;
    s_sdl.perf_acquire_ns = 0;
    s_sdl.perf_blit_ns = 0;
    s_sdl.perf_fence_ns = 0;
    s_sdl.perf_render_passes = 0;
    s_sdl.perf_blits = 0;
    s_sdl.perf_copy_passes = 0;
    s_sdl.perf_vertex_bytes = 0;
    s_sdl.perf_constant_bytes = 0;
}

static void upload_surface_init(const rsx_surface_init* init)
{
    SDL_GPUTexture* texture = get_surface(&init->surface);
    const rsx_owned_blob* blob = init->surface.is_depth
        ? &init->depth_stencil_data : &init->color_data;
    if (!texture || !blob->data || !blob->size || blob->size > UINT32_MAX) return;
    SDL_GPUTextureFormat format = portable_format(init->surface.format);
    Uint32 block_size = SDL_GPUTextureFormatTexelBlockSize(format);
    if (!block_size) { ++s_sdl.errors; return; }
    SDL_GPUTransferBufferCreateInfo transfer_info;
    SDL_zero(transfer_info);
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = (u32)blob->size;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(
        s_sdl.device, &transfer_info);
    if (!transfer) { ++s_sdl.errors; return; }
    void* mapped = SDL_MapGPUTransferBuffer(s_sdl.device, transfer, false);
    if (!mapped) { SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer); ++s_sdl.errors; return; }
    memcpy(mapped, blob->data, (size_t)blob->size);
    SDL_UnmapGPUTransferBuffer(s_sdl.device, transfer);
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(s_sdl.device);
    SDL_GPUCopyPass* copy = commands ? SDL_BeginGPUCopyPass(commands) : NULL;
    if (copy) ++s_sdl.perf_copy_passes;
    if (!copy) {
        if (commands) SDL_CancelGPUCommandBuffer(commands);
        SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer);
        ++s_sdl.errors; return;
    }
    SDL_GPUTextureTransferInfo source;
    SDL_zero(source);
    source.transfer_buffer = transfer;
    source.pixels_per_row = init->surface.pitch
        ? init->surface.pitch / block_size : init->surface.width;
    source.rows_per_layer = init->surface.height;
    SDL_GPUTextureRegion destination;
    SDL_zero(destination);
    destination.texture = texture;
    destination.w = init->surface.width;
    destination.h = init->surface.height;
    destination.d = 1;
    SDL_UploadToGPUTexture(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    submit_commands(commands);
    SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer);
}

static void execute_batch(const rsx_render_batch* batch, Uint64 enqueue_ns)
{
    Uint64 perf_start = SDL_GetTicksNS();
    const Uint64 execute_start_ns = perf_start;
    Uint64 perf_mark;
    const int resource_trace = getenv("RSX_RESOURCE_TRACE") != NULL;
    Uint64 surface_init_ns = 0, surface_ns = 0;
    Uint64 pipeline_ns = 0, texture_ns = 0, sampler_ns = 0;
    const unsigned old_surfaces = s_sdl.surface_count;
    const unsigned old_shaders = s_sdl.shader_count;
    const unsigned old_pipelines = s_sdl.pipeline_count;
    const unsigned old_textures = s_sdl.texture_count;
    const unsigned old_samplers = s_sdl.sampler_count;
    unsigned render_passes = 0;
    SDL_GPUBuffer* vertex_constants = NULL;
    SDL_GPUBuffer* vertices = NULL;
    u32* vertex_constant_offsets = NULL;
    u32* vertex_offsets = NULL;
    u64 vertex_bytes = 0;
    unsigned draw_count = 0;
    for (unsigned i = 0; i < batch->operation_count; ++i)
        if (batch->operations[i].type == RSX_RENDER_OP_DRAW) {
            const rsx_render_op* op = &batch->operations[i];
            if (skip_character_mesh_outline(op)) continue;
            ++draw_count;
            u64 aligned = (vertex_bytes + 15u) & ~(u64)15u;
            if (aligned > UINT32_MAX ||
                op->data.draw.vertex_data.size > UINT32_MAX - aligned) {
                vertex_bytes = UINT32_MAX + (u64)1;
                break;
            }
            vertex_bytes = aligned + op->data.draw.vertex_data.size;
        }
    s_sdl.last_batch_draws = draw_count;
    s_sdl.perf_vertex_bytes += vertex_bytes <= UINT32_MAX ? vertex_bytes : 0;

    /* Resource creation and texture uploads may submit copy command buffers.
     * Finish all of those before acquiring the shared render command buffer. */
    Uint64 resource_mark = resource_trace ? SDL_GetTicksNS() : 0;
    for (unsigned i = 0; i < batch->surface_init_count; ++i)
        upload_surface_init(&batch->surface_inits[i]);
    if (resource_trace) {
        Uint64 now = SDL_GetTicksNS();
        surface_init_ns += now - resource_mark;
        resource_mark = now;
    }
    for (unsigned i = 0; i < batch->operation_count; ++i) {
        const rsx_render_op* op = &batch->operations[i];
        for (unsigned target = 0; target < op->color_count && target < 4; ++target)
            get_surface(&op->color[target]);
        get_surface(&op->depth);
        if (resource_trace) {
            Uint64 now = SDL_GetTicksNS();
            surface_ns += now - resource_mark;
            resource_mark = now;
        }
        if (op->type != RSX_RENDER_OP_DRAW) continue;
        if (skip_character_mesh_outline(op)) continue;
        get_pipeline(op);
        if (resource_trace) {
            Uint64 now = SDL_GetTicksNS();
            pipeline_ns += now - resource_mark;
            resource_mark = now;
        }
        shader_entry* fragment = get_shader(
            op, SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
        unsigned sampler_count = fragment
            ? fragment->sampler_unit_count : 0;
        if (sampler_count > 4) sampler_count = 4;
        for (unsigned slot = 0; slot < sampler_count; ++slot) {
            unsigned unit = fragment->sampler_units[slot];
            if (unit < op->data.draw.texture_count) {
                get_sample_texture(&op->data.draw.textures[unit]);
                if (resource_trace) {
                    Uint64 now = SDL_GetTicksNS();
                    texture_ns += now - resource_mark;
                    resource_mark = now;
                }
                get_sampler(op->data.draw.textures[unit].address);
                if (resource_trace) {
                    Uint64 now = SDL_GetTicksNS();
                    sampler_ns += now - resource_mark;
                    resource_mark = now;
                }
            }
        }
    }
    perf_mark = SDL_GetTicksNS();
    s_sdl.perf_prepare_ns += perf_mark - perf_start;
    if (resource_trace &&
        (getenv("RSX_RESOURCE_TRACE_ALL") ||
         perf_mark - perf_start >= UINT64_C(5000000)))
        fprintf(stderr,
                "[SDL_GPU-SLOWPREP] serial=%llu total=%.2fms "
                "surface_init=%.2f surface=%.2f pipeline=%.2f texture=%.2f "
                "sampler=%.2f new(surface=%u shader=%u pipeline=%u texture=%u sampler=%u)\n",
                (unsigned long long)batch->serial,
                (double)(perf_mark - perf_start) / 1000000.0,
                (double)surface_init_ns / 1000000.0,
                (double)surface_ns / 1000000.0,
                (double)pipeline_ns / 1000000.0,
                (double)texture_ns / 1000000.0,
                (double)sampler_ns / 1000000.0,
                s_sdl.surface_count - old_surfaces,
                s_sdl.shader_count - old_shaders,
                s_sdl.pipeline_count - old_pipelines,
                s_sdl.texture_count - old_textures,
                s_sdl.sampler_count - old_samplers);
    perf_start = perf_mark;

    /* Pipelines above have populated each vertex shader's static guest-slot
     * map. Give every draw only the float4 values that its shader can read;
     * address-indexed programs expose the identity map and retain all 514. */
    u64 constant_bytes = 0;
    if (batch->operation_count) {
        vertex_constant_offsets = (u32*)malloc(
            (size_t)batch->operation_count * sizeof(*vertex_constant_offsets));
        if (vertex_constant_offsets) {
            memset(vertex_constant_offsets, 0xff,
                   (size_t)batch->operation_count *
                       sizeof(*vertex_constant_offsets));
            for (unsigned i = 0; i < batch->operation_count; ++i) {
                const rsx_render_op* op = &batch->operations[i];
                if (op->type != RSX_RENDER_OP_DRAW ||
                    skip_character_mesh_outline(op) ||
                    !op->data.draw.vertex_shader.size)
                    continue;
                shader_entry* shader = get_shader(
                    op, SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
                if (!shader || !shader->vertex_constant_count) continue;
                const u64 bytes =
                    (u64)shader->vertex_constant_count * 4u * sizeof(float);
                if (constant_bytes > UINT32_MAX ||
                    bytes > UINT32_MAX - constant_bytes) {
                    constant_bytes = UINT32_MAX + (u64)1;
                    break;
                }
                vertex_constant_offsets[i] = (u32)constant_bytes;
                constant_bytes += bytes;
            }
        }
    }
    s_sdl.perf_constant_bytes +=
        constant_bytes <= UINT32_MAX ? constant_bytes : 0;

    /* Record dynamic buffer uploads, display rendering, and presentation into
     * one command buffer. The previous per-buffer submissions cost several
     * milliseconds each on V3DV and made the heavy attract frames miss 60 Hz. */
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(s_sdl.device);
    if (!commands) {
        fprintf(stderr, "[SDL_GPU] batch command acquisition failed: %s\n",
                SDL_GetError());
        ++s_sdl.errors;
        goto done;
    }
    const int fps_overlay_uploaded = upload_fps_overlay(commands);

    if (vertex_constant_offsets && constant_bytes &&
        constant_bytes <= UINT32_MAX) {
        u8* packed = (u8*)map_dynamic_upload(
            &s_sdl.vertex_constants_upload, constant_bytes,
            SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
        if (packed) {
            for (unsigned i = 0; i < batch->operation_count; ++i) {
                const rsx_render_op* op = &batch->operations[i];
                if (op->type != RSX_RENDER_OP_DRAW ||
                    skip_character_mesh_outline(op) ||
                    vertex_constant_offsets[i] == UINT32_MAX)
                    continue;
                shader_entry* shader = get_shader(
                    op, SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
                if (!shader) continue;
                float* destination = (float*)(
                    packed + vertex_constant_offsets[i]);
                for (unsigned dense = 0;
                     dense < shader->vertex_constant_count; ++dense) {
                    const unsigned guest =
                        shader->vertex_constant_units[dense];
                    float value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    const u64 source_offset = (u64)guest * sizeof(value);
                    if (source_offset + sizeof(value) <=
                        op->data.draw.vertex_constants.size)
                        memcpy(value,
                               op->data.draw.vertex_constants.data +
                                   source_offset,
                               sizeof(value));
                    /* RSXB v2's VP epilogue constants are defined in clip
                     * space. Canonicalize x/y at the consumer boundary so
                     * captures from before the recorder fix remain valid. */
                    if (guest == 512) {
                        value[0] = 1.0f; value[1] = 1.0f; value[3] = 1.0f;
                    } else if (guest == 513) {
                        value[0] = 0.0f; value[1] = 0.0f; value[3] = 0.0f;
                    }
                    memcpy(destination + dense * 4u, value, sizeof(value));
                }
            }
            vertex_constants = commit_dynamic_upload(
                commands, &s_sdl.vertex_constants_upload, constant_bytes);
        }
    }
    perf_mark = SDL_GetTicksNS();
    s_sdl.perf_constants_ns += perf_mark - perf_start;
    perf_start = perf_mark;

    if (batch->operation_count && vertex_bytes && vertex_bytes <= UINT32_MAX) {
        vertex_offsets = (u32*)malloc(
            (size_t)batch->operation_count * sizeof(*vertex_offsets));
        u8* packed = (u8*)map_dynamic_upload(
            &s_sdl.vertices_upload, vertex_bytes,
            SDL_GPU_BUFFERUSAGE_VERTEX);
        if (vertex_offsets)
            memset(vertex_offsets, 0xff,
                   (size_t)batch->operation_count * sizeof(*vertex_offsets));
        if (vertex_offsets && packed) {
            u64 offset = 0;
            for (unsigned i = 0; i < batch->operation_count; ++i) {
                const rsx_render_op* op = &batch->operations[i];
                if (op->type != RSX_RENDER_OP_DRAW ||
                    skip_character_mesh_outline(op) ||
                    !op->data.draw.vertex_data.size)
                    continue;
                offset = (offset + 15u) & ~(u64)15u;
                vertex_offsets[i] = (u32)offset;
                memcpy(packed + offset, op->data.draw.vertex_data.data,
                       (size_t)op->data.draw.vertex_data.size);
                offset += op->data.draw.vertex_data.size;
            }
            vertices = commit_dynamic_upload(
                commands, &s_sdl.vertices_upload, vertex_bytes);
        } else if (packed) {
            SDL_UnmapGPUTransferBuffer(s_sdl.device,
                                       s_sdl.vertices_upload.transfer);
        }
    }
    perf_mark = SDL_GetTicksNS();
    s_sdl.perf_vertices_ns += perf_mark - perf_start;
    perf_start = perf_mark;

    /* V3DV has produced stale vertex/constant data when a command buffer both
     * uploads and consumes these buffers. Splitting at the queue boundary
     * preserves asynchronous execution while making the transfer-to-graphics
     * dependency explicit. Other drivers retain the single-submit path. */
    const int upload_idle = getenv("TAIKO_GPU_UPLOAD_IDLE") != NULL;
    const int upload_fence_wait =
        getenv("TAIKO_GPU_UPLOAD_FENCE_WAIT") != NULL;
    if ((getenv("TAIKO_GPU_SEPARATE_UPLOAD_SUBMIT") || upload_idle ||
         upload_fence_wait) &&
        (vertex_constants || vertices || fps_overlay_uploaded)) {
        if ((upload_fence_wait && !upload_idle
                 ? submit_commands_and_wait(commands)
                 : submit_commands(commands)) != 0) {
            commands = NULL;
            goto done;
        }
        if (upload_fence_wait && !upload_idle) {
            static int announced_upload_fence;
            if (!announced_upload_fence++)
                fprintf(stderr,
                        "[SDL_GPU] dynamic upload fence completes before rendering\n");
        }
        if (upload_idle) {
            static int announced_upload_idle;
            if (!announced_upload_idle++)
                fprintf(stderr,
                        "[SDL_GPU] dynamic uploads complete before rendering\n");
            if (!SDL_WaitForGPUIdle(s_sdl.device)) {
                fprintf(stderr, "[SDL_GPU] upload GPU idle wait failed: %s\n",
                        SDL_GetError());
                ++s_sdl.errors;
                commands = NULL;
                goto done;
            }
        }
        commands = SDL_AcquireGPUCommandBuffer(s_sdl.device);
        if (!commands) {
            fprintf(stderr, "[SDL_GPU] render command acquisition failed: %s\n",
                    SDL_GetError());
            ++s_sdl.errors;
            goto done;
        }
    }

    SDL_GPURenderPass* active_pass = NULL;
    const rsx_render_op* active_attachments = NULL;
    const rsx_render_op* pending_clear = NULL;
    for (unsigned i = 0; i < batch->operation_count; ++i) {
        const rsx_render_op* op = &batch->operations[i];
        if (op->type == RSX_RENDER_OP_CLEAR) {
            if (active_pass) {
                SDL_EndGPURenderPass(active_pass);
                active_pass = NULL;
                active_attachments = NULL;
            }
            /* Hold the clear back: if draws to the same attachments follow, it
             * becomes their pass's load op instead of a pass of its own. */
            if (pending_clear) {
                execute_clear(commands, pending_clear);
                ++render_passes;
            }
            pending_clear = op;
        } else if (op->type == RSX_RENDER_OP_DRAW) {
            if (skip_character_mesh_outline(op)) continue;
            shader_entry* vertex_shader = get_shader(
                op, SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
            int valid = get_pipeline(op) && vertex_shader && vertices &&
                        vertex_offsets && vertex_offsets[i] != UINT32_MAX &&
                        (!vertex_shader->resources.num_storage_buffers ||
                         (vertex_constants && vertex_constant_offsets &&
                          vertex_constant_offsets[i] != UINT32_MAX));
            if (!valid) {
                if (active_pass) {
                    SDL_EndGPURenderPass(active_pass);
                    active_pass = NULL;
                    active_attachments = NULL;
                }
                if (pending_clear) {
                    execute_clear(commands, pending_clear);
                    ++render_passes;
                    pending_clear = NULL;
                }
                execute_diagnostic_fallback(commands, op);
                ++render_passes;
                continue;
            }
            if (!active_pass ||
                !same_draw_attachments(active_attachments, op)) {
                if (active_pass) SDL_EndGPURenderPass(active_pass);
                const rsx_render_op* fold = NULL;
                if (pending_clear) {
                    static int no_merge = -1;
                    if (no_merge < 0)
                        no_merge = getenv("TAIKO_GPU_NO_CLEAR_MERGE") ? 1 : 0;
                    if (!no_merge && same_draw_attachments(pending_clear, op)) {
                        fold = pending_clear;
                    } else {
                        execute_clear(commands, pending_clear);
                        ++render_passes;
                    }
                    pending_clear = NULL;
                }
                active_pass = begin_draw_pass(commands, op, fold);
                active_attachments = active_pass ? op : NULL;
                if (active_pass) ++render_passes;
            }
            execute_draw(commands, active_pass, op, vertices,
                         vertex_offsets ? vertex_offsets[i] : UINT32_MAX,
                         vertex_constants,
                         vertex_constant_offsets &&
                             vertex_constant_offsets[i] != UINT32_MAX
                             ? vertex_constant_offsets[i] /
                                   (4u * (u32)sizeof(float))
                             : 0u);
        }
    }
    if (active_pass) SDL_EndGPURenderPass(active_pass);
    if (pending_clear) {
        execute_clear(commands, pending_clear);
        ++render_passes;
        pending_clear = NULL;
    }
    if (s_sdl.kms_present) {
        /* The KMS path owns a separate display-target ring tied to its readback
         * slots. The generic swapchain presentation ring must stay out of it. */
        s_sdl.presentation_override = NULL;
        const Uint64 render_wait_start_ns = SDL_GetTicksNS();
        submit_kms_render_and_wait(commands);
        s_sdl.perf_kms_render_ns += SDL_GetTicksNS() - render_wait_start_ns;
        commands = NULL;
    } else if (getenv("TAIKO_GPU_SERIAL_PRESENT")) {
        const int pipelined = getenv("TAIKO_GPU_PIPELINED_PRESENT") != NULL;
        static int announced_mode;
        if (!announced_mode++) {
            if (pipelined)
                fprintf(stderr,
                        "[SDL_GPU] pipelined presentation with %u display targets\n",
                        SDL_RSX_PRESENT_SLOTS);
            else
                fprintf(stderr,
                        "[SDL_GPU] serializing display render before present\n");
        }
        if (pipelined && !s_sdl.presentation_pipeline_disabled) {
            const int result = submit_pipelined_display(commands);
            if (result < 0) {
                fprintf(stderr,
                        "[SDL_GPU] pipelined presentation disabled; "
                        "using serialized fallback\n");
                s_sdl.presentation_override = NULL;
                submit_commands_and_wait(commands);
            } else if (result > 0) {
                fprintf(stderr,
                        "[SDL_GPU] pipelined presentation disabled after "
                        "submission; retaining last complete frame\n");
            }
        } else {
            s_sdl.presentation_override = NULL;
            submit_commands_and_wait(commands);
        }
        commands = NULL;
    }
done:
    perf_mark = SDL_GetTicksNS();
    s_sdl.perf_render_ns += perf_mark - perf_start;
    s_sdl.perf_render_passes += render_passes;
    perf_start = perf_mark;
    free(vertex_constant_offsets);
    free(vertex_offsets);
    const Uint64 present_start_ns = perf_start;
    if (present_display(commands) == 0)
        ++s_sdl.fps_window_frames;
    perf_mark = SDL_GetTicksNS();
    s_sdl.perf_present_ns += perf_mark - perf_start;
    trace_frame_pacing(enqueue_ns, execute_start_ns, present_start_ns, perf_mark);
    ++s_sdl.perf_batches;
    update_window_title();
}

static unsigned queue_depth(void)
{
    return s_sdl.queue_limit ? s_sdl.queue_limit : 2u;
}

static int consumer_submit(void* userdata, const rsx_render_batch* batch)
{
    (void)userdata;
    if (!batch || !s_sdl.queue_mutex) return -1;
    SDL_LockMutex(s_sdl.queue_mutex);
    while (s_sdl.queue_count >= queue_depth() && !s_sdl.stopping)
        SDL_WaitCondition(s_sdl.queue_space, s_sdl.queue_mutex);
    if (s_sdl.stopping) {
        SDL_UnlockMutex(s_sdl.queue_mutex);
        return -1;
    }
    queued_batch* slot = &s_sdl.queue[s_sdl.queue_write];
    rsx_render_batch_init(&slot->batch, batch->serial);
    if (rsx_render_batch_clone_shared(&slot->batch, batch) != 0) {
        SDL_UnlockMutex(s_sdl.queue_mutex);
        return -1;
    }
    slot->occupied = 1;
    slot->enqueue_ns = SDL_GetTicksNS();
    s_sdl.queue_write = (s_sdl.queue_write + 1) % SDL_RSX_QUEUE_DEPTH;
    ++s_sdl.queue_count;
    SDL_UnlockMutex(s_sdl.queue_mutex);
    SDL_Event event;
    SDL_zero(event);
    event.type = s_sdl.wake_event;
    SDL_PushEvent(&event);
    return 0;
}

static int consumer_snapshot(void* userdata, rsx_render_batch* seed)
{
    (void)userdata;
    if (!seed || !s_sdl.queue_mutex || !s_sdl.snapshot_done) return -1;
    SDL_LockMutex(s_sdl.queue_mutex);
    while (s_sdl.snapshot_pending && !s_sdl.stopping)
        SDL_WaitCondition(s_sdl.snapshot_done, s_sdl.queue_mutex);
    if (s_sdl.stopping) {
        SDL_UnlockMutex(s_sdl.queue_mutex);
        return -1;
    }
    s_sdl.snapshot_target = seed;
    s_sdl.snapshot_result = -1;
    s_sdl.snapshot_pending = 1;
    SDL_UnlockMutex(s_sdl.queue_mutex);
    SDL_Event event;
    SDL_zero(event);
    event.type = s_sdl.wake_event;
    SDL_PushEvent(&event);
    SDL_LockMutex(s_sdl.queue_mutex);
    while (s_sdl.snapshot_pending && !s_sdl.stopping)
        SDL_WaitCondition(s_sdl.snapshot_done, s_sdl.queue_mutex);
    int result = s_sdl.stopping ? -1 : s_sdl.snapshot_result;
    SDL_UnlockMutex(s_sdl.queue_mutex);
    return result;
}

static const char* consumer_driver(void* userdata)
{
    (void)userdata;
    return s_sdl.device ? SDL_GetGPUDeviceDriver(s_sdl.device) : "sdl_gpu";
}

static const rsx_render_backend_ops s_consumer = {
    NULL, consumer_submit, consumer_snapshot, NULL, NULL, consumer_driver
};

static unsigned keyboard_action(SDL_Scancode code)
{
    switch (code) {
    case SDL_SCANCODE_D: return TAIKO_HIT_SL;
    case SDL_SCANCODE_F: return TAIKO_HIT_CL;
    case SDL_SCANCODE_J: return TAIKO_HIT_CR;
    case SDL_SCANCODE_K: return TAIKO_HIT_SR;
    case SDL_SCANCODE_RETURN: return TAIKO_ENTER;
    case SDL_SCANCODE_F1: return TAIKO_TEST;
    case SDL_SCANCODE_F2: return TAIKO_SERVICE;
    case SDL_SCANCODE_C: return TAIKO_COIN;
    case SDL_SCANCODE_UP: return TAIKO_UP;
    case SDL_SCANCODE_DOWN: return TAIKO_DOWN;
    default: return 0;
    }
}

#ifndef RSX_SDL_REPLAY_STANDALONE
static void toggle_performance_overlay(void)
{
    s_sdl.perf_overlay = !s_sdl.perf_overlay;
    fprintf(stderr, "[SDL_GPU] performance overlay %s\n",
            s_sdl.perf_overlay ? "enabled" : "disabled");
}

static void arm_capture_hotkey(void)
{
    /* Hotkey-specific variables do not auto-arm the recorder during startup.
     * Fall back to the auto-capture names for compatibility with existing
     * desktop launch scripts. */
    const char* path = getenv("RSX_BATCH_CAPTURE_HOTKEY");
    const char* frames = getenv("RSX_BATCH_CAPTURE_HOTKEY_FRAMES");
    if (!path || !path[0]) path = getenv("RSX_BATCH_CAPTURE");
    if (!frames || !frames[0]) frames = getenv("RSX_BATCH_CAPTURE_FRAMES");
    rsx_recorder_arm_capture(path && path[0] ? path : "rsx_capture.rsxb",
                             frames ? (u32)strtoul(frames, NULL, 0) : 1u);
    fprintf(stderr, "[SDL_GPU] F10 armed portable RSX capture\n");
}
#endif

#if defined(__linux__)
#define EVDEV_BITS_PER_WORD (sizeof(unsigned long) * 8u)
#define EVDEV_KEY_WORDS ((KEY_MAX + EVDEV_BITS_PER_WORD) / EVDEV_BITS_PER_WORD)

static int evdev_bit(const unsigned long* bits, unsigned code)
{
    return (bits[code / EVDEV_BITS_PER_WORD] &
            (1ul << (code % EVDEV_BITS_PER_WORD))) != 0;
}

static SDL_Scancode evdev_scancode(unsigned code)
{
    switch (code) {
    case KEY_D: return SDL_SCANCODE_D;
    case KEY_F: return SDL_SCANCODE_F;
    case KEY_J: return SDL_SCANCODE_J;
    case KEY_K: return SDL_SCANCODE_K;
    case KEY_ENTER: case KEY_KPENTER: return SDL_SCANCODE_RETURN;
    case KEY_F1: return SDL_SCANCODE_F1;
    case KEY_F2: return SDL_SCANCODE_F2;
    case KEY_F9: return SDL_SCANCODE_F9;
    case KEY_F10: return SDL_SCANCODE_F10;
    case KEY_C: return SDL_SCANCODE_C;
    case KEY_UP: return SDL_SCANCODE_UP;
    case KEY_DOWN: return SDL_SCANCODE_DOWN;
#ifdef RSX_SDL_REPLAY_STANDALONE
    case KEY_LEFT: return SDL_SCANCODE_LEFT;
    case KEY_RIGHT: return SDL_SCANCODE_RIGHT;
    case KEY_Y: return SDL_SCANCODE_Y;
    case KEY_N: return SDL_SCANCODE_N;
    case KEY_ESC: return SDL_SCANCODE_ESCAPE;
#endif
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

static void evdev_close_keyboards(void)
{
    for (unsigned i = 0; i < s_sdl.evdev_keyboard_count; ++i)
        if (s_sdl.evdev_keyboards[i] >= 0) close(s_sdl.evdev_keyboards[i]);
    s_sdl.evdev_keyboard_count = 0;
}

/* Additive: keep what is already open and pick up anything new. This used to
 * return as soon as one keyboard was open, and only rescanned when none were,
 * so whichever device enumerated first won for the rest of the session and a
 * drum plugged in later was invisible. Most drums present as generic keyboards,
 * so there is no useful way to prefer one -- open them all. */
static void evdev_open_keyboards(void)
{
    if (!s_sdl.kms_present) return;
    for (unsigned index = 0; index < 64 &&
             s_sdl.evdev_keyboard_count < SDL_arraysize(s_sdl.evdev_keyboards);
         ++index) {
        int already = 0;
        for (unsigned i = 0; i < s_sdl.evdev_keyboard_count; ++i)
            if (s_sdl.evdev_indices[i] == (int)index) { already = 1; break; }
        if (already) continue;

        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%u", index);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long keys[EVDEV_KEY_WORDS];
        memset(keys, 0, sizeof(keys));
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 ||
            !evdev_bit(keys, KEY_ENTER) ||
            !(evdev_bit(keys, KEY_C) || evdev_bit(keys, KEY_D))) {
            close(fd);
            continue;
        }
        char name[128] = "keyboard";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        s_sdl.evdev_indices[s_sdl.evdev_keyboard_count] = (int)index;
        s_sdl.evdev_keyboards[s_sdl.evdev_keyboard_count++] = fd;
        fprintf(stderr, "[SDL_INPUT] evdev keyboard: %s (%s)\n", name, path);
    }
    if (!s_sdl.evdev_keyboard_count) {
        static int reported;
        if (!reported++)
            fprintf(stderr,
                    "[SDL_INPUT] no readable evdev keyboard; "
                    "check the service input group\n");
    }
}

static void evdev_handle_key(unsigned code, int down)
{
    const SDL_Scancode scancode = evdev_scancode(code);
    if (scancode == SDL_SCANCODE_UNKNOWN) return;
#ifdef RSX_SDL_REPLAY_STANDALONE
    if (down && g_rsx_replay_key_hook) {
        g_rsx_replay_key_hook((int)scancode);
        return;
    }
#else
    if (down && scancode == SDL_SCANCODE_F9) {
        toggle_performance_overlay();
        return;
    }
    if (down && scancode == SDL_SCANCODE_F10) {
        arm_capture_hotkey();
        return;
    }
#endif
    const unsigned action = keyboard_action(scancode);
    if (!action) return;
    if (down)
        taiko_host_input_press(0, action, SDL_GetTicksNS());
    else
        taiko_host_input_release(0, action);
}

static void evdev_poll_keyboards(void)
{
    if (!s_sdl.kms_present) return;
    const Uint64 now = SDL_GetTicksNS();
    if (now >= s_sdl.evdev_rescan_ns) {
        /* Scanning means up to 64 open()+ioctl()+close() calls, and this runs on
         * the render thread -- doing that every second is a visible hitch on the
         * Pi. /dev/input's mtime changes when a device appears or disappears, so
         * one stat() per second replaces the scan in the common case. */
        struct stat st;
        const long mtime = (stat("/dev/input", &st) == 0)
            ? (long)st.st_mtime : 0;
        if (mtime != s_sdl.evdev_dir_mtime || !s_sdl.evdev_keyboard_count) {
            s_sdl.evdev_dir_mtime = mtime;
            evdev_open_keyboards();
        }
        s_sdl.evdev_rescan_ns = now + UINT64_C(1000000000);
    }
    for (unsigned i = 0; i < s_sdl.evdev_keyboard_count; ++i) {
        struct input_event events[32];
        ssize_t bytes;
        while ((bytes = read(s_sdl.evdev_keyboards[i], events,
                             sizeof(events))) > 0) {
            const size_t count = (size_t)bytes / sizeof(events[0]);
            for (size_t event = 0; event < count; ++event)
                if (events[event].type == EV_KEY && events[event].value != 2)
                    evdev_handle_key(events[event].code,
                                     events[event].value != 0);
        }
        if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            close(s_sdl.evdev_keyboards[i]);
            s_sdl.evdev_keyboards[i] =
                s_sdl.evdev_keyboards[s_sdl.evdev_keyboard_count - 1];
            s_sdl.evdev_indices[i] =
                s_sdl.evdev_indices[s_sdl.evdev_keyboard_count - 1];
            --s_sdl.evdev_keyboard_count;
            --i;
            s_sdl.evdev_rescan_ns = 0;
        }
    }
}
#else
static void evdev_close_keyboards(void) {}
static void evdev_open_keyboards(void) {}
static void evdev_poll_keyboards(void) {}
#endif

static int gamepad_player(SDL_JoystickID id)
{
    for (unsigned i = 0; i < 2; ++i)
        if (s_sdl.gamepads[i].handle && s_sdl.gamepads[i].id == id)
            return (int)i;
    return -1;
}

static unsigned gamepad_button_action(SDL_GamepadButton button)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return TAIKO_HIT_SL;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return TAIKO_HIT_CL;
    case SDL_GAMEPAD_BUTTON_WEST:
    case SDL_GAMEPAD_BUTTON_SOUTH: return TAIKO_HIT_CR;
    case SDL_GAMEPAD_BUTTON_NORTH:
    case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return TAIKO_HIT_SR;
    case SDL_GAMEPAD_BUTTON_START: return TAIKO_ENTER;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return TAIKO_SERVICE;
    case SDL_GAMEPAD_BUTTON_BACK: return TAIKO_TEST;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return TAIKO_COIN;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return TAIKO_HIT_SL | TAIKO_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return TAIKO_HIT_CL | TAIKO_DOWN;
    default: return 0;
    }
}

static void handle_event(const SDL_Event* event)
{
    if (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        if (s_sdl.queue_mutex) {
            SDL_LockMutex(s_sdl.queue_mutex);
            s_sdl.stopping = 1;
            SDL_BroadcastCondition(s_sdl.queue_space);
            if (s_sdl.snapshot_done)
                SDL_BroadcastCondition(s_sdl.snapshot_done);
            SDL_UnlockMutex(s_sdl.queue_mutex);
        } else s_sdl.stopping = 1;
        return;
    }
    if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) {
        if (event->key.repeat) return;
#ifdef RSX_SDL_REPLAY_STANDALONE
        if (event->key.down && g_rsx_replay_key_hook) {
            g_rsx_replay_key_hook((int)event->key.scancode);
            return;
        }
#endif
        if (event->key.down && event->key.scancode == SDL_SCANCODE_F11) {
            /* ponytail: SDL borderless-desktop fullscreen, no mode switching. */
            SDL_SetWindowFullscreen(s_sdl.window,
                                    !(SDL_GetWindowFlags(s_sdl.window) &
                                      SDL_WINDOW_FULLSCREEN));
            return;
        }
#ifndef RSX_SDL_REPLAY_STANDALONE
        if (event->key.down && event->key.scancode == SDL_SCANCODE_F9) {
            toggle_performance_overlay();
            return;
        }
        if (event->key.down && event->key.scancode == SDL_SCANCODE_F10) {
            arm_capture_hotkey();
            return;
        }
#endif
        unsigned action = keyboard_action(event->key.scancode);
        if (event->key.down)
            taiko_host_input_press(0, action, event->key.timestamp);
        else
            taiko_host_input_release(0, action);
        return;
    }
    if (event->type == SDL_EVENT_GAMEPAD_ADDED) {
        for (unsigned i = 0; i < 2; ++i) {
            if (s_sdl.gamepads[i].handle) continue;
            SDL_Gamepad* pad = SDL_OpenGamepad(event->gdevice.which);
            if (pad) {
                s_sdl.gamepads[i].handle = pad;
                s_sdl.gamepads[i].id = SDL_GetGamepadID(pad);
                fprintf(stderr, "[SDL_INPUT] gamepad %u connected: %s\n", i + 1,
                        SDL_GetGamepadName(pad));
            }
            break;
        }
        return;
    }
    if (event->type == SDL_EVENT_GAMEPAD_REMOVED) {
        int player = gamepad_player(event->gdevice.which);
        if (player >= 0) {
            taiko_host_input_release((unsigned)player,
                                     s_sdl.gamepads[player].levels);
            SDL_CloseGamepad(s_sdl.gamepads[player].handle);
            SDL_zero(s_sdl.gamepads[player]);
        }
        return;
    }
    if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
        event->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        int player = gamepad_player(event->gbutton.which);
        unsigned action = gamepad_button_action((SDL_GamepadButton)event->gbutton.button);
        if (player >= 0 && action) {
            if (event->gbutton.down) {
                s_sdl.gamepads[player].levels |= action;
                taiko_host_input_press((unsigned)player, action,
                                       event->gbutton.timestamp);
            } else {
                s_sdl.gamepads[player].levels &= ~action;
                taiko_host_input_release((unsigned)player, action);
            }
        }
        return;
    }
    if (event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        int player = gamepad_player(event->gaxis.which);
        if (player < 0) return;
        unsigned action = 0;
        if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
            action = TAIKO_HIT_SL;
        else if (event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
            action = TAIKO_HIT_SR;
        if (!action) return;
        if (event->gaxis.value > 4096) {
            s_sdl.gamepads[player].levels |= action;
            taiko_host_input_press((unsigned)player, action, event->gaxis.timestamp);
        } else {
            s_sdl.gamepads[player].levels &= ~action;
            taiko_host_input_release((unsigned)player, action);
        }
    }
}

static int snapshot_one_surface(const gpu_surface* surface,
                                rsx_render_batch* destination)
{
    SDL_GPUTextureFormat format = portable_format(surface->ref.format);
    Uint32 block_size = SDL_GPUTextureFormatTexelBlockSize(format);
    u64 size = (u64)surface->ref.width * surface->ref.height * block_size;
    if (!surface->texture || !block_size || !size || size > UINT32_MAX)
        return -1;
    SDL_GPUTransferBufferCreateInfo transfer_info;
    SDL_zero(transfer_info);
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transfer_info.size = (u32)size;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(
        s_sdl.device, &transfer_info);
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(s_sdl.device);
    SDL_GPUCopyPass* copy = commands ? SDL_BeginGPUCopyPass(commands) : NULL;
    if (!transfer || !copy) {
        if (copy) SDL_EndGPUCopyPass(copy);
        if (commands) SDL_CancelGPUCommandBuffer(commands);
        if (transfer) SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer);
        return -1;
    }
    SDL_GPUTextureRegion source;
    SDL_zero(source);
    source.texture = surface->texture;
    source.w = surface->ref.width;
    source.h = surface->ref.height;
    source.d = 1;
    SDL_GPUTextureTransferInfo download;
    SDL_zero(download);
    download.transfer_buffer = transfer;
    download.pixels_per_row = surface->ref.width;
    download.rows_per_layer = surface->ref.height;
    SDL_DownloadFromGPUTexture(copy, &source, &download);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!fence || !SDL_WaitForGPUFences(s_sdl.device, true, &fence, 1)) {
        if (fence) SDL_ReleaseGPUFence(s_sdl.device, fence);
        SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer);
        return -1;
    }
    void* mapped = SDL_MapGPUTransferBuffer(s_sdl.device, transfer, false);
    rsx_surface_init* init = mapped
        ? rsx_render_batch_add_surface_init(destination) : NULL;
    int result = -1;
    if (init) {
        init->surface = surface->ref;
        init->surface.pitch = surface->ref.width * block_size;
        rsx_owned_blob* blob = surface->ref.is_depth
            ? &init->depth_stencil_data : &init->color_data;
        result = rsx_owned_blob_copy(blob, mapped, size);
    }
    if (mapped) SDL_UnmapGPUTransferBuffer(s_sdl.device, transfer);
    SDL_ReleaseGPUFence(s_sdl.device, fence);
    SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer);
    return result;
}

static void process_snapshot_request(void)
{
    if (!s_sdl.queue_mutex) return;
    SDL_LockMutex(s_sdl.queue_mutex);
    rsx_render_batch* destination = s_sdl.snapshot_pending
        ? s_sdl.snapshot_target : NULL;
    SDL_UnlockMutex(s_sdl.queue_mutex);
    if (!destination) return;
    int result = SDL_WaitForGPUIdle(s_sdl.device) ? 0 : -1;
    for (unsigned i = 0; result == 0 && i < s_sdl.surface_count; ++i)
        result = snapshot_one_surface(&s_sdl.surfaces[i], destination);
    SDL_LockMutex(s_sdl.queue_mutex);
    s_sdl.snapshot_result = result;
    s_sdl.snapshot_target = NULL;
    s_sdl.snapshot_pending = 0;
    SDL_BroadcastCondition(s_sdl.snapshot_done);
    SDL_UnlockMutex(s_sdl.queue_mutex);
}

static unsigned drain_batches(void)
{
    unsigned executed = 0;
    if (!s_sdl.queue_mutex) return 0;
    /* Executing a batch presents, and a vsync present blocks for a frame, so
     * an unbounded drain hands the main thread to the producer: while the boot
     * fast-forward ticks vblank at 240 Hz the queue refills faster than 60 Hz
     * presents empty it, the loop never returns, SDL never polls, and the
     * window desktop-side goes "not responding". Stop at a budget instead and
     * let the caller pump events; the queue's own backpressure
     * (rsx_sdl_gpu_backend_queue_has_capacity) throttles the producer. At the
     * normal rate the queue holds at most a batch or two, so this never
     * triggers and behaviour is unchanged. */
    const Uint64 budget_end = SDL_GetTicksNS() + 8000000ull;   /* 8 ms */
    for (;;) {
        rsx_render_batch batch;
        Uint64 enqueue_ns = 0;
        int have_batch = 0;
        SDL_LockMutex(s_sdl.queue_mutex);
        if (s_sdl.queue_count) {
            queued_batch* slot = &s_sdl.queue[s_sdl.queue_read];
            batch = slot->batch;
            enqueue_ns = slot->enqueue_ns;
            memset(&slot->batch, 0, sizeof(slot->batch));
            slot->enqueue_ns = 0;
            slot->occupied = 0;
            s_sdl.queue_read = (s_sdl.queue_read + 1) % SDL_RSX_QUEUE_DEPTH;
            --s_sdl.queue_count;
            SDL_SignalCondition(s_sdl.queue_space);
            have_batch = 1;
        }
        SDL_UnlockMutex(s_sdl.queue_mutex);
        if (!have_batch) break;
        execute_batch(&batch, enqueue_ns);
        rsx_render_batch_destroy(&batch);
        ++executed;
        if (SDL_GetTicksNS() >= budget_end) break;
    }
    process_snapshot_request();
    return executed;
}

int rsx_sdl_gpu_backend_main_init(unsigned width, unsigned height,
                                  const char* title)
{
    (void)width; (void)height;
    if (s_sdl.initialized) return 0;
    memset(&s_sdl, 0, sizeof(s_sdl));
    s_sdl.texture_limit = SDL_RSX_MAX_TEXTURES;
    const char* texture_limit_text = getenv("RSX_TEXTURE_CACHE_LIMIT");
    if (texture_limit_text && texture_limit_text[0]) {
        unsigned requested = (unsigned)strtoul(texture_limit_text, NULL, 0);
        if (requested < 16u) requested = 16u;
        if (requested > SDL_RSX_MAX_TEXTURES)
            requested = SDL_RSX_MAX_TEXTURES;
        s_sdl.texture_limit = requested;
        fprintf(stderr, "[SDL_GPU] sampled texture cache limit=%u\n",
                s_sdl.texture_limit);
    }
    snprintf(s_sdl.base_title, sizeof(s_sdl.base_title), "%s",
             title ? title : "Taiko no Tatsujin (ps3recomp)");
    s_sdl.fps_log = getenv("RSX_FPS_LOG") != NULL;
    const char* perf_overlay = getenv("TAIKO_PERF_OVERLAY");
    s_sdl.perf_overlay = perf_overlay && strcmp(perf_overlay, "0") != 0;
    if (s_sdl.perf_overlay)
        fprintf(stderr, "[SDL_GPU] performance overlay enabled (F9 toggles)\n");
    s_sdl.fence_present = getenv("TAIKO_GPU_UNFENCED_PRESENT") == NULL;
    /* The producer is already paced to 60 Hz by the guest's vblank, so queueing
     * several finished frames buys no throughput -- it only adds latency, and
     * when the queue does fill the producer blocks until the consumer's next
     * vsync-locked present, which costs it a whole vblank. Measured at depth 4:
     * batches waited 41 ms before display and the submit interval went bimodal
     * at 16.7/33.3 ms. At depth 2 the same scenes waited 7-8 ms with no FPS
     * cost, so 2 is the default and the full depth stays available. */
    s_sdl.queue_limit = 2u;
    const char* depth_text = getenv("TAIKO_RSX_QUEUE_DEPTH");
    if (depth_text) {
        int depth = atoi(depth_text);
        if (depth >= 1 && depth <= (int)SDL_RSX_QUEUE_DEPTH)
            s_sdl.queue_limit = (unsigned)depth;
    }
    fprintf(stderr, "[SDL_GPU] batch queue depth %u\n", s_sdl.queue_limit);
    s_sdl.pace_trace = getenv("RSX_FRAME_PACING_TRACE") != NULL;
    const SDL_InitFlags required_subsystems = SDL_INIT_VIDEO | SDL_INIT_GAMEPAD;
    if ((SDL_WasInit(required_subsystems) & required_subsystems) !=
        required_subsystems) {
        fprintf(stderr, "[SDL_GPU] host SDL lifecycle is not initialized\n");
        return -1;
    }
    if (!SDL_ShaderCross_Init()) {
        fprintf(stderr, "[SDL_GPU] shadercross init failed: %s\n", SDL_GetError());
        return -1;
    }
    /* A kiosk wants the surface fullscreen from the first frame, and not only
     * for looks: wlroots only hands a buffer straight to the display controller
     * when one fullscreen surface covers the output and nothing (a cursor, a
     * second surface) has to be blended over it. As a plain resizable toplevel
     * the compositor composited every frame instead, which measured 16.6% of
     * the Pi's GPU -- more than the game itself was using. */
#ifdef RSX_SDL_KMS_PRESENT
    /* Driving KMS ourselves means there is no compositor to give us a window,
     * and no swapchain: frames go from the display target to a scanout buffer.
     * See rsx_kms_present.h for why the Pi needs this. */
    s_sdl.kms_present = getenv("TAIKO_KMS_PRESENT") != NULL;
    s_sdl.kms_zero_copy = s_sdl.kms_present &&
        getenv("TAIKO_KMS_ZERO_COPY") != NULL;
#endif
    SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE;
    if (getenv("TAIKO_FULLSCREEN")) window_flags |= SDL_WINDOW_FULLSCREEN;
    if (!s_sdl.kms_present) {
        s_sdl.window = SDL_CreateWindow(s_sdl.base_title,
                                        SDL_RSX_WIDTH, SDL_RSX_HEIGHT,
                                        window_flags);
        if (!s_sdl.window) goto fail;
    }
    if (getenv("TAIKO_HIDE_CURSOR")) SDL_HideCursor();
    const char* requested_driver = getenv("TAIKO_GPU_DRIVER");
    if (requested_driver && !requested_driver[0]) requested_driver = NULL;
    const SDL_GPUShaderFormat shader_formats =
        SDL_ShaderCross_GetHLSLShaderFormats();
    if (s_sdl.kms_zero_copy) {
        const char* extensions[] = {
            "VK_KHR_external_memory",
            "VK_KHR_external_memory_fd",
            "VK_EXT_external_memory_dma_buf",
        };
        SDL_GPUVulkanOptions vulkan_options;
        SDL_zero(vulkan_options);
        vulkan_options.vulkan_api_version = (1u << 22) | (1u << 12); /* 1.1 */
        vulkan_options.device_extension_count = SDL_arraysize(extensions);
        vulkan_options.device_extension_names = extensions;
        SDL_PropertiesID props = SDL_CreateProperties();
        if (!props) goto fail;
        SDL_SetBooleanProperty(
            props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN,
            (shader_formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0);
        SDL_SetBooleanProperty(
            props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN,
            getenv("SDL_GPU_DEBUG") != NULL);
        if (requested_driver)
            SDL_SetStringProperty(
                props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING,
                requested_driver);
        SDL_SetPointerProperty(
            props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER,
            &vulkan_options);
        s_sdl.device = SDL_CreateGPUDeviceWithProperties(props);
        SDL_DestroyProperties(props);
    } else {
#ifdef __ANDROID__
        /* Older Adreno Vulkan implementations can lack
         * drawIndirectFirstInstance. SDL_GPU exposes it as an optional
         * creation feature but enables it by default; this renderer issues no
         * indirect draws, so do not reject otherwise capable Android GPUs. */
        SDL_PropertiesID props = SDL_CreateProperties();
        if (!props) goto fail;
        SDL_SetBooleanProperty(
            props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN,
            (shader_formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0);
        SDL_SetBooleanProperty(
            props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXBC_BOOLEAN,
            (shader_formats & SDL_GPU_SHADERFORMAT_DXBC) != 0);
        SDL_SetBooleanProperty(
            props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN,
            (shader_formats & SDL_GPU_SHADERFORMAT_DXIL) != 0);
        SDL_SetBooleanProperty(
            props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN,
            (shader_formats & SDL_GPU_SHADERFORMAT_MSL) != 0);
        SDL_SetBooleanProperty(
            props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN,
            getenv("SDL_GPU_DEBUG") != NULL);
        SDL_SetBooleanProperty(
            props,
            SDL_PROP_GPU_DEVICE_CREATE_FEATURE_INDIRECT_DRAW_FIRST_INSTANCE_BOOLEAN,
            false);
        if (requested_driver)
            SDL_SetStringProperty(
                props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING,
                requested_driver);
        s_sdl.device = SDL_CreateGPUDeviceWithProperties(props);
        SDL_DestroyProperties(props);
#else
        s_sdl.device = SDL_CreateGPUDevice(
            shader_formats, getenv("SDL_GPU_DEBUG") != NULL,
            requested_driver);
#endif
    }
    if (!s_sdl.device) goto fail;
    if (requested_driver)
        fprintf(stderr, "[SDL_GPU] driver: %s (requested %s)\n",
                SDL_GetGPUDeviceDriver(s_sdl.device), requested_driver);
    else
        fprintf(stderr, "[SDL_GPU] driver: %s (automatic)\n",
                SDL_GetGPUDeviceDriver(s_sdl.device));
    if (s_sdl.kms_present) {
#ifdef RSX_SDL_KMS_PRESENT
        s_sdl.kms_gpu_idle = getenv("TAIKO_KMS_GPU_IDLE") != NULL;
        if (rsx_kms_present_init(SDL_RSX_WIDTH, SDL_RSX_HEIGHT) != 0) {
            fprintf(stderr, "[SDL_GPU] KMS presentation unavailable\n");
            goto fail;
        }
        if (s_sdl.kms_zero_copy) {
            s_sdl.kms_modifier_count = rsx_kms_present_get_modifiers(
                s_sdl.kms_modifiers, SDL_RSX_KMS_MODIFIERS);
        }
        /* A direct-scanout target remains owned by KMS until a later atomic
         * commit reaches the CRTC. Two slots turn a frame that narrowly misses
         * vblank into a 30 Hz render/acquire cycle; the normal download path
         * does not have that ownership constraint on its render targets. */
        s_sdl.kms_display_count = s_sdl.kms_zero_copy ? 3u : 2u;
        for (unsigned i = 0; i < s_sdl.kms_display_count; ++i) {
            s_sdl.kms_display[i] = create_presentation_slot();
            s_sdl.kms_display_reader[i] = -1;
            if (!s_sdl.kms_display[i]) goto fail;
            if (s_sdl.kms_zero_copy) {
                int fd = -1;
                Uint32 pitch = 0;
                Uint64 offset = 0, modifier = 0;
                if (!SDL_GPUVulkanExportTextureDMABUF) {
                    fprintf(stderr,
                            "[SDL_GPU] zero-copy KMS needs the Pi SDL "
                            "dma-buf extension\n");
                    goto fail;
                }
                if (!SDL_GPUVulkanExportTextureDMABUF(
                        s_sdl.device, s_sdl.kms_display[i], &fd, &pitch,
                        &offset, &modifier)) {
                    fprintf(stderr,
                            "[SDL_GPU] presentation dma-buf export failed: %s\n",
                            SDL_GetError());
                    goto fail;
                }
                if (rsx_kms_present_import_dmabuf(
                        i, fd, pitch, offset, modifier) != 0)
                    goto fail;
            }
        }
        if (s_sdl.kms_zero_copy) {
            Uint64 wait_ns = 0;
            if (rsx_kms_present_acquire_dmabuf(0, &wait_ns) != 0) goto fail;
            fprintf(stderr, "[SDL_GPU] zero-copy KMS dma-buf path active\n");
        }
        s_sdl.display = s_sdl.kms_display[0];
#endif
        goto swapchain_ready;
    }
    if (!SDL_ClaimWindowForGPUDevice(s_sdl.device, s_sdl.window)) goto fail;

    const int immediate_supported = SDL_WindowSupportsGPUPresentMode(
        s_sdl.device, s_sdl.window, SDL_GPU_PRESENTMODE_IMMEDIATE);
    const int mailbox_supported = SDL_WindowSupportsGPUPresentMode(
        s_sdl.device, s_sdl.window, SDL_GPU_PRESENTMODE_MAILBOX);
    const char* present_text = getenv("TAIKO_PRESENT_MODE");
    SDL_GPUPresentMode present_mode = SDL_GPU_PRESENTMODE_VSYNC;
    const char* present_name = "vsync";
    if (present_text && strcmp(present_text, "mailbox") == 0) {
        present_mode = SDL_GPU_PRESENTMODE_MAILBOX;
        present_name = "mailbox";
    } else if (present_text && strcmp(present_text, "immediate") == 0) {
        present_mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
        present_name = "immediate";
    }
    if (!SDL_WindowSupportsGPUPresentMode(s_sdl.device, s_sdl.window,
                                          present_mode) ||
        !SDL_SetGPUSwapchainParameters(s_sdl.device, s_sdl.window,
                                       SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                       present_mode)) {
        fprintf(stderr,
                "[SDL_GPU] present mode '%s' unavailable (%s); using vsync\n",
                present_name, SDL_GetError());
        present_mode = SDL_GPU_PRESENTMODE_VSYNC;
        present_name = "vsync";
        if (!SDL_SetGPUSwapchainParameters(s_sdl.device, s_sdl.window,
                                           SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                           present_mode))
            goto fail;
    }
    const char* flight_text = getenv("TAIKO_FRAMES_IN_FLIGHT");
    unsigned frames_in_flight = flight_text
        ? (unsigned)strtoul(flight_text, NULL, 0) : 2u;
    if (frames_in_flight < 1u) frames_in_flight = 1u;
    if (frames_in_flight > 3u) frames_in_flight = 3u;
    if (!SDL_SetGPUAllowedFramesInFlight(s_sdl.device, frames_in_flight))
        goto fail;
    fprintf(stderr,
            "[SDL_GPU] swapchain present=%s frames_in_flight=%u "
            "supported(immediate=%d mailbox=%d)\n",
            present_name, frames_in_flight,
            immediate_supported, mailbox_supported);
swapchain_ready:
    ;

    static const rsx_portable_format required[] = {
        RSX_FORMAT_RGBA8, RSX_FORMAT_RGBA16F, RSX_FORMAT_RGBA32F, RSX_FORMAT_R32F
    };
    for (unsigned i = 0; i < SDL_arraysize(required); ++i) {
        SDL_GPUTextureFormat format = portable_format(required[i]);
        if (!SDL_GPUTextureSupportsFormat(
                s_sdl.device, format, SDL_GPU_TEXTURETYPE_2D,
                SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
            fprintf(stderr, "[SDL_GPU] required render-target format %u unavailable\n",
                    (unsigned)required[i]);
            goto fail;
        }
    }
    SDL_GPUTextureCreateInfo white_info;
    SDL_zero(white_info);
    white_info.type = SDL_GPU_TEXTURETYPE_2D;
    white_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    white_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER |
                       SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    white_info.width = white_info.height = white_info.layer_count_or_depth = 1;
    white_info.num_levels = 1;
    white_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    s_sdl.white_texture = SDL_CreateGPUTexture(s_sdl.device, &white_info);
    SDL_GPUSamplerCreateInfo sampler_info;
    SDL_zero(sampler_info);
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler_info.address_mode_u = sampler_info.address_mode_v =
        sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.max_lod = 1000.0f;
    s_sdl.default_sampler = SDL_CreateGPUSampler(s_sdl.device, &sampler_info);
    if (!s_sdl.white_texture || !s_sdl.default_sampler) goto fail;
    {
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(s_sdl.device);
        SDL_GPUColorTargetInfo white_target;
        SDL_zero(white_target);
        white_target.texture = s_sdl.white_texture;
        white_target.clear_color.r = white_target.clear_color.g =
            white_target.clear_color.b = white_target.clear_color.a = 1.0f;
        white_target.load_op = SDL_GPU_LOADOP_CLEAR;
        white_target.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = commands
            ? SDL_BeginGPURenderPass(commands, &white_target, 1, NULL) : NULL;
        if (!pass) goto fail;
        SDL_EndGPURenderPass(pass);
        if (submit_commands(commands) != 0) goto fail;
    }
    s_sdl.queue_mutex = SDL_CreateMutex();
    s_sdl.queue_space = SDL_CreateCondition();
    s_sdl.snapshot_done = SDL_CreateCondition();
    s_sdl.wake_event = SDL_RegisterEvents(1);
    if (!s_sdl.queue_mutex || !s_sdl.queue_space || !s_sdl.snapshot_done ||
        s_sdl.wake_event == (Uint32)-1)
        goto fail;
#ifndef RSX_SDL_REPLAY_STANDALONE
    if (rsx_recorder_install(NULL, &s_consumer, &s_sdl) != 0) goto fail;
#endif
    s_sdl.initialized = 1;
    taiko_host_input_set_active(1);
    evdev_open_keyboards();
    fprintf(stderr, "[SDL_GPU] initialized %s, fixed output %ux%u\n",
            SDL_GetGPUDeviceDriver(s_sdl.device), SDL_RSX_WIDTH, SDL_RSX_HEIGHT);
    return 0;

fail:
    fprintf(stderr, "[SDL_GPU] initialization failed: %s\n", SDL_GetError());
    rsx_sdl_gpu_backend_main_shutdown();
    return -1;
}

int rsx_sdl_gpu_backend_main_iterate(int timeout_ms)
{
    /* Work left over from a budget-bounded drain must not sit behind an idle
     * event wait, or presentation would fall to one batch per timeout. */
    if (rsx_sdl_gpu_backend_has_pending_batches()) timeout_ms = 0;

    /* This thread owns the window, so it is the one Windows watches: five
     * seconds without pumping and the desktop replaces the window with a grey
     * "Not Responding" ghost while the game keeps rendering behind it. Report
     * any iteration that takes long enough to matter, split into the wait and
     * the batch execution, so a stall names its own half. */
    const Uint64 iterate_start_ns = SDL_GetTicksNS();

    evdev_poll_keyboards();
    SDL_Event event;
    if (SDL_WaitEventTimeout(&event, timeout_ms)) {
        handle_event(&event);
        while (SDL_PollEvent(&event)) handle_event(&event);
    }
    evdev_poll_keyboards();
    const Uint64 events_done_ns = SDL_GetTicksNS();
    const unsigned executed = drain_batches();
    const Uint64 end_ns = SDL_GetTicksNS();

    if (end_ns - iterate_start_ns > 250000000ull) {
        fprintf(stderr, "[SDL_GPU-STALL] iterate %.0f ms "
                        "(events %.0f ms, %u batches %.0f ms, timeout %d ms)\n",
                (double)(end_ns - iterate_start_ns) / 1000000.0,
                (double)(events_done_ns - iterate_start_ns) / 1000000.0,
                executed,
                (double)(end_ns - events_done_ns) / 1000000.0,
                timeout_ms);
    }
    return s_sdl.stopping;
}

int rsx_sdl_gpu_backend_queue_has_capacity(void)
{
    if (!s_sdl.queue_mutex) return 0;
    SDL_LockMutex(s_sdl.queue_mutex);
    int result = s_sdl.queue_count < queue_depth() && !s_sdl.stopping;
    SDL_UnlockMutex(s_sdl.queue_mutex);
    return result;
}

int rsx_sdl_gpu_backend_has_pending_batches(void)
{
    if (!s_sdl.queue_mutex) return 0;
    SDL_LockMutex(s_sdl.queue_mutex);
    int result = s_sdl.queue_count != 0;
    SDL_UnlockMutex(s_sdl.queue_mutex);
    return result;
}

unsigned rsx_sdl_gpu_backend_error_count(void) { return s_sdl.errors; }

int rsx_sdl_gpu_backend_submit_batch(const rsx_render_batch* batch)
{
    return consumer_submit(&s_sdl, batch);
}

int rsx_sdl_gpu_backend_save_display_bmp(const char* path)
{
    if (!path || !s_sdl.display) return -1;
    Uint32 source_width, source_height;
    SDL_GPUTexture* source_texture = presentation_texture(
        &source_width, &source_height);
    const Uint32 pitch = source_width * 4u;
    const Uint32 bytes = pitch * source_height;
    SDL_GPUTransferBufferCreateInfo transfer_info;
    SDL_zero(transfer_info);
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transfer_info.size = bytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(
        s_sdl.device, &transfer_info);
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(s_sdl.device);
    if (!transfer || !commands) {
        if (commands) SDL_CancelGPUCommandBuffer(commands);
        goto fail;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    ++s_sdl.perf_copy_passes;
    if (!copy) goto fail_commands;
    SDL_GPUTextureRegion source;
    SDL_zero(source);
    source.texture = source_texture;
    source.w = source_width;
    source.h = source_height;
    source.d = 1;
    SDL_GPUTextureTransferInfo destination;
    SDL_zero(destination);
    destination.transfer_buffer = transfer;
    destination.pixels_per_row = source_width;
    destination.rows_per_layer = source_height;
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!fence) goto fail;
    SDL_GPUFence* fences[] = {fence};
    if (!SDL_WaitForGPUFences(s_sdl.device, true, fences, 1)) {
        SDL_ReleaseGPUFence(s_sdl.device, fence);
        goto fail;
    }
    void* pixels = SDL_MapGPUTransferBuffer(s_sdl.device, transfer, false);
    if (!pixels) {
        SDL_ReleaseGPUFence(s_sdl.device, fence);
        goto fail;
    }
    SDL_Surface* surface = SDL_CreateSurfaceFrom(
        source_width, source_height, SDL_PIXELFORMAT_RGBA32, pixels, pitch);
    int result = surface && SDL_SaveBMP(surface, path) ? 0 : -1;
    if (surface) SDL_DestroySurface(surface);
    SDL_UnmapGPUTransferBuffer(s_sdl.device, transfer);
    SDL_ReleaseGPUFence(s_sdl.device, fence);
    SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer);
    if (result != 0)
        fprintf(stderr, "[SDL_GPU] BMP write failed: %s\n", SDL_GetError());
    return result;

fail_commands:
    SDL_CancelGPUCommandBuffer(commands);
fail:
    if (transfer) SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer);
    fprintf(stderr, "[SDL_GPU] display readback failed: %s\n", SDL_GetError());
    return -1;
}

void rsx_sdl_gpu_backend_main_shutdown(void)
{
#ifdef RSX_SDL_KMS_PRESENT
    if (s_sdl.kms_present) {
        kms_stop_worker();
        rsx_kms_present_shutdown();
    }
#endif
    s_sdl.stopping = 1;
    if (s_sdl.queue_mutex) {
        SDL_LockMutex(s_sdl.queue_mutex);
        SDL_BroadcastCondition(s_sdl.queue_space);
        if (s_sdl.snapshot_done)
            SDL_BroadcastCondition(s_sdl.snapshot_done);
        SDL_UnlockMutex(s_sdl.queue_mutex);
    }
#ifndef RSX_SDL_REPLAY_STANDALONE
    if (s_sdl.initialized) rsx_recorder_uninstall();
#endif
    drain_batches();
    taiko_host_input_set_active(0);
    evdev_close_keyboards();
    for (unsigned i = 0; i < 2; ++i)
        if (s_sdl.gamepads[i].handle) SDL_CloseGamepad(s_sdl.gamepads[i].handle);
    if (s_sdl.device) {
        SDL_WaitForGPUIdle(s_sdl.device);
        for (unsigned i = 0; i < s_sdl.pipeline_count; ++i)
            if (s_sdl.pipelines[i].pipeline)
                SDL_ReleaseGPUGraphicsPipeline(s_sdl.device,
                                                s_sdl.pipelines[i].pipeline);
        for (unsigned i = 0; i < s_sdl.shader_count; ++i)
            if (s_sdl.shaders[i].shader)
                SDL_ReleaseGPUShader(s_sdl.device, s_sdl.shaders[i].shader);
        for (unsigned i = 0; i < s_sdl.texture_count; ++i)
            if (s_sdl.textures[i].texture)
                SDL_ReleaseGPUTexture(s_sdl.device, s_sdl.textures[i].texture);
        for (unsigned i = 0; i < s_sdl.sampler_count; ++i)
            if (s_sdl.samplers[i].sampler)
                SDL_ReleaseGPUSampler(s_sdl.device, s_sdl.samplers[i].sampler);
        for (unsigned i = 0; i < s_sdl.surface_count; ++i)
            if (s_sdl.surfaces[i].texture)
                SDL_ReleaseGPUTexture(s_sdl.device, s_sdl.surfaces[i].texture);
        for (unsigned i = 0; i < SDL_RSX_PRESENT_SLOTS; ++i) {
            if (s_sdl.presentation_fences[i])
                SDL_ReleaseGPUFence(s_sdl.device,
                                    s_sdl.presentation_fences[i]);
            /* Slot zero is the original display texture and is owned by the
             * persistent surface table above. */
            if (i && s_sdl.presentation_slots[i])
                SDL_ReleaseGPUTexture(s_sdl.device,
                                      s_sdl.presentation_slots[i]);
        }
        for (unsigned i = 0; i < s_sdl.kms_display_count; ++i)
            if (s_sdl.kms_display[i])
                SDL_ReleaseGPUTexture(s_sdl.device, s_sdl.kms_display[i]);
        if (s_sdl.white_texture)
            SDL_ReleaseGPUTexture(s_sdl.device, s_sdl.white_texture);
        if (s_sdl.default_sampler)
            SDL_ReleaseGPUSampler(s_sdl.device, s_sdl.default_sampler);
        if (s_overlay.texture)
            SDL_ReleaseGPUTexture(s_sdl.device, s_overlay.texture);
        if (s_overlay.pipeline)
            SDL_ReleaseGPUGraphicsPipeline(s_sdl.device, s_overlay.pipeline);
        if (s_overlay.sampler)
            SDL_ReleaseGPUSampler(s_sdl.device, s_overlay.sampler);
        SDL_zero(s_overlay);
        if (s_fps_overlay.texture)
            SDL_ReleaseGPUTexture(s_sdl.device, s_fps_overlay.texture);
        SDL_zero(s_fps_overlay);
        release_dynamic_upload(&s_sdl.vertices_upload);
        release_dynamic_upload(&s_sdl.vertex_constants_upload);
        if (s_sdl.window) SDL_ReleaseWindowFromGPUDevice(s_sdl.device, s_sdl.window);
        SDL_DestroyGPUDevice(s_sdl.device);
    }
    if (s_sdl.queue_space) SDL_DestroyCondition(s_sdl.queue_space);
    if (s_sdl.snapshot_done) SDL_DestroyCondition(s_sdl.snapshot_done);
    if (s_sdl.queue_mutex) SDL_DestroyMutex(s_sdl.queue_mutex);
    if (s_sdl.window) SDL_DestroyWindow(s_sdl.window);
    SDL_ShaderCross_Quit();
    memset(&s_sdl, 0, sizeof(s_sdl));
}

#else

int rsx_sdl_gpu_backend_main_init(unsigned width, unsigned height,
                                  const char* title)
{ (void)width; (void)height; (void)title; return -1; }
int rsx_sdl_gpu_backend_main_iterate(int timeout_ms)
{ (void)timeout_ms; return 1; }
void rsx_sdl_gpu_backend_main_shutdown(void) {}
int rsx_sdl_gpu_backend_queue_has_capacity(void) { return 0; }
int rsx_sdl_gpu_backend_has_pending_batches(void) { return 0; }
unsigned rsx_sdl_gpu_backend_error_count(void) { return 0; }
int rsx_sdl_gpu_backend_submit_batch(const struct rsx_render_batch* batch)
{ (void)batch; return -1; }
int rsx_sdl_gpu_backend_save_display_bmp(const char* path)
{ (void)path; return -1; }

#endif

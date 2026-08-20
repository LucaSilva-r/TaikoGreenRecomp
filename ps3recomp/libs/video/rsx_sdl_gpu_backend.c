#include "rsx_sdl_gpu_backend.h"

#ifdef PS3RECOMP_RSX_BACKEND_SDL_GPU

#include "rsx_recorder.h"
#include "rsx_render_batch.h"
#include "rsx_vp_decompiler.h"
#include "rsx_fp_decompiler.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3_shadercross/SDL_shadercross.h>

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define SDL_RSX_WIDTH 1280u
#define SDL_RSX_HEIGHT 720u
#define SDL_RSX_QUEUE_DEPTH 4u
#define SDL_RSX_MAX_SURFACES 256u
#define SDL_RSX_MAX_SHADERS 512u
#define SDL_RSX_MAX_PIPELINES 1024u
#define SDL_RSX_MAX_TEXTURES 1024u
#define SDL_RSX_MAX_SAMPLERS 64u
#define SDL_RSX_SHADER_DIALECT_VERSION 2u
#define SDL_RSX_PACE_SAMPLES 256u

typedef struct shader_entry {
    u64 hash;
    unsigned stage;
    unsigned flags;
    SDL_GPUShader* shader;
    SDL_ShaderCross_GraphicsShaderResourceInfo resources;
    u8 sampler_units[4]; /* dense SDL slot -> sparse RSX texture unit */
    u8 sampler_unit_count;
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

typedef struct sdl_rsx_state {
    SDL_Window* window;
    SDL_GPUDevice* device;
    SDL_Mutex* queue_mutex;
    SDL_Condition* queue_space;
    SDL_Condition* snapshot_done;
    queued_batch queue[SDL_RSX_QUEUE_DEPTH];
    unsigned queue_read;
    unsigned queue_write;
    unsigned queue_count;
    Uint32 wake_event;
    gpu_surface surfaces[SDL_RSX_MAX_SURFACES];
    unsigned surface_count;
    SDL_GPUTexture* display;
    gamepad_slot gamepads[2];
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
    sampler_entry samplers[SDL_RSX_MAX_SAMPLERS];
    unsigned sampler_count;
    unsigned errors;
    char base_title[160];
    Uint64 fps_window_start_ns;
    Uint64 fps_window_frames;
    Uint64 perf_batches;
    Uint64 perf_prepare_ns;
    Uint64 perf_constants_ns;
    Uint64 perf_vertices_ns;
    Uint64 perf_render_ns;
    Uint64 perf_present_ns;
    Uint64 perf_render_passes;
    unsigned last_batch_draws;
    int fps_log;
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
    if (!commands) return -1;
    if (!SDL_SubmitGPUCommandBuffer(commands)) {
        fprintf(stderr, "[SDL_GPU] command submission failed: %s\n", SDL_GetError());
        ++s_sdl.errors;
        return -1;
    }
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
    Uint8* spirv = SDL_ShaderCross_CompileSPIRVFromHLSL(&hlsl, &spirv_size);
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

static shader_entry* get_shader(const rsx_render_op* op,
                                SDL_ShaderCross_ShaderStage stage)
{
    const rsx_draw_op_data* draw = &op->data.draw;
    const rsx_owned_blob* blob = stage == SDL_SHADERCROSS_SHADERSTAGE_VERTEX
        ? &draw->vertex_shader : &draw->fragment_shader;
    u64 hash = stage == SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT && blob->size
        ? rsx_fp_program_structure_hash(blob->data, (u32)blob->size)
        : blob->hash;
    unsigned flags = stage == SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT
        ? draw->pipeline.fragment_32bit_exports : draw->pipeline.vertex_layout;
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
    if (stage == SDL_SHADERCROSS_SHADERSTAGE_VERTEX && blob->size) {
        result = rsx_vp_decompile(blob->data, (u32)blob->size,
                                  source, 512u * 1024u);
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
        replace_all(source, 512u * 1024u, "vp_c[",
                    "rsx_vp_constants[rsx_vp_base+");
        replace_all(source, 512u * 1024u, "vp_posscale",
                    "rsx_vp_constants[rsx_vp_base+512]");
        replace_all(source, 512u * 1024u, "vp_posoffset",
                    "rsx_vp_constants[rsx_vp_base+513]");
        /* SDL_GPU reserves set/space 1 for vertex-stage uniforms. This also
         * handles an unexpected older decompiler declaration defensively. */
        replace_all(source, 512u * 1024u,
                    "cbuffer VPConst : register(b0)",
                    "cbuffer VPConst : register(b0, space1)");
    } else if (stage == SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT && blob->size) {
        result = rsx_fp_decompile_dynamic(
            blob->data, (u32)blob->size, source, 512u * 1024u,
            draw->pipeline.fragment_32bit_exports);
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
    if (op->data.draw.fragment_shader.size)
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
    SDL_GPUVertexBufferDescription buffer_description;
    SDL_zero(buffer_description);
    buffer_description.slot = 0;
    buffer_description.pitch = key->vertex_layout == RSX_VERTEX_LAYOUT_FLOAT4_X16
        ? 16u * 4u * sizeof(float) : 9u * sizeof(float);
    buffer_description.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attributes[16];
    SDL_zero(attributes);
    unsigned attribute_count = key->vertex_layout == RSX_VERTEX_LAYOUT_FLOAT4_X16 ? 16 : 3;
    for (unsigned i = 0; i < attribute_count; ++i) {
        attributes[i].location = i;
        attributes[i].buffer_slot = 0;
        attributes[i].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attributes[i].offset = i * 4u * sizeof(float);
    }
    if (attribute_count == 3) {
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

static SDL_GPUBuffer* upload_buffer(const void* data, u64 size,
                                    SDL_GPUBufferUsageFlags usage)
{
    if (!data || !size || size > UINT32_MAX) return NULL;
    SDL_GPUBufferCreateInfo buffer_info;
    SDL_zero(buffer_info);
    buffer_info.usage = usage;
    buffer_info.size = (u32)size;
    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(s_sdl.device, &buffer_info);
    SDL_GPUTransferBufferCreateInfo transfer_info;
    SDL_zero(transfer_info);
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = (u32)size;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(
        s_sdl.device, &transfer_info);
    if (!buffer || !transfer) goto fail;
    void* mapped = SDL_MapGPUTransferBuffer(s_sdl.device, transfer, false);
    if (!mapped) goto fail;
    memcpy(mapped, data, (size_t)size);
    SDL_UnmapGPUTransferBuffer(s_sdl.device, transfer);
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(s_sdl.device);
    if (!commands) goto fail;
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) { SDL_CancelGPUCommandBuffer(commands); goto fail; }
    SDL_GPUTransferBufferLocation source = {transfer, 0};
    SDL_GPUBufferRegion destination = {buffer, 0, (u32)size};
    SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    if (submit_commands(commands) != 0) goto fail;
    SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer);
    return buffer;
fail:
    if (transfer) SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer);
    if (buffer) SDL_ReleaseGPUBuffer(s_sdl.device, buffer);
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

static SDL_GPURenderPass* begin_draw_pass(SDL_GPUCommandBuffer* commands,
                                          const rsx_render_op* op)
{
    SDL_GPUColorTargetInfo colors[4];
    SDL_zero(colors);
    unsigned color_count = 0;
    for (unsigned i = 0; i < op->color_count && i < 4; ++i) {
        SDL_GPUTexture* texture = get_surface(&op->color[i]);
        if (!texture) continue;
        colors[color_count].texture = texture;
        colors[color_count].load_op = SDL_GPU_LOADOP_LOAD;
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
        depth.store_op = SDL_GPU_STOREOP_STORE;
        depth.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
        depth.stencil_store_op = SDL_GPU_STOREOP_STORE;
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
    SDL_GPUTexture* result = s_sdl.display;
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

static int present_display(void)
{
    if (!s_sdl.display || !s_sdl.window) return -1;
    Uint32 source_width, source_height;
    SDL_GPUTexture* source_texture = presentation_texture(
        &source_width, &source_height);
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(s_sdl.device);
    if (!commands) { ++s_sdl.errors; return -1; }
    SDL_GPUTexture* swapchain = NULL;
    Uint32 width = 0, height = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, s_sdl.window,
                                               &swapchain, &width, &height)) {
        fprintf(stderr, "[SDL_GPU] swapchain acquisition failed: %s\n", SDL_GetError());
        ++s_sdl.errors;
        submit_commands(commands);
        return -1;
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
    }
    return submit_commands(commands);
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
    SDL_SetWindowTitle(s_sdl.window, title);
    if (s_sdl.fps_log)
        fprintf(stderr,
                "[RSXFPS] %.2f fps draws=%u errors=%u "
                "cpu_ms(prep=%.2f const=%.2f vert=%.2f render=%.2f present=%.2f) "
                "passes=%.1f\n",
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
                s_sdl.perf_batches ? (double)s_sdl.perf_render_passes /
                    (double)s_sdl.perf_batches : 0.0);
    s_sdl.fps_window_start_ns = now;
    s_sdl.fps_window_frames = 0;
    s_sdl.perf_batches = 0;
    s_sdl.perf_prepare_ns = 0;
    s_sdl.perf_constants_ns = 0;
    s_sdl.perf_vertices_ns = 0;
    s_sdl.perf_render_ns = 0;
    s_sdl.perf_present_ns = 0;
    s_sdl.perf_render_passes = 0;
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
    u32* vertex_offsets = NULL;
    u64 constant_stride = (u64)RSX_BATCH_VP_CONSTANTS * 4u * sizeof(float);
    int needs_constants = 0;
    u64 vertex_bytes = 0;
    unsigned draw_count = 0;
    for (unsigned i = 0; i < batch->operation_count; ++i)
        if (batch->operations[i].type == RSX_RENDER_OP_DRAW) {
            const rsx_render_op* op = &batch->operations[i];
            ++draw_count;
            if (op->data.draw.vertex_shader.size)
                needs_constants = 1;
            u64 aligned = (vertex_bytes + 15u) & ~(u64)15u;
            if (aligned > UINT32_MAX ||
                op->data.draw.vertex_data.size > UINT32_MAX - aligned) {
                vertex_bytes = UINT32_MAX + (u64)1;
                break;
            }
            vertex_bytes = aligned + op->data.draw.vertex_data.size;
        }
    s_sdl.last_batch_draws = draw_count;

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
    if (resource_trace && perf_mark - perf_start >= UINT64_C(5000000))
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

    u64 constant_bytes = needs_constants
        ? (u64)batch->operation_count * constant_stride : 0;
    if (constant_bytes && constant_bytes <= UINT32_MAX) {
        u8* packed = (u8*)calloc(1, (size_t)constant_bytes);
        if (packed) {
            for (unsigned i = 0; i < batch->operation_count; ++i) {
                const rsx_render_op* op = &batch->operations[i];
                if (op->type != RSX_RENDER_OP_DRAW) continue;
                u64 bytes = op->data.draw.vertex_constants.size;
                if (bytes > constant_stride) bytes = constant_stride;
                if (bytes)
                    memcpy(packed + (u64)i * constant_stride,
                           op->data.draw.vertex_constants.data, (size_t)bytes);
                /* RSXB v2's VP epilogue constants are defined in clip space.
                 * Canonicalize x/y at the consumer boundary as well so early
                 * v2 captures produced before the recorder-side fix remain
                 * replayable. The z lanes keep the recorded GL-to-D3D depth
                 * remap. */
                if (bytes >= constant_stride) {
                    float (*constants)[4] = (float (*)[4])(
                        packed + (u64)i * constant_stride);
                    constants[512][0] = 1.0f;
                    constants[512][1] = 1.0f;
                    constants[512][3] = 1.0f;
                    constants[513][0] = 0.0f;
                    constants[513][1] = 0.0f;
                    constants[513][3] = 0.0f;
                }
            }
            vertex_constants = upload_buffer(
                packed, constant_bytes,
                SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
            free(packed);
        }
    }
    perf_mark = SDL_GetTicksNS();
    s_sdl.perf_constants_ns += perf_mark - perf_start;
    perf_start = perf_mark;

    if (batch->operation_count && vertex_bytes && vertex_bytes <= UINT32_MAX) {
        vertex_offsets = (u32*)malloc(
            (size_t)batch->operation_count * sizeof(*vertex_offsets));
        u8* packed = (u8*)malloc((size_t)vertex_bytes);
        if (vertex_offsets)
            memset(vertex_offsets, 0xff,
                   (size_t)batch->operation_count * sizeof(*vertex_offsets));
        if (vertex_offsets && packed) {
            u64 offset = 0;
            for (unsigned i = 0; i < batch->operation_count; ++i) {
                const rsx_render_op* op = &batch->operations[i];
                if (op->type != RSX_RENDER_OP_DRAW ||
                    !op->data.draw.vertex_data.size)
                    continue;
                offset = (offset + 15u) & ~(u64)15u;
                vertex_offsets[i] = (u32)offset;
                memcpy(packed + offset, op->data.draw.vertex_data.data,
                       (size_t)op->data.draw.vertex_data.size);
                offset += op->data.draw.vertex_data.size;
            }
            vertices = upload_buffer(packed, vertex_bytes,
                                     SDL_GPU_BUFFERUSAGE_VERTEX);
        }
        free(packed);
    }
    perf_mark = SDL_GetTicksNS();
    s_sdl.perf_vertices_ns += perf_mark - perf_start;
    perf_start = perf_mark;

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(s_sdl.device);
    if (!commands) {
        fprintf(stderr, "[SDL_GPU] batch command acquisition failed: %s\n",
                SDL_GetError());
        ++s_sdl.errors;
        goto done;
    }
    SDL_GPURenderPass* active_pass = NULL;
    const rsx_render_op* active_attachments = NULL;
    for (unsigned i = 0; i < batch->operation_count; ++i) {
        const rsx_render_op* op = &batch->operations[i];
        if (op->type == RSX_RENDER_OP_CLEAR) {
            if (active_pass) {
                SDL_EndGPURenderPass(active_pass);
                active_pass = NULL;
                active_attachments = NULL;
            }
            execute_clear(commands, op);
            ++render_passes;
        } else if (op->type == RSX_RENDER_OP_DRAW) {
            int valid = get_pipeline(op) && vertices && vertex_offsets &&
                        vertex_offsets[i] != UINT32_MAX;
            if (!valid) {
                if (active_pass) {
                    SDL_EndGPURenderPass(active_pass);
                    active_pass = NULL;
                    active_attachments = NULL;
                }
                execute_diagnostic_fallback(commands, op);
                ++render_passes;
                continue;
            }
            if (!active_pass ||
                !same_draw_attachments(active_attachments, op)) {
                if (active_pass) SDL_EndGPURenderPass(active_pass);
                active_pass = begin_draw_pass(commands, op);
                active_attachments = active_pass ? op : NULL;
                if (active_pass) ++render_passes;
            }
            execute_draw(commands, active_pass, op, vertices,
                         vertex_offsets ? vertex_offsets[i] : UINT32_MAX,
                         vertex_constants,
                         i * RSX_BATCH_VP_CONSTANTS);
        }
    }
    if (active_pass) SDL_EndGPURenderPass(active_pass);
    submit_commands(commands);
done:
    perf_mark = SDL_GetTicksNS();
    s_sdl.perf_render_ns += perf_mark - perf_start;
    s_sdl.perf_render_passes += render_passes;
    perf_start = perf_mark;
    free(vertex_offsets);
    if (vertices)
        SDL_ReleaseGPUBuffer(s_sdl.device, vertices);
    if (vertex_constants)
        SDL_ReleaseGPUBuffer(s_sdl.device, vertex_constants);
    const Uint64 present_start_ns = perf_start;
    if (present_display() == 0)
        ++s_sdl.fps_window_frames;
    perf_mark = SDL_GetTicksNS();
    s_sdl.perf_present_ns += perf_mark - perf_start;
    trace_frame_pacing(enqueue_ns, execute_start_ns, present_start_ns, perf_mark);
    ++s_sdl.perf_batches;
    update_window_title();
}

static int consumer_submit(void* userdata, const rsx_render_batch* batch)
{
    (void)userdata;
    if (!batch || !s_sdl.queue_mutex) return -1;
    SDL_LockMutex(s_sdl.queue_mutex);
    while (s_sdl.queue_count == SDL_RSX_QUEUE_DEPTH && !s_sdl.stopping)
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
#ifndef RSX_SDL_REPLAY_STANDALONE
        if (event->key.down && event->key.scancode == SDL_SCANCODE_F10) {
            const char* path = getenv("RSX_BATCH_CAPTURE");
            const char* frames = getenv("RSX_BATCH_CAPTURE_FRAMES");
            rsx_recorder_arm_capture(path && path[0] ? path : "rsx_capture.rsxb",
                                     frames ? (u32)strtoul(frames, NULL, 0) : 1u);
            fprintf(stderr, "[SDL_GPU] F10 armed portable RSX capture\n");
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

static void drain_batches(void)
{
    if (!s_sdl.queue_mutex) return;
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
    }
    process_snapshot_request();
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
    s_sdl.window = SDL_CreateWindow(s_sdl.base_title,
                                    SDL_RSX_WIDTH, SDL_RSX_HEIGHT,
                                    SDL_WINDOW_RESIZABLE);
    if (!s_sdl.window) goto fail;
    const char* requested_driver = getenv("TAIKO_GPU_DRIVER");
    if (requested_driver && !requested_driver[0]) requested_driver = NULL;
    s_sdl.device = SDL_CreateGPUDevice(SDL_ShaderCross_GetHLSLShaderFormats(),
                                      getenv("SDL_GPU_DEBUG") != NULL,
                                      requested_driver);
    if (!s_sdl.device) goto fail;
    if (requested_driver)
        fprintf(stderr, "[SDL_GPU] driver: %s (requested %s)\n",
                SDL_GetGPUDeviceDriver(s_sdl.device), requested_driver);
    else
        fprintf(stderr, "[SDL_GPU] driver: %s (automatic)\n",
                SDL_GetGPUDeviceDriver(s_sdl.device));
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
    SDL_Event event;
    if (SDL_WaitEventTimeout(&event, timeout_ms)) {
        handle_event(&event);
        while (SDL_PollEvent(&event)) handle_event(&event);
    }
    drain_batches();
    return s_sdl.stopping;
}

int rsx_sdl_gpu_backend_queue_has_capacity(void)
{
    if (!s_sdl.queue_mutex) return 0;
    SDL_LockMutex(s_sdl.queue_mutex);
    int result = s_sdl.queue_count < SDL_RSX_QUEUE_DEPTH && !s_sdl.stopping;
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
        if (s_sdl.white_texture)
            SDL_ReleaseGPUTexture(s_sdl.device, s_sdl.white_texture);
        if (s_sdl.default_sampler)
            SDL_ReleaseGPUSampler(s_sdl.device, s_sdl.default_sampler);
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

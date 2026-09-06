/* Included by the SDL backend: renderer-owned resources, no title dependency. */
#include "rsx_host_ui.h"
HostUiVisit g_rsx_host_ui_visit;
enum { UI_TEXTURES = 256, UI_DRAWS = 512 };
static struct {
    SDL_GPUTexture* texture;
    uint64_t id, used;
    size_t bytes;
} ui_textures[UI_TEXTURES];
static uint64_t ui_clock;
static size_t ui_bytes;
static SDL_GPUGraphicsPipeline* ui_pipeline;
static SDL_GPUTextureFormat ui_format;
static SDL_GPUTexture* ui_white;
static HostUiInfo ui_info;
static int ui_active;
static const char* ui_vs =
    "cbuffer Params : register(b0, space1) { float4 rect; float4 colour; float4 shape; };\n"
    "struct Out { float4 pos:SV_Position; float2 uv:TEXCOORD0; };\n"
    "Out main(uint id:SV_VertexID) { Out o; float2 c=float2(id&1u,(id>>1u)&1u);"
    "o.pos=float4(rect.xy+c*rect.zw,0,1); o.uv=c; return o; }\n";
static const char* ui_fs =
    "Texture2D tex:register(t0,space2); SamplerState samp:register(s0,space2);\n"
    "cbuffer Params:register(b0,space3) { float4 rect; float4 colour; float4 shape; };\n"
    "float4 main(float4 pos:SV_Position,float2 uv:TEXCOORD0):SV_Target {"
    "float4 c=tex.Sample(samp,uv)*colour;"
    "if(shape.z>0) { float2 q=abs((uv-0.5)*shape.xy)-shape.xy*0.5+shape.z;"
    "float d=length(max(q,0))+min(max(q.x,q.y),0)-shape.z;"
    "c.a*=saturate(0.5-d/max(fwidth(d),0.0001)); } return c; }\n";

static int ui_pipeline_ready(SDL_GPUTextureFormat format)
{
    if (ui_pipeline && ui_format == format) return 1;
    if (ui_pipeline) SDL_ReleaseGPUGraphicsPipeline(s_sdl.device, ui_pipeline);
    ui_pipeline = NULL;
    SDL_ShaderCross_GraphicsShaderResourceInfo vr = {0}, fr = {0};
    SDL_GPUShader* vs = compile_hlsl(ui_vs, SDL_SHADERCROSS_SHADERSTAGE_VERTEX, &vr, 0);
    SDL_GPUShader* fs = compile_hlsl(ui_fs, SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT, &fr, 0);
    if (!vs || !fs) {
        if (vs) SDL_ReleaseGPUShader(s_sdl.device, vs);
        if (fs) SDL_ReleaseGPUShader(s_sdl.device, fs);
        return 0;
    }
    SDL_GPUColorTargetDescription target = {0};
    target.format = format;
    target.blend_state.enable_blend = true;
    target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    target.blend_state.color_write_mask = 15;
    SDL_GPUGraphicsPipelineCreateInfo p = {0};
    p.vertex_shader = vs; p.fragment_shader = fs;
    p.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    p.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    p.target_info.num_color_targets = 1;
    p.target_info.color_target_descriptions = &target;
    ui_pipeline = SDL_CreateGPUGraphicsPipeline(s_sdl.device, &p);
    SDL_ReleaseGPUShader(s_sdl.device, vs); SDL_ReleaseGPUShader(s_sdl.device, fs);
    ui_format = format;
    return ui_pipeline != NULL;
}

static SDL_GPUTexture* ui_upload(SDL_GPUCommandBuffer* cmd, const uint32_t* pixels,
                                  uint32_t width, uint32_t height)
{
    SDL_GPUTextureCreateInfo ti = {0};
    ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; ti.width = width; ti.height = height;
    ti.layer_count_or_depth = 1; ti.num_levels = 1; ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* tex = SDL_CreateGPUTexture(s_sdl.device, &ti);
    if (!tex) return NULL;
    SDL_GPUTransferBufferCreateInfo bi = {0};
    bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; bi.size = width * height * 4;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(s_sdl.device, &bi);
    void* mapped = transfer ? SDL_MapGPUTransferBuffer(s_sdl.device, transfer, false) : NULL;
    if (!mapped) {
        if (transfer) SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer);
        SDL_ReleaseGPUTexture(s_sdl.device, tex); return NULL;
    }
    memcpy(mapped, pixels, bi.size);
    SDL_UnmapGPUTransferBuffer(s_sdl.device, transfer);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    if (!copy) {
        SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer);
        SDL_ReleaseGPUTexture(s_sdl.device, tex); return NULL;
    }
    SDL_GPUTextureTransferInfo source = {0}; source.transfer_buffer = transfer;
    source.pixels_per_row = width; source.rows_per_layer = height;
    SDL_GPUTextureRegion region = {0}; region.texture = tex;
    region.w = width; region.h = height; region.d = 1;
    SDL_UploadToGPUTexture(copy, &source, &region, false);
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(s_sdl.device, transfer);
    return tex;
}

typedef struct UiBatch {
    SDL_GPUCommandBuffer* commands;
    HostUiDraw draws[UI_DRAWS];
    SDL_GPUTexture* textures[UI_DRAWS];
    unsigned count, uploads;
    int failed;
} UiBatch;
static void ui_emit(void* user, const HostUiDraw* draw)
{
    UiBatch* batch = user;
    if (batch->count == UI_DRAWS) { batch->failed = 1; return; }
    SDL_GPUTexture* tex = ui_white;
    if (draw->texture_id) {
        unsigned found = UI_TEXTURES, oldest = 0;
        for (unsigned i = 0; i < UI_TEXTURES; ++i) {
            if (ui_textures[i].id == draw->texture_id) { found = i; break; }
            if (ui_textures[i].used < ui_textures[oldest].used) oldest = i;
        }
        if (found == UI_TEXTURES) {
            // Never evict a texture referenced earlier in this frame.
            if (ui_textures[oldest].used == ui_clock) { batch->failed = 1; return; }
            if (ui_textures[oldest].texture) {
                SDL_ReleaseGPUTexture(s_sdl.device, ui_textures[oldest].texture);
                ui_bytes -= ui_textures[oldest].bytes;
            }
            SDL_zero(ui_textures[oldest]);
            tex = ui_upload(batch->commands, draw->pixels, draw->width, draw->height);
            if (!tex) { batch->failed = 1; return; }
            ui_textures[oldest].texture = tex;
            ui_textures[oldest].id = draw->texture_id;
            ui_textures[oldest].bytes = (size_t)draw->width * draw->height * 4;
            ui_bytes += ui_textures[oldest].bytes;
            ++batch->uploads;
            found = oldest;
        }
        ui_textures[found].used = ui_clock;
        tex = ui_textures[found].texture;
    }
    batch->draws[batch->count] = *draw;
    batch->textures[batch->count++] = tex;
}

static int draw_host_ui(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target,
                         Uint32 width, Uint32 height, Uint32 x, Uint32 y,
                         Uint32 draw_w, Uint32 draw_h, SDL_GPUTextureFormat format)
{
    if (!g_rsx_host_ui_visit || !ui_pipeline_ready(format) || !overlay_pipeline_ready(format)) return 0;
    if (!ui_white) { uint32_t white = 0xffffffffu; ui_white = ui_upload(cmd, &white, 1, 1); }
    if (!ui_white) return 0;
    Uint64 begin = SDL_GetTicksNS();
    UiBatch batch = {0}; batch.commands = cmd; ++ui_clock;
    const float scale = (float)draw_w / 1280.0f;
    if (!g_rsx_host_ui_visit(scale, ui_emit, &batch, &ui_info) || batch.failed) return 0;
    if (getenv("TAIKO_UI_TRACE")) {
        fprintf(stderr, "[HOST_UI] scale=%.3f draws=%u prepare=%.3fms uploads=%u bytes=%zu\n",
            scale, batch.count, (SDL_GetTicksNS()-begin)/1000000.0, batch.uploads, ui_bytes);
    }
    SDL_GPUColorTargetInfo t = {0}; t.texture = target;
    t.load_op = SDL_GPU_LOADOP_LOAD; t.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, NULL);
    if (!pass) return 0;
    SDL_BindGPUGraphicsPipeline(pass, ui_pipeline);
    SDL_Rect clip = {(int)x, (int)y, (int)draw_w, (int)draw_h};
    SDL_SetGPUScissor(pass, &clip);
    for (unsigned i = 0; i < batch.count; ++i) {
        const HostUiDraw* d = &batch.draws[i];
        float params[12] = {
            (x+d->x*scale)/width*2-1, 1-(y+d->y*scale)/height*2,
            d->w*scale/width*2, -d->h*scale/height*2,
            (d->colour&255)/255.0f, ((d->colour>>8)&255)/255.0f,
            ((d->colour>>16)&255)/255.0f, (d->colour>>24)/255.0f,
            d->w*scale, d->h*scale, d->radius*scale, 0
        };
        SDL_GPUTextureSamplerBinding binding = {batch.textures[i], s_overlay.sampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_PushGPUVertexUniformData(cmd, 0, params, sizeof(params));
        SDL_PushGPUFragmentUniformData(cmd, 0, params, sizeof(params));
        SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
    }
    SDL_EndGPURenderPass(pass);
    // Retain at most 64 MiB between frames. Current-frame references remain
    // alive until the next frame; SDL defers destruction of in-flight resources.
    for (unsigned i = 0; i < UI_TEXTURES && ui_bytes > 64u*1024u*1024u; ++i) {
        if (ui_textures[i].used == ui_clock) continue;
        if (ui_textures[i].texture) SDL_ReleaseGPUTexture(s_sdl.device, ui_textures[i].texture);
        ui_bytes -= ui_textures[i].bytes; SDL_zero(ui_textures[i]);
    }
    return 1;
}

static Uint64 host_ui_interval(void)
{
    float hz = 60;
    if (s_sdl.window) {
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(s_sdl.window));
        if (mode && mode->refresh_rate > 0) hz = mode->refresh_rate;
    }
    return (Uint64)(1000000000.0 / hz);
}
static void shutdown_host_ui(void)
{
    for (unsigned i = 0; i < UI_TEXTURES; ++i) {
        if (ui_textures[i].texture) SDL_ReleaseGPUTexture(s_sdl.device, ui_textures[i].texture);
        SDL_zero(ui_textures[i]);
    }
    if (ui_white) SDL_ReleaseGPUTexture(s_sdl.device, ui_white);
    if (ui_pipeline) SDL_ReleaseGPUGraphicsPipeline(s_sdl.device, ui_pipeline);
    ui_white = NULL; ui_pipeline = NULL; ui_bytes = 0; ui_active = 0;
}

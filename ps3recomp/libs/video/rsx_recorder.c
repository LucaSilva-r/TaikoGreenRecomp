#include "rsx_recorder.h"

#include "cellGcmSys.h"
#include "rsx_fp_decompiler.h"
#include "rsx_batch_io.h"
#include "ps3emu/host_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

extern u8* vm_base;
extern u32 cellGcmResolveLocated(int local, u32 offset);
extern u32 cellGcmResolveOffset(u32 offset);
extern int cellGcmOffsetIsDisplay(u32 offset);

#define RECORDER_TEXTURE_CACHE_SIZE 256u
#define RECORDER_TEXTURE_CACHE_BYTES (UINT64_C(256) * 1024u * 1024u)
#define RECORDER_VERTEX_CACHE_SIZE 256u

typedef struct recorder_texture_cache_entry {
    rsx_texture_state state;
    rsx_texture_source source;
    u64 source_hash;
    u64 validated_serial;
    u64 last_used_serial;
} recorder_texture_cache_entry;

typedef struct recorder_vertex_key {
    u32 primitive;
    u32 first;
    u32 count;
    u32 indexed;
    u32 vertex_data_base_offset;
    u32 vertex_data_base_index;
    u32 index_array_offset;
    u32 index_array_dma;
    u32 restart_index_enable;
    u32 restart_index;
    rsx_vertex_attrib attributes[RSX_MAX_VERTEX_ATTRIBS];
} recorder_vertex_key;

typedef struct recorder_vertex_cache_entry {
    recorder_vertex_key key;
    rsx_owned_blob data;
    u32 vertex_count;
    rsx_portable_topology topology;
    rsx_vertex_layout layout;
} recorder_vertex_cache_entry;

typedef struct recorder_state {
    rsx_backend backend;
    rsx_backend* legacy;
    const rsx_render_backend_ops* consumer;
    void* consumer_userdata;
    rsx_render_batch batch;
    rsx_state state;
    rsx_texture_state textures[RSX_BATCH_MAX_TEXTURES];
    recorder_texture_cache_entry texture_cache[RECORDER_TEXTURE_CACHE_SIZE];
    u32 texture_cache_count;
    u64 texture_cache_bytes;
    recorder_vertex_cache_entry vertex_cache[RECORDER_VERTEX_CACHE_SIZE];
    u32 vertex_cache_count;
    u64 prof_last_ns;
    u64 prof_batches;
    u64 prof_draws;
    u64 prof_vertex_ns;
    u64 prof_constants_ns;
    u64 prof_shaders_ns;
    u64 prof_textures_ns;
    u64 prof_submit_ns;
    u64 prof_texture_bytes;
    u64 prof_texture_hash_bytes;
    u64 prof_texture_revalidations;
    u64 prof_texture_hits;
    u64 prof_texture_misses;
    u64 prof_surface_skips;
    int profiling;
    u64 next_serial;
    u32 gated_flips;
    int spurs_drained_for_batch;
    int batch_has_display_clear;
    int seen_content;
    char capture_path[1024];
    u32 capture_target;
    u32 capture_count;
    int capture_stage;
    atomic_int capture_request;
    atomic_uint debug_draw_seq;
    rsx_render_batch capture_seed;
    rsx_render_batch* captures;
    int installed;
} recorder_state;

static recorder_state s_recorder;

/* Compatibility probes used by title-side diagnostics. They now describe the
 * portable recorder instead of a renderer-specific D3D12 command list. */
u32 rsx_dbg_draw_seq(void)
{
    return atomic_load_explicit(&s_recorder.debug_draw_seq,
                                memory_order_relaxed);
}

int rsx_dbg_trace_armed(void)
{
    return s_recorder.capture_stage ||
           atomic_load_explicit(&s_recorder.capture_request,
                                memory_order_relaxed);
}

int rsx_dbg_capture_left(void)
{
    if (!s_recorder.capture_stage ||
        s_recorder.capture_count >= s_recorder.capture_target)
        return 0;
    return (int)(s_recorder.capture_target - s_recorder.capture_count);
}

static void clear_texture_cache(void)
{
    for (u32 i = 0; i < s_recorder.texture_cache_count; ++i)
        rsx_owned_blob_destroy(&s_recorder.texture_cache[i].source.payload);
    s_recorder.texture_cache_count = 0;
    s_recorder.texture_cache_bytes = 0;
}

static void evict_texture_cache_entry(u32 index)
{
    if (index >= s_recorder.texture_cache_count) return;
    recorder_texture_cache_entry* entry = &s_recorder.texture_cache[index];
    if (entry->source.payload.size <= s_recorder.texture_cache_bytes)
        s_recorder.texture_cache_bytes -= entry->source.payload.size;
    else
        s_recorder.texture_cache_bytes = 0;
    rsx_owned_blob_destroy(&entry->source.payload);
    u32 last = --s_recorder.texture_cache_count;
    if (index != last) *entry = s_recorder.texture_cache[last];
    memset(&s_recorder.texture_cache[last], 0,
           sizeof(s_recorder.texture_cache[last]));
}

static void clear_vertex_cache(void)
{
    for (u32 i = 0; i < s_recorder.vertex_cache_count; ++i)
        rsx_owned_blob_destroy(&s_recorder.vertex_cache[i].data);
    s_recorder.vertex_cache_count = 0;
}

static u32 selected_surface_index(const rsx_state* state)
{
    return state && state->color_target == CELL_GCM_SURFACE_TARGET_1 ? 1u : 0u;
}

static u32 color_target_count(u32 target)
{
    switch (target) {
    case CELL_GCM_SURFACE_TARGET_NONE: return 0;
    case CELL_GCM_SURFACE_TARGET_MRT1: return 2;
    case CELL_GCM_SURFACE_TARGET_MRT2: return 3;
    case CELL_GCM_SURFACE_TARGET_MRT3: return 4;
    default: return 1;
    }
}

static u32 display_id_for_offset(u32 raw)
{
    for (u32 i = 0; i < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; ++i) {
        CellGcmDisplayInfo* info = cellGcmGetDisplayBufferByFlipIndex(i);
        if (info && info->offset == raw && info->width && info->height) return i;
    }
    return UINT32_MAX;
}

static rsx_surface_ref make_color_surface(const rsx_state* state, u32 index)
{
    rsx_surface_ref surface;
    memset(&surface, 0, sizeof(surface));
    if (!state || index >= RSX_MAX_RENDER_TARGETS) return surface;
    surface.raw_offset = state->surface_color_offset[index];
    surface.width = state->surface_clip_w;
    surface.height = state->surface_clip_h;
    surface.pitch = state->surface_color_pitch[index];
    surface.format = rsx_surface_format_from_gcm(state->surface_format);
    surface.is_display = cellGcmOffsetIsDisplay(surface.raw_offset) ? 1u : 0u;
    surface.display_buffer_id = surface.is_display
        ? display_id_for_offset(surface.raw_offset) : UINT32_MAX;
    surface.resolved_offset = cellGcmResolveOffset(surface.raw_offset);
    return surface;
}

static void fill_surfaces(rsx_render_op* op, const rsx_state* state)
{
    if (!op || !state) return;
    op->color_count = color_target_count(state->color_target);
    u32 first = selected_surface_index(state);
    for (u32 i = 0; i < op->color_count && i < 4; ++i)
        op->color[i] = make_color_surface(state, first + i);
    op->depth.raw_offset = state->surface_zeta_offset;
    op->depth.resolved_offset = state->surface_zeta_offset
        ? cellGcmResolveOffset(state->surface_zeta_offset) : 0;
    op->depth.width = state->surface_clip_w;
    op->depth.height = state->surface_clip_h;
    op->depth.pitch = state->surface_zeta_pitch;
    op->depth.format = RSX_FORMAT_D24S8;
    op->depth.is_depth = 1;
    op->viewport[0] = state->viewport_x; op->viewport[1] = state->viewport_y;
    op->viewport[2] = state->viewport_w; op->viewport[3] = state->viewport_h;
    op->scissor[0] = state->scissor_x; op->scissor[1] = state->scissor_y;
    op->scissor[2] = state->scissor_w; op->scissor[3] = state->scissor_h;
}

static int batch_has_display_draw(const rsx_render_batch* batch)
{
    if (!batch) return 0;
    for (u32 i = 0; i < batch->operation_count; ++i) {
        const rsx_render_op* op = &batch->operations[i];
        if (op->type == RSX_RENDER_OP_DRAW && op->color_count &&
            op->color[0].is_display)
            return 1;
    }
    return 0;
}

static void discard_capture(void)
{
    if (s_recorder.captures) {
        for (u32 i = 0; i < s_recorder.capture_count; ++i)
            rsx_render_batch_destroy(&s_recorder.captures[i]);
        free(s_recorder.captures);
    }
    s_recorder.captures = NULL;
    s_recorder.capture_count = 0;
    s_recorder.capture_stage = 0;
    rsx_render_batch_destroy(&s_recorder.capture_seed);
}

static void begin_capture_request(void)
{
    if (!atomic_exchange_explicit(&s_recorder.capture_request, 0,
                                  memory_order_acquire)) return;
    discard_capture();
    if (!s_recorder.capture_target) s_recorder.capture_target = 1;
    if (s_recorder.capture_target > 120) s_recorder.capture_target = 120;
    s_recorder.captures = (rsx_render_batch*)calloc(
        s_recorder.capture_target, sizeof(*s_recorder.captures));
    if (!s_recorder.captures) {
        fprintf(stderr, "[RSXB] capture allocation failed\n");
        return;
    }
    rsx_render_batch_init(&s_recorder.capture_seed, 0);
    s_recorder.capture_stage = 1;
    fprintf(stderr, "[RSXB] armed %u frame(s) -> %s\n",
            s_recorder.capture_target, s_recorder.capture_path);
}

static int append_seed_surfaces(rsx_render_batch* destination)
{
    for (u32 i = 0; i < s_recorder.capture_seed.surface_init_count; ++i) {
        const rsx_surface_init* input = &s_recorder.capture_seed.surface_inits[i];
        rsx_surface_init* output = rsx_render_batch_add_surface_init(destination);
        if (!output) return -1;
        output->surface = input->surface;
        if (rsx_owned_blob_copy(&output->color_data, input->color_data.data,
                                input->color_data.size) != 0 ||
            rsx_owned_blob_copy(&output->depth_stencil_data,
                                input->depth_stencil_data.data,
                                input->depth_stencil_data.size) != 0)
            return -1;
    }
    return 0;
}

static void capture_submitted_batch(const rsx_render_batch* batch)
{
    begin_capture_request();
    if (!s_recorder.capture_stage || !batch_has_display_draw(batch)) return;
    if (s_recorder.capture_stage == 1) {
        /* This completed visible batch is the safe boundary. The consumer has
         * already encoded it; snapshot persistent surfaces now, then record
         * the following complete batch. */
        if (s_recorder.consumer && s_recorder.consumer->snapshot_surfaces &&
            s_recorder.consumer->snapshot_surfaces(s_recorder.consumer_userdata,
                                                   &s_recorder.capture_seed) != 0) {
            fprintf(stderr, "[RSXB] persistent-surface snapshot failed\n");
            discard_capture();
            return;
        }
        s_recorder.capture_stage = 2;
        return;
    }
    rsx_render_batch* output = &s_recorder.captures[s_recorder.capture_count];
    if (rsx_render_batch_clone(output, batch) != 0 ||
        (s_recorder.capture_count == 0 && append_seed_surfaces(output) != 0)) {
        fprintf(stderr, "[RSXB] immutable batch copy failed\n");
        discard_capture();
        return;
    }
    if (++s_recorder.capture_count < s_recorder.capture_target) return;
    char error[256] = {0};
    if (rsxb_write_file(s_recorder.capture_path, s_recorder.captures,
                        s_recorder.capture_count, error, sizeof(error)) != 0)
        fprintf(stderr, "[RSXB] capture failed: %s\n", error);
    else
        fprintf(stderr, "[RSXB] wrote %u frame(s) to %s\n",
                s_recorder.capture_count, s_recorder.capture_path);
    discard_capture();
}

static int submit_batch(u32 display_id, int allow_empty)
{
    rsx_render_batch* batch = &s_recorder.batch;
    if (!allow_empty && batch->operation_count == 0) return 0;
    batch->display_buffer_id = display_id;
    u64 submit_start = s_recorder.profiling ? ps3_host_monotonic_ns() : 0;
    int result = 0;
    if (s_recorder.consumer && s_recorder.consumer->submit_batch)
        result = s_recorder.consumer->submit_batch(s_recorder.consumer_userdata,
                                                    batch);
    if (s_recorder.profiling) {
        u64 now = ps3_host_monotonic_ns();
        s_recorder.prof_submit_ns += now - submit_start;
        ++s_recorder.prof_batches;
        if (!s_recorder.prof_last_ns) s_recorder.prof_last_ns = now;
        if (now - s_recorder.prof_last_ns >= UINT64_C(1000000000)) {
            double batches = s_recorder.prof_batches
                ? (double)s_recorder.prof_batches : 1.0;
            fprintf(stderr,
                    "[RSXREC] batches=%llu draws=%.1f "
                    "cpu_ms(vertex=%.2f const=%.2f shader=%.2f tex=%.2f submit=%.2f) "
                    "tex_mb=%.2f hash_mb=%.2f revalidations=%llu "
                    "hits=%llu misses=%llu surfaces=%llu\n",
                    (unsigned long long)s_recorder.prof_batches,
                    (double)s_recorder.prof_draws / batches,
                    (double)s_recorder.prof_vertex_ns / batches / 1000000.0,
                    (double)s_recorder.prof_constants_ns / batches / 1000000.0,
                    (double)s_recorder.prof_shaders_ns / batches / 1000000.0,
                    (double)s_recorder.prof_textures_ns / batches / 1000000.0,
                    (double)s_recorder.prof_submit_ns / batches / 1000000.0,
                    (double)s_recorder.prof_texture_bytes / batches /
                        (1024.0 * 1024.0),
                    (double)s_recorder.prof_texture_hash_bytes / batches /
                        (1024.0 * 1024.0),
                    (unsigned long long)s_recorder.prof_texture_revalidations,
                    (unsigned long long)s_recorder.prof_texture_hits,
                    (unsigned long long)s_recorder.prof_texture_misses,
                    (unsigned long long)s_recorder.prof_surface_skips);
            s_recorder.prof_last_ns = now;
            s_recorder.prof_batches = s_recorder.prof_draws = 0;
            s_recorder.prof_vertex_ns = s_recorder.prof_constants_ns = 0;
            s_recorder.prof_shaders_ns = s_recorder.prof_textures_ns = 0;
            s_recorder.prof_submit_ns = s_recorder.prof_texture_bytes = 0;
            s_recorder.prof_texture_hash_bytes = 0;
            s_recorder.prof_texture_revalidations = 0;
            s_recorder.prof_texture_hits = s_recorder.prof_texture_misses = 0;
            s_recorder.prof_surface_skips = 0;
        }
    }
    if (result == 0) capture_submitted_batch(batch);
    if (batch_has_display_draw(batch)) s_recorder.seen_content = 1;
    /* Textures are usually immutable title assets and remain valid across
     * batches.  Keep their converted payloads; snapshot_texture_cached
     * fingerprints guest memory once per batch and refreshes an entry when
     * the title actually changes it.  Skinned vertices, on the other hand,
     * are written into reused SPURS arenas every frame and must never survive
     * this boundary. */
    clear_vertex_cache();
    s_recorder.spurs_drained_for_batch = 0;
    rsx_render_batch_reset(batch, ++s_recorder.next_serial);
    return result;
}

static u32 swizzled_index_2d(u32 x, u32 y, u32 w, u32 h)
{
    u32 log_w = 0, log_h = 0;
    for (u32 p = 1; p < w; p <<= 1) ++log_w;
    for (u32 p = 1; p < h; p <<= 1) ++log_h;
    u32 offset = 0, shift = 0;
    while (log_w || log_h) {
        if (log_w) { offset |= (x & 1u) << shift++; x >>= 1; --log_w; }
        if (log_h) { offset |= (y & 1u) << shift++; y >>= 1; --log_h; }
    }
    return offset;
}

static int texture_is_recorded_surface(const rsx_texture_source* texture)
{
    const rsx_render_batch* batch = &s_recorder.batch;
    for (u32 i = 0; i < batch->operation_count; ++i) {
        const rsx_render_op* op = &batch->operations[i];
        for (u32 target = 0; target < op->color_count && target < 4; ++target) {
            const rsx_surface_ref* surface = &op->color[target];
            if (!surface->is_depth &&
                surface->resolved_offset == texture->resolved_offset &&
                surface->width == texture->width &&
                surface->height == texture->height)
                return 1;
        }
    }
    return 0;
}

static int snapshot_texture(rsx_texture_source* out,
                            const rsx_texture_state* texture)
{
    memset(out, 0, sizeof(*out));
    if (!texture || !vm_base) return 0;
    u32 width = texture->image_rect >> 16;
    u32 height = texture->image_rect & 0xffffu;
    u32 rsx_format = (texture->format >> 8) & 0xffu;
    u32 base = rsx_format & 0x9fu;
    if (!width || !height || width > 16384 || height > 16384) return 0;
    out->raw_offset = texture->offset;
    out->resolved_offset = cellGcmResolveLocated((texture->format & 3u) == 1,
                                                 texture->offset);
    out->width = width; out->height = height;
    out->address = texture->address; out->control1 = texture->control1;
    out->border_color = texture->border_color;
    if (rsx_format & 0x40u)
        out->flags |= RSX_TEXTURE_FLAG_UNNORMALIZED_COORDS;
    u32 source_pitch = texture->control3 & 0xfffffu;
    u32 rows, row_bytes;
    if (base == 0x86 || base == 0x87 || base == 0x88) {
        out->format = base == 0x86 ? RSX_TEXTURE_BC1
                    : base == 0x87 ? RSX_TEXTURE_BC2 : RSX_TEXTURE_BC3;
        row_bytes = ((width + 3u) / 4u) * (base == 0x86 ? 8u : 16u);
        rows = (height + 3u) / 4u;
        out->pitch = row_bytes;
        u32 guest_pitch = ((rsx_format & 0x20u) && source_pitch >= row_bytes)
            ? source_pitch : row_bytes;
        u64 total = (u64)row_bytes * rows;
        if (texture_is_recorded_surface(out)) {
            ++s_recorder.prof_surface_skips;
            return 1;
        }
        if (total > SIZE_MAX ||
            rsx_owned_blob_allocate(&out->payload, total) != 0) return -1;
        u8* copy = out->payload.data;
        for (u32 y = 0; y < rows; ++y)
            memcpy(copy + (u64)y * row_bytes,
                   vm_base + out->resolved_offset + (u64)y * guest_pitch,
                   row_bytes);
        out->payload.hash = rsx_blob_hash64(copy, total);
        return 1;
    }
    if (base != 0x81 && base != 0x85 && base != 0x9e) return 0;
    int rgba = base != 0x81;
    out->format = rgba ? RSX_TEXTURE_RGBA8 : RSX_TEXTURE_R8;
    row_bytes = width * (rgba ? 4u : 1u); rows = height; out->pitch = row_bytes;
    u32 guest_pitch = ((rsx_format & 0x20u) && source_pitch >= row_bytes)
        ? source_pitch : row_bytes;
    u64 total = (u64)row_bytes * rows;
    if (texture_is_recorded_surface(out)) {
        ++s_recorder.prof_surface_skips;
        return 1;
    }
    if (total > SIZE_MAX ||
        rsx_owned_blob_allocate(&out->payload, total) != 0) return -1;
    u8* copy = out->payload.data;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            u64 source_index = (rsx_format & 0x20u)
                ? (u64)y * guest_pitch + x * (rgba ? 4u : 1u)
                : (u64)swizzled_index_2d(x, y, width, height) * (rgba ? 4u : 1u);
            const u8* source = vm_base + out->resolved_offset + source_index;
            u8* dest = copy + (u64)y * row_bytes + x * (rgba ? 4u : 1u);
            if (rgba) {
                dest[0] = source[1]; dest[1] = source[2]; dest[2] = source[3];
                dest[3] = base == 0x9e ? 0xffu : source[0];
            } else {
                dest[0] = source[0];
            }
        }
    }
    out->payload.hash = rsx_blob_hash64(copy, total);
    return 1;
}

static int same_texture_state(const rsx_texture_state* a,
                              const rsx_texture_state* b)
{
    return a->offset == b->offset && a->format == b->format &&
           a->address == b->address && a->control0 == b->control0 &&
           a->control1 == b->control1 && a->control3 == b->control3 &&
           a->filter == b->filter && a->image_rect == b->image_rect &&
           a->border_color == b->border_color;
}

static u64 texture_hash_bytes(u64 hash, const u8* bytes, u64 size)
{
    /* A private change fingerprint, not the serialized payload identity.
     * Mix whole words so validating a 4 MiB immutable atlas is much cheaper
     * than deswizzling it.  memcpy keeps unaligned guest addresses defined. */
    while (size >= sizeof(u64)) {
        u64 word;
        memcpy(&word, bytes, sizeof(word));
        hash ^= word + UINT64_C(0x9e3779b97f4a7c15) +
                (hash << 6) + (hash >> 2);
        bytes += sizeof(word);
        size -= sizeof(word);
    }
    if (size) {
        u64 tail = 0;
        memcpy(&tail, bytes, (size_t)size);
        hash ^= tail + UINT64_C(0x517cc1b727220a95) +
                (hash << 6) + (hash >> 2);
    }
    return hash;
}

/* Hash the guest bytes which feed snapshot_texture without doing its costly
 * per-texel deswizzle/conversion.  This makes immutable title textures cheap
 * after their first frame while preserving correctness for CPU-updated data.
 * Render-target textures deliberately bypass this cache: their authoritative
 * content already lives in the consumer's persistent GPU surface. */
static int texture_source_hash(const rsx_texture_state* texture, u64* result,
                               u64* byte_count)
{
    if (!texture || !result || !byte_count || !vm_base) return 0;
    u32 width = texture->image_rect >> 16;
    u32 height = texture->image_rect & 0xffffu;
    u32 rsx_format = (texture->format >> 8) & 0xffu;
    u32 base = rsx_format & 0x9fu;
    if (!width || !height || width > 16384 || height > 16384) return 0;

    rsx_texture_source probe;
    memset(&probe, 0, sizeof(probe));
    probe.resolved_offset = cellGcmResolveLocated((texture->format & 3u) == 1,
                                                  texture->offset);
    probe.width = width;
    probe.height = height;
    if (texture_is_recorded_surface(&probe)) return 0;

    u32 rows, row_bytes;
    if (base == 0x86 || base == 0x87 || base == 0x88) {
        row_bytes = ((width + 3u) / 4u) * (base == 0x86 ? 8u : 16u);
        rows = (height + 3u) / 4u;
    } else if (base == 0x81 || base == 0x85 || base == 0x9e) {
        row_bytes = width * (base == 0x81 ? 1u : 4u);
        rows = height;
    } else {
        return 0;
    }

    u32 source_pitch = texture->control3 & 0xfffffu;
    u32 guest_pitch = ((rsx_format & 0x20u) && source_pitch >= row_bytes)
        ? source_pitch : row_bytes;
    u64 hash = UINT64_C(1469598103934665603);
    const u8* source = vm_base + probe.resolved_offset;
    if (!(rsx_format & 0x20u) || guest_pitch == row_bytes) {
        u64 bytes = (u64)row_bytes * rows;
        hash = texture_hash_bytes(hash, source, bytes);
        *byte_count = bytes;
    } else {
        for (u32 y = 0; y < rows; ++y)
            hash = texture_hash_bytes(hash, source + (u64)y * guest_pitch,
                                      row_bytes);
        *byte_count = (u64)row_bytes * rows;
    }
    *result = hash;
    return 1;
}

static int snapshot_texture_cached(rsx_texture_source* out,
                                   const rsx_texture_state* texture)
{
    recorder_texture_cache_entry* match = NULL;
    for (u32 i = 0; i < s_recorder.texture_cache_count; ++i) {
        recorder_texture_cache_entry* entry = &s_recorder.texture_cache[i];
        if (!same_texture_state(&entry->state, texture)) continue;
        match = entry;
        break;
    }

    u64 source_hash = 0, hash_bytes = 0;
    if (match && match->validated_serial == s_recorder.batch.serial) {
        ++s_recorder.prof_texture_hits;
        match->last_used_serial = s_recorder.batch.serial;
        *out = match->source;
        memset(&out->payload, 0, sizeof(out->payload));
        return rsx_owned_blob_share(&out->payload, &match->source.payload) == 0
            ? 1 : -1;
    }
    int cacheable = texture_source_hash(texture, &source_hash, &hash_bytes);
    if (cacheable) {
        s_recorder.prof_texture_hash_bytes += hash_bytes;
        ++s_recorder.prof_texture_revalidations;
    }
    if (match && cacheable && match->source_hash == source_hash) {
        ++s_recorder.prof_texture_hits;
        match->validated_serial = s_recorder.batch.serial;
        match->last_used_serial = s_recorder.batch.serial;
        *out = match->source;
        memset(&out->payload, 0, sizeof(out->payload));
        return rsx_owned_blob_share(&out->payload, &match->source.payload) == 0
            ? 1 : -1;
    }

    ++s_recorder.prof_texture_misses;
    int result = snapshot_texture(out, texture);
    if (result > 0) s_recorder.prof_texture_bytes += out->payload.size;
    if (result > 0 && cacheable && out->payload.size) {
        recorder_texture_cache_entry* entry = match;
        int appended = 0;
        if (!entry &&
            s_recorder.texture_cache_count < RECORDER_TEXTURE_CACHE_SIZE) {
            entry = &s_recorder.texture_cache[s_recorder.texture_cache_count++];
            appended = 1;
        }
        if (!entry) {
            u32 oldest = 0;
            for (u32 i = 1; i < s_recorder.texture_cache_count; ++i)
                if (s_recorder.texture_cache[i].last_used_serial <
                    s_recorder.texture_cache[oldest].last_used_serial)
                    oldest = i;
            entry = &s_recorder.texture_cache[oldest];
        }
        if (entry->source.payload.size <= s_recorder.texture_cache_bytes)
            s_recorder.texture_cache_bytes -= entry->source.payload.size;
        rsx_owned_blob_destroy(&entry->source.payload);
        memset(entry, 0, sizeof(*entry));
        entry->state = *texture;
        entry->source = *out;
        entry->source_hash = source_hash;
        entry->validated_serial = s_recorder.batch.serial;
        entry->last_used_serial = s_recorder.batch.serial;
        memset(&entry->source.payload, 0, sizeof(entry->source.payload));
        if (rsx_owned_blob_share(&entry->source.payload, &out->payload) != 0) {
            if (appended) --s_recorder.texture_cache_count;
            memset(entry, 0, sizeof(*entry));
            return -1;
        }
        s_recorder.texture_cache_bytes += entry->source.payload.size;
        while (s_recorder.texture_cache_bytes > RECORDER_TEXTURE_CACHE_BYTES &&
               s_recorder.texture_cache_count) {
            u32 oldest = 0;
            for (u32 i = 1; i < s_recorder.texture_cache_count; ++i)
                if (s_recorder.texture_cache[i].last_used_serial <
                    s_recorder.texture_cache[oldest].last_used_serial)
                    oldest = i;
            evict_texture_cache_entry(oldest);
        }
    }
    return result;
}

static float read_be_float(const u8* source)
{
    u32 word = ((u32)source[0] << 24) | ((u32)source[1] << 16) |
               ((u32)source[2] << 8) | source[3];
    float value;
    memcpy(&value, &word, sizeof(value));
    return value;
}

static float read_be_half(const u8* source)
{
    u32 half = ((u32)source[0] << 8) | source[1];
    u32 sign = half >> 15, exponent = (half >> 10) & 31u, mantissa = half & 1023u;
    u32 word;
    if (exponent == 0) word = sign << 31;
    else if (exponent == 31) word = (sign << 31) | 0x7f800000u | (mantissa << 13);
    else word = (sign << 31) | ((exponent + 112u) << 23) | (mantissa << 13);
    float value; memcpy(&value, &word, sizeof(value)); return value;
}

static u32 resolve_vertex(const rsx_state* state, u32 encoded, u32 byte_offset)
{
    int local = (encoded >> 31) == 0;
    u32 offset = ((state->vertex_data_base_offset + (encoded & 0x7fffffffu))
                  & 0x0fffffffu) + byte_offset;
    return cellGcmResolveLocated(local, offset);
}

/* Every vertex is expanded to all sixteen attribute slots because that is the
 * layout the translated vertex shaders read. Walking all sixteen per vertex to
 * re-test `enabled` and re-store the (0,0,0,1) default was the hottest function
 * in the whole process (10.1% of cycles): a sprite uses two or three slots, and
 * the draw's attribute set cannot change between its own vertices. Collect the
 * live slots once per draw, then per vertex copy the default block and fill
 * only those. */
typedef struct vertex_plan_slot {
    u32 slot;
    u32 offset;
    u32 stride;
    u32 type;
    u32 lanes;
} vertex_plan_slot;

typedef struct vertex_plan {
    vertex_plan_slot slots[16];
    u32 count;
} vertex_plan;

static const float k_vertex_defaults[16][4] = {
    {0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},
    {0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},
    {0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},
    {0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},
};

static void build_vertex_plan(const rsx_state* state, vertex_plan* plan)
{
    plan->count = 0;
    for (u32 attribute = 0; attribute < 16; ++attribute) {
        const rsx_vertex_attrib* input = &state->vertex_attribs[attribute];
        if (!input->enabled || !input->stride) continue;
        vertex_plan_slot* out = &plan->slots[plan->count++];
        u32 lanes = input->size ? input->size : 4u;
        out->slot = attribute;
        out->offset = input->offset;
        out->stride = input->stride;
        out->type = input->type;
        out->lanes = lanes > 4 ? 4u : lanes;
    }
}

static void read_vertex_planned(const rsx_state* state, const vertex_plan* plan,
                                u32 vertex, float output[16][4])
{
    memcpy(output, k_vertex_defaults, sizeof(k_vertex_defaults));
    for (u32 i = 0; i < plan->count; ++i) {
        const vertex_plan_slot* input = &plan->slots[i];
        float* out = output[input->slot];
        const u8* source = vm_base + resolve_vertex(state, input->offset,
                                                    vertex * input->stride);
        const u32 lanes = input->lanes;
        for (u32 lane = 0; lane < lanes; ++lane) {
            switch (input->type) {
            case 1: {
                int value = (int)(short)(((u32)source[lane * 2] << 8) |
                                         source[lane * 2 + 1]);
                out[lane] = (float)value / 32767.0f; break;
            }
            case 2: out[lane] = read_be_float(source + lane * 4); break;
            case 3: out[lane] = read_be_half(source + lane * 2); break;
            case 4: out[lane] = source[lane] / 255.0f; break;
            case 5: out[lane] = (float)(short)(((u32)source[lane * 2] << 8) |
                                               source[lane * 2 + 1]); break;
            case 7: out[lane] = (float)source[lane]; break;
            default: out[lane] = read_be_float(source + lane * 4); break;
            }
        }
    }
}

typedef struct fallback_vertex {
    float position[3];
    float color[4];
    float texcoord[2];
} fallback_vertex;

static u32 read_index(const rsx_state* state, u32 index)
{
    int local = (state->index_array_dma & 0xfu) == 0;
    u32 address = cellGcmResolveLocated(local, state->index_array_offset);
    const u8* source = vm_base + address;
    if (((state->index_array_dma >> 4) & 0xfu) == 1)
        return ((u32)source[index * 2] << 8) | source[index * 2 + 1];
    source += (u64)index * 4u;
    return ((u32)source[0] << 24) | ((u32)source[1] << 16) |
           ((u32)source[2] << 8) | source[3];
}

static void fill_pipeline(rsx_pipeline_key* key, const rsx_state* state,
                          rsx_portable_topology topology,
                          rsx_vertex_layout layout)
{
    memset(key, 0, sizeof(*key));
    key->topology = topology; key->vertex_layout = layout;
    key->color_target_count = color_target_count(state->color_target);
    for (u32 i = 0; i < key->color_target_count && i < 4; ++i)
        key->color_format[i] = rsx_surface_format_from_gcm(state->surface_format);
    key->depth_format = RSX_FORMAT_D24S8;
    key->color_write_mask = ((state->color_mask & 0x00010000u) ? 1u : 0u)
                          | ((state->color_mask & 0x00000100u) ? 2u : 0u)
                          | ((state->color_mask & 0x00000001u) ? 4u : 0u)
                          | ((state->color_mask & 0x01000000u) ? 8u : 0u);
    key->blend_sfactor = state->blend_sfactor; key->blend_dfactor = state->blend_dfactor;
    key->blend_equation = state->blend_equation; key->blend_color = state->blend_color;
    key->depth_func = state->depth_func;
    key->stencil_func = state->stencil_func; key->stencil_ref = state->stencil_ref;
    key->stencil_mask = state->stencil_mask; key->stencil_fail = state->stencil_op_fail;
    key->stencil_zfail = state->stencil_op_zfail; key->stencil_zpass = state->stencil_op_zpass;
    key->cull_face = state->cull_face; key->front_face = state->front_face;
    key->alpha_func = state->alpha_func; key->alpha_ref = state->alpha_ref;
    key->blend_enable = state->blend_enable; key->depth_test_enable = state->depth_test_enable;
    key->depth_write_enable = state->depth_mask; key->stencil_test_enable = state->stencil_test_enable;
    key->cull_enable = state->cull_face_enable; key->alpha_test_enable = state->alpha_test_enable;
    key->fragment_32bit_exports = (state->shader_control & 0x40u) != 0;
}

static int snapshot_draw(rsx_render_op* op, u32 primitive, u32 first, u32 count,
                         int indexed)
{
    u64 prof_start = s_recorder.profiling ? ps3_host_monotonic_ns() : 0;
    u64 prof_mark;
    const rsx_state* state = &s_recorder.state;
    recorder_vertex_key vertex_key;
    memset(&vertex_key, 0, sizeof(vertex_key));
    vertex_key.primitive = primitive;
    vertex_key.first = first;
    vertex_key.count = count;
    vertex_key.indexed = indexed ? 1u : 0u;
    vertex_key.vertex_data_base_offset = state->vertex_data_base_offset;
    vertex_key.vertex_data_base_index = state->vertex_data_base_index;
    vertex_key.index_array_offset = state->index_array_offset;
    vertex_key.index_array_dma = state->index_array_dma;
    vertex_key.restart_index_enable = state->restart_index_enable ? 1u : 0u;
    vertex_key.restart_index = state->restart_index;
    memcpy(vertex_key.attributes, state->vertex_attribs,
           sizeof(vertex_key.attributes));
    rsx_portable_topology topology = RSX_TOPOLOGY_TRIANGLE_LIST;
    rsx_vertex_layout layout = state->vp_ucode_bytes >= 16
        ? RSX_VERTEX_LAYOUT_FLOAT4_X16 : RSX_VERTEX_LAYOUT_FALLBACK_36;
    for (u32 i = 0; indexed && i < s_recorder.vertex_cache_count; ++i) {
        const recorder_vertex_cache_entry* entry = &s_recorder.vertex_cache[i];
        if (memcmp(&entry->key, &vertex_key, sizeof(vertex_key)) != 0) continue;
        if (rsx_owned_blob_share(&op->data.draw.vertex_data,
                                 &entry->data) != 0)
            return -1;
        op->data.draw.vertex_count = entry->vertex_count;
        topology = entry->topology;
        layout = entry->layout;
        fill_pipeline(&op->data.draw.pipeline, state, topology, layout);
        goto geometry_complete;
    }
    u32* source_indices = NULL;
    if (indexed) {
        source_indices = (u32*)malloc((size_t)count * sizeof(*source_indices));
        if (!source_indices) return -1;
        for (u32 i = 0; i < count; ++i)
            source_indices[i] = read_index(state, first + i);
    }
    u32 expanded_count = rsx_expand_primitive_indices_restart(
        primitive, first, count, source_indices,
        indexed && state->restart_index_enable, state->restart_index,
        NULL, 0, &topology);
    if (!expanded_count) { free(source_indices); return 0; }
    u32* expanded = (u32*)malloc((size_t)expanded_count * sizeof(*expanded));
    if (!expanded) { free(source_indices); return -1; }
    rsx_expand_primitive_indices_restart(
        primitive, first, count, source_indices,
        indexed && state->restart_index_enable, state->restart_index,
        expanded, expanded_count, &topology);
    free(source_indices);
    if (indexed)
        for (u32 i = 0; i < expanded_count; ++i)
            expanded[i] = (expanded[i] + state->vertex_data_base_index)
                        & 0x000fffffu;

    u32 stride = layout == RSX_VERTEX_LAYOUT_FLOAT4_X16
        ? 16u * 4u * sizeof(float) : sizeof(fallback_vertex);
    u64 vertex_bytes = (u64)expanded_count * stride;
    if (rsx_owned_blob_allocate(&op->data.draw.vertex_data,
                                vertex_bytes) != 0) {
        free(expanded);
        return -1;
    }
    u8* vertices = op->data.draw.vertex_data.data;
    vertex_plan plan;
    build_vertex_plan(state, &plan);
    for (u32 i = 0; i < expanded_count; ++i) {
        if (layout == RSX_VERTEX_LAYOUT_FLOAT4_X16) {
            read_vertex_planned(state, &plan, expanded[i],
                (float (*)[4])(vertices + (u64)i * stride));
        } else {
            float slots[16][4];
            fallback_vertex* out =
                (fallback_vertex*)(vertices + (u64)i * stride);
            read_vertex_planned(state, &plan, expanded[i], slots);
            out->position[0] = slots[0][0]; out->position[1] = slots[0][1];
            out->position[2] = slots[0][2];
            memcpy(out->color, slots[3], sizeof(out->color));
            out->texcoord[0] = slots[0][2]; out->texcoord[1] = slots[0][3];
        }
    }
    free(expanded);
    op->data.draw.vertex_count = expanded_count;
    fill_pipeline(&op->data.draw.pipeline, state, topology, layout);
    if (indexed && s_recorder.vertex_cache_count < RECORDER_VERTEX_CACHE_SIZE) {
        recorder_vertex_cache_entry* entry =
            &s_recorder.vertex_cache[s_recorder.vertex_cache_count++];
        entry->key = vertex_key;
        entry->vertex_count = expanded_count;
        entry->topology = topology;
        entry->layout = layout;
        if (rsx_owned_blob_share(&entry->data,
                                 &op->data.draw.vertex_data) != 0) {
            --s_recorder.vertex_cache_count;
            memset(entry, 0, sizeof(*entry));
            return -1;
        }
    }
geometry_complete:
    {
        static int trace_left = -1;
        if (trace_left < 0) {
            const char* setting = getenv("RSX_VERTEX_TRACE");
            trace_left = setting ? atoi(setting) : 0;
            if (setting && trace_left <= 0) trace_left = 120;
        }
        if (trace_left > 0 && op->data.draw.vertex_count >= 5000u) {
            --trace_left;
            fprintf(stderr,
                    "[RSX-VERTEX] verts=%u indexed=%d first=%u count=%u "
                    "base=%08X base_index=%u index=%08X dma=%08X",
                    op->data.draw.vertex_count, indexed, first, count,
                    state->vertex_data_base_offset,
                    state->vertex_data_base_index,
                    state->index_array_offset, state->index_array_dma);
            for (u32 attribute = 0; attribute < 16; ++attribute) {
                const rsx_vertex_attrib* input =
                    &state->vertex_attribs[attribute];
                if (!input->enabled || !input->stride) continue;
                fprintf(stderr, " a%u=%08X/raw%08X/s%u/t%u/n%u",
                        attribute, resolve_vertex(state, input->offset, 0),
                        input->offset, input->stride, input->type,
                        input->size);
            }
            fprintf(stderr, "\n");
        }
    }
    if (s_recorder.profiling) {
        prof_mark = ps3_host_monotonic_ns();
        s_recorder.prof_vertex_ns += prof_mark - prof_start;
        prof_start = prof_mark;
    }

    float constants[RSX_BATCH_VP_CONSTANTS][4];
    memcpy(constants, state->vertex_constants, sizeof(state->vertex_constants));
    /* rsx_vp_decompile's epilogue consumes a clip-space transform, not the
     * raw RSX window transform. D3D12 has always normalized x/y here because
     * the host viewport performs that mapping. Recording raw values such as
     * scale=(640,-360) and offset=(640,360) made SDL apply the viewport twice,
     * squeezing otherwise fullscreen geometry into the upper-right quarter.
     * Preserve only the z remap, matching the D3D12 behavioral oracle. */
    constants[512][0] = 1.0f;
    constants[512][1] = 1.0f;
    constants[512][2] = state->viewport_scale[2] != 0.0f
        ? state->viewport_scale[2] : 1.0f;
    constants[512][3] = 1.0f;
    constants[513][0] = 0.0f;
    constants[513][1] = 0.0f;
    constants[513][2] = state->viewport_scale[2] != 0.0f
        ? state->viewport_offset[2] : 0.0f;
    constants[513][3] = 0.0f;
    if (rsx_owned_blob_allocate(&op->data.draw.vertex_constants,
                                sizeof(constants)) != 0) return -1;
    memcpy(op->data.draw.vertex_constants.data, constants, sizeof(constants));
    if (s_recorder.profiling) {
        prof_mark = ps3_host_monotonic_ns();
        s_recorder.prof_constants_ns += prof_mark - prof_start;
        prof_start = prof_mark;
    }
    if (state->vp_ucode_bytes &&
        rsx_owned_blob_copy(&op->data.draw.vertex_shader, state->vp_ucode,
                            state->vp_ucode_bytes) != 0) return -1;
    op->data.draw.pipeline.vertex_shader_hash = op->data.draw.vertex_shader.hash;

    if (state->shader_program && vm_base) {
        u32 address = cellGcmResolveLocated((state->shader_program & 3u) == 1,
                                            state->shader_program & ~3u);
        u32 bytes = rsx_fp_program_size(vm_base + address, 4096);
        if (!bytes) bytes = 64;
        if (rsx_owned_blob_copy(&op->data.draw.fragment_shader,
                                vm_base + address, bytes) != 0) return -1;
        op->data.draw.pipeline.fragment_shader_hash =
            rsx_fp_program_structure_hash(
                op->data.draw.fragment_shader.data,
                (u32)op->data.draw.fragment_shader.size);
    }
    if (s_recorder.profiling) {
        prof_mark = ps3_host_monotonic_ns();
        s_recorder.prof_shaders_ns += prof_mark - prof_start;
        prof_start = prof_mark;
    }
    for (u32 unit = 0; unit < RSX_BATCH_MAX_TEXTURES; ++unit) {
        int result = snapshot_texture_cached(&op->data.draw.textures[unit],
                                             &s_recorder.textures[unit]);
        if (result < 0) return -1;
        if (result > 0) op->data.draw.texture_count = unit + 1;
    }
    if (s_recorder.profiling) {
        prof_mark = ps3_host_monotonic_ns();
        s_recorder.prof_textures_ns += prof_mark - prof_start;
        ++s_recorder.prof_draws;
    }
    return 1;
}

static int rec_init(void* userdata, u32 width, u32 height)
{
    (void)userdata;
    return s_recorder.legacy && s_recorder.legacy->init
        ? s_recorder.legacy->init(s_recorder.legacy->userdata, width, height) : 0;
}
static void rec_shutdown(void* userdata)
{
    (void)userdata;
    if (s_recorder.legacy && s_recorder.legacy->shutdown)
        s_recorder.legacy->shutdown(s_recorder.legacy->userdata);
}
static void rec_begin(void* userdata)
{
    (void)userdata;
    if (s_recorder.legacy && s_recorder.legacy->begin_frame)
        s_recorder.legacy->begin_frame(s_recorder.legacy->userdata);
}
static void rec_end(void* userdata)
{
    (void)userdata;
    if (s_recorder.legacy && s_recorder.legacy->end_frame)
        s_recorder.legacy->end_frame(s_recorder.legacy->userdata);
}

static void rec_present(void* userdata, u32 display_id)
{
    (void)userdata;
    if (s_recorder.batch_has_display_clear) {
        if (++s_recorder.gated_flips <= 3) goto legacy;
        s_recorder.batch_has_display_clear = 0;
        s_recorder.gated_flips = 0;
    }
    if (batch_has_display_draw(&s_recorder.batch) ||
        (s_recorder.batch.operation_count == 0 && !s_recorder.seen_content))
        submit_batch(display_id, 1);
legacy:
    if (s_recorder.legacy && s_recorder.legacy->present)
        s_recorder.legacy->present(s_recorder.legacy->userdata, display_id);
}

static void rec_clear(void* userdata, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)userdata;
    atomic_fetch_add_explicit(&s_recorder.debug_draw_seq, 1,
                              memory_order_relaxed);
    rsx_surface_ref target = make_color_surface(&s_recorder.state,
                                                selected_surface_index(&s_recorder.state));
    if (target.is_display && batch_has_display_draw(&s_recorder.batch))
        submit_batch(target.display_buffer_id, 0);
    rsx_render_op* op = rsx_render_batch_add_op(&s_recorder.batch,
                                                RSX_RENDER_OP_CLEAR);
    if (op) {
        fill_surfaces(op, &s_recorder.state);
        op->data.clear.flags = flags;
        op->data.clear.color[0] = ((color >> 16) & 255u) / 255.0f;
        op->data.clear.color[1] = ((color >> 8) & 255u) / 255.0f;
        op->data.clear.color[2] = (color & 255u) / 255.0f;
        op->data.clear.color[3] = ((color >> 24) & 255u) / 255.0f;
        op->data.clear.depth = depth; op->data.clear.stencil = stencil;
    }
    if (target.is_display) {
        s_recorder.batch_has_display_clear = 1;
        s_recorder.gated_flips = 0;
    }
    if (s_recorder.legacy && s_recorder.legacy->clear)
        s_recorder.legacy->clear(s_recorder.legacy->userdata, flags, color,
                                 depth, stencil);
}

#define FORWARD_STATE(name) \
static void rec_##name(void* userdata, const rsx_state* state) { \
    (void)userdata; \
    if (s_recorder.legacy && s_recorder.legacy->name) \
        s_recorder.legacy->name(s_recorder.legacy->userdata, state); \
}
/* CLEAR_SURFACE pushes attachment state through set_render_target without a
 * preceding sync_state (there need not be a BEGIN_END draw). Keep the
 * recorder's snapshot current here or the clear is recorded against the
 * previous pass's target. This used to erase Don-chan's completed 600x600 RT
 * when the guest was actually clearing the following composite RT. */
static void rec_set_render_target(void* userdata, const rsx_state* state)
{
    (void)userdata;
    if (state) s_recorder.state = *state;
    if (s_recorder.legacy && s_recorder.legacy->set_render_target)
        s_recorder.legacy->set_render_target(
            s_recorder.legacy->userdata, state);
}
FORWARD_STATE(set_viewport)
FORWARD_STATE(set_blend)
FORWARD_STATE(set_depth_stencil)
FORWARD_STATE(set_color_mask)
FORWARD_STATE(set_alpha_test)
FORWARD_STATE(set_shader)
FORWARD_STATE(set_vertex_attribs)

static void rec_sync_state(void* userdata, const rsx_state* state)
{
    (void)userdata;
    if (state) s_recorder.state = *state;
    if (s_recorder.legacy && s_recorder.legacy->sync_state)
        s_recorder.legacy->sync_state(s_recorder.legacy->userdata, state);
}

static void rec_bind_texture(void* userdata, u32 unit,
                             const rsx_texture_state* texture)
{
    (void)userdata;
    if (unit < RSX_BATCH_MAX_TEXTURES && texture)
        s_recorder.textures[unit] = *texture;
    if (s_recorder.legacy && s_recorder.legacy->bind_texture)
        s_recorder.legacy->bind_texture(s_recorder.legacy->userdata, unit, texture);
}

static void rec_draw_arrays(void* userdata, u32 primitive, u32 first, u32 count)
{
    (void)userdata;
    atomic_fetch_add_explicit(&s_recorder.debug_draw_seq, 1,
                              memory_order_relaxed);
    rsx_render_op* op = rsx_render_batch_add_op(&s_recorder.batch,
                                                RSX_RENDER_OP_DRAW);
    if (op) {
        fill_surfaces(op, &s_recorder.state);
        if (snapshot_draw(op, primitive, first, count, 0) <= 0)
            rsx_render_batch_remove_last_op(&s_recorder.batch);
    }
    if (s_recorder.legacy && s_recorder.legacy->draw_arrays)
        s_recorder.legacy->draw_arrays(s_recorder.legacy->userdata, primitive, first, count);
}

static void rec_draw_indexed(void* userdata, u32 primitive, u32 first, u32 count)
{
    (void)userdata;
    atomic_fetch_add_explicit(&s_recorder.debug_draw_seq, 1,
                              memory_order_relaxed);
    if (!s_recorder.spurs_drained_for_batch) {
        extern void (*g_spurs_job_chain_drain)(void);
        if (g_spurs_job_chain_drain) g_spurs_job_chain_drain();
        s_recorder.spurs_drained_for_batch = 1;
    }
    rsx_render_op* op = rsx_render_batch_add_op(&s_recorder.batch,
                                                RSX_RENDER_OP_DRAW);
    if (op) {
        fill_surfaces(op, &s_recorder.state);
        if (snapshot_draw(op, primitive, first, count, 1) <= 0)
            rsx_render_batch_remove_last_op(&s_recorder.batch);
    }
    if (s_recorder.legacy && s_recorder.legacy->draw_indexed)
        s_recorder.legacy->draw_indexed(s_recorder.legacy->userdata, primitive, first, count);
}

int rsx_recorder_install(rsx_backend* legacy,
                         const rsx_render_backend_ops* consumer,
                         void* consumer_userdata)
{
    if (s_recorder.installed) return -1;
    memset(&s_recorder, 0, sizeof(s_recorder));
    atomic_init(&s_recorder.capture_request, 0);
    s_recorder.legacy = legacy;
    s_recorder.consumer = consumer;
    s_recorder.consumer_userdata = consumer_userdata;
    s_recorder.next_serial = 1;
    s_recorder.profiling = getenv("RSX_FPS_LOG") != NULL;
    rsx_render_batch_init(&s_recorder.batch, s_recorder.next_serial);
    rsx_state_init(&s_recorder.state);
    s_recorder.backend.userdata = &s_recorder;
    s_recorder.backend.init = rec_init; s_recorder.backend.shutdown = rec_shutdown;
    s_recorder.backend.begin_frame = rec_begin; s_recorder.backend.end_frame = rec_end;
    s_recorder.backend.present = rec_present; s_recorder.backend.clear = rec_clear;
    s_recorder.backend.sync_state = rec_sync_state;
    s_recorder.backend.set_render_target = rec_set_render_target;
    s_recorder.backend.set_viewport = rec_set_viewport;
    s_recorder.backend.set_blend = rec_set_blend;
    s_recorder.backend.set_depth_stencil = rec_set_depth_stencil;
    s_recorder.backend.set_color_mask = rec_set_color_mask;
    s_recorder.backend.set_alpha_test = rec_set_alpha_test;
    s_recorder.backend.set_shader = rec_set_shader;
    s_recorder.backend.set_vertex_attribs = rec_set_vertex_attribs;
    s_recorder.backend.draw_arrays = rec_draw_arrays;
    s_recorder.backend.draw_indexed = rec_draw_indexed;
    s_recorder.backend.bind_texture = rec_bind_texture;
    s_recorder.installed = 1;
    {
        const char* path = getenv("RSX_BATCH_CAPTURE");
        if (path && path[0]) {
            const char* frames = getenv("RSX_BATCH_CAPTURE_FRAMES");
            rsx_recorder_arm_capture(path, frames ? (u32)strtoul(frames, NULL, 0) : 1u);
        }
    }
    rsx_set_backend(&s_recorder.backend);
    return 0;
}

void rsx_recorder_uninstall(void)
{
    if (!s_recorder.installed) return;
    rsx_set_backend(NULL);
    discard_capture();
    clear_texture_cache();
    rsx_render_batch_destroy(&s_recorder.batch);
    memset(&s_recorder, 0, sizeof(s_recorder));
}

const rsx_render_batch* rsx_recorder_pending_batch(void)
{
    return s_recorder.installed ? &s_recorder.batch : NULL;
}

int rsx_recorder_flush(u32 display_buffer_id, int allow_empty)
{
    if (!s_recorder.installed) return -1;
    return submit_batch(display_buffer_id, allow_empty);
}

void rsx_recorder_arm_capture(const char* path, u32 frame_count)
{
    if (!path || !path[0]) path = "rsx_capture.rsxb";
    snprintf(s_recorder.capture_path, sizeof(s_recorder.capture_path), "%s", path);
    s_recorder.capture_target = frame_count ? frame_count : 1u;
    atomic_store_explicit(&s_recorder.capture_request, 1, memory_order_release);
}

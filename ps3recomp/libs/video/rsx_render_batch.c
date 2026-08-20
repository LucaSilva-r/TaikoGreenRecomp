#include "rsx_render_batch.h"
#include "rsx_commands.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

u64 rsx_blob_hash64(const void* data, u64 size)
{
    const u8* bytes = (const u8*)data;
    u64 hash = UINT64_C(1469598103934665603);
    for (u64 i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

int rsx_owned_blob_allocate(rsx_owned_blob* blob, u64 size)
{
    if (!blob || size > SIZE_MAX - sizeof(u32)) return -1;
    rsx_owned_blob_destroy(blob);
    if (!size) return 0;
    blob->references = (u32*)malloc(sizeof(u32) + (size_t)size);
    if (!blob->references) return -1;
    *blob->references = 1;
    blob->data = (u8*)(blob->references + 1);
    blob->size = size;
    return 0;
}

int rsx_owned_blob_copy(rsx_owned_blob* blob, const void* data, u64 size)
{
    if (!blob || (size && !data)) return -1;
    if (rsx_owned_blob_allocate(blob, size) != 0) return -1;
    if (!size) return 0;
    memcpy(blob->data, data, (size_t)size);
    blob->hash = rsx_blob_hash64(data, size);
    return 0;
}

int rsx_owned_blob_share(rsx_owned_blob* destination,
                         const rsx_owned_blob* source)
{
    if (!destination || !source || destination == source) return -1;
    rsx_owned_blob_destroy(destination);
    if (!source->size) return 0;
    if (!source->data || !source->references) return -1;
    __atomic_add_fetch(source->references, 1u, __ATOMIC_RELAXED);
    *destination = *source;
    return 0;
}

void rsx_owned_blob_destroy(rsx_owned_blob* blob)
{
    if (!blob) return;
    if (blob->references) {
        if (__atomic_sub_fetch(blob->references, 1u, __ATOMIC_ACQ_REL) == 0)
            free(blob->references);
    } else {
        free(blob->data);
    }
    memset(blob, 0, sizeof(*blob));
}

void rsx_render_batch_init(rsx_render_batch* batch, u64 serial)
{
    if (!batch) return;
    memset(batch, 0, sizeof(*batch));
    batch->serial = serial;
}

static void destroy_op(rsx_render_op* op)
{
    if (!op || op->type != RSX_RENDER_OP_DRAW) return;
    for (u32 i = 0; i < RSX_BATCH_MAX_TEXTURES; ++i)
        rsx_owned_blob_destroy(&op->data.draw.textures[i].payload);
    rsx_owned_blob_destroy(&op->data.draw.vertex_data);
    rsx_owned_blob_destroy(&op->data.draw.index_data);
    rsx_owned_blob_destroy(&op->data.draw.vertex_constants);
    rsx_owned_blob_destroy(&op->data.draw.vertex_shader);
    rsx_owned_blob_destroy(&op->data.draw.fragment_shader);
}

void rsx_render_batch_destroy(rsx_render_batch* batch)
{
    if (!batch) return;
    for (u32 i = 0; i < batch->operation_count; ++i)
        destroy_op(&batch->operations[i]);
    for (u32 i = 0; i < batch->surface_init_count; ++i) {
        rsx_owned_blob_destroy(&batch->surface_inits[i].color_data);
        rsx_owned_blob_destroy(&batch->surface_inits[i].depth_stencil_data);
    }
    free(batch->operations);
    free(batch->surface_inits);
    memset(batch, 0, sizeof(*batch));
}

void rsx_render_batch_reset(rsx_render_batch* batch, u64 next_serial)
{
    rsx_render_batch_destroy(batch);
    rsx_render_batch_init(batch, next_serial);
}

static int clone_blob(rsx_owned_blob* destination, const rsx_owned_blob* source,
                      int shared)
{
    return shared ? rsx_owned_blob_share(destination, source)
                  : rsx_owned_blob_copy(destination, source->data, source->size);
}

static int clone_batch(rsx_render_batch* destination,
                       const rsx_render_batch* source, int shared)
{
    if (!destination || !source || destination == source) return -1;
    rsx_render_batch_init(destination, source->serial);
    destination->display_buffer_id = source->display_buffer_id;
    destination->flags = source->flags;
    for (u32 i = 0; i < source->operation_count; ++i) {
        const rsx_render_op* input = &source->operations[i];
        rsx_render_op* output = rsx_render_batch_add_op(destination, input->type);
        if (!output) goto fail;
        *output = *input;
        if (input->type == RSX_RENDER_OP_DRAW) {
            for (u32 unit = 0; unit < RSX_BATCH_MAX_TEXTURES; ++unit)
                memset(&output->data.draw.textures[unit].payload, 0,
                       sizeof(output->data.draw.textures[unit].payload));
            memset(&output->data.draw.vertex_data, 0, sizeof(rsx_owned_blob) * 5u);
            for (u32 unit = 0; unit < RSX_BATCH_MAX_TEXTURES; ++unit)
                if (clone_blob(&output->data.draw.textures[unit].payload,
                               &input->data.draw.textures[unit].payload,
                               shared) != 0)
                    goto fail;
            if (clone_blob(&output->data.draw.vertex_data, &input->data.draw.vertex_data, shared) != 0 ||
                clone_blob(&output->data.draw.index_data, &input->data.draw.index_data, shared) != 0 ||
                clone_blob(&output->data.draw.vertex_constants, &input->data.draw.vertex_constants, shared) != 0 ||
                clone_blob(&output->data.draw.vertex_shader, &input->data.draw.vertex_shader, shared) != 0 ||
                clone_blob(&output->data.draw.fragment_shader, &input->data.draw.fragment_shader, shared) != 0)
                goto fail;
        }
    }
    for (u32 i = 0; i < source->surface_init_count; ++i) {
        const rsx_surface_init* input = &source->surface_inits[i];
        rsx_surface_init* output = rsx_render_batch_add_surface_init(destination);
        if (!output) goto fail;
        output->surface = input->surface;
        if (clone_blob(&output->color_data, &input->color_data, shared) != 0 ||
            clone_blob(&output->depth_stencil_data,
                       &input->depth_stencil_data, shared) != 0)
            goto fail;
    }
    return 0;
fail:
    rsx_render_batch_destroy(destination);
    return -1;
}

int rsx_render_batch_clone(rsx_render_batch* destination,
                           const rsx_render_batch* source)
{
    return clone_batch(destination, source, 0);
}

int rsx_render_batch_clone_shared(rsx_render_batch* destination,
                                  const rsx_render_batch* source)
{
    return clone_batch(destination, source, 1);
}

static int grow_array(void** data, u32* capacity, u32 count, size_t item_size)
{
    if (count < *capacity) return 0;
    u32 next = *capacity ? *capacity * 2u : 64u;
    if (next <= *capacity || (size_t)next > SIZE_MAX / item_size) return -1;
    void* grown = realloc(*data, (size_t)next * item_size);
    if (!grown) return -1;
    memset((u8*)grown + (size_t)*capacity * item_size, 0,
           (size_t)(next - *capacity) * item_size);
    *data = grown;
    *capacity = next;
    return 0;
}

rsx_render_op* rsx_render_batch_add_op(rsx_render_batch* batch,
                                       rsx_render_op_type type)
{
    if (!batch || grow_array((void**)&batch->operations,
                             &batch->operation_capacity,
                             batch->operation_count,
                             sizeof(*batch->operations)) != 0)
        return NULL;
    rsx_render_op* op = &batch->operations[batch->operation_count++];
    memset(op, 0, sizeof(*op));
    op->type = type;
    op->sequence = batch->operation_count - 1u;
    return op;
}

void rsx_render_batch_remove_last_op(rsx_render_batch* batch)
{
    if (!batch || !batch->operation_count) return;
    rsx_render_op* op = &batch->operations[--batch->operation_count];
    destroy_op(op);
    memset(op, 0, sizeof(*op));
}

rsx_surface_init* rsx_render_batch_add_surface_init(rsx_render_batch* batch)
{
    if (!batch || grow_array((void**)&batch->surface_inits,
                             &batch->surface_init_capacity,
                             batch->surface_init_count,
                             sizeof(*batch->surface_inits)) != 0)
        return NULL;
    rsx_surface_init* init = &batch->surface_inits[batch->surface_init_count++];
    memset(init, 0, sizeof(*init));
    return init;
}

static u32 index_at(const u32* source, u32 first, u32 i)
{
    return source ? source[i] : first + i;
}

u32 rsx_expand_primitive_indices(u32 primitive, u32 first, u32 count,
                                 const u32* source, u32* output, u32 capacity,
                                 rsx_portable_topology* topology)
{
    u64 required = 0;
    rsx_portable_topology out_topology;
    switch (primitive) {
    case RSX_PRIMITIVE_POINTS:
        required = count; out_topology = RSX_TOPOLOGY_POINT_LIST; break;
    case RSX_PRIMITIVE_LINES:
        required = count - (count % 2u); out_topology = RSX_TOPOLOGY_LINE_LIST; break;
    case RSX_PRIMITIVE_LINE_STRIP:
        required = count >= 2 ? (u64)(count - 1u) * 2u : 0;
        out_topology = RSX_TOPOLOGY_LINE_LIST; break;
    case RSX_PRIMITIVE_LINE_LOOP:
        required = count >= 2 ? (u64)count * 2u : 0;
        out_topology = RSX_TOPOLOGY_LINE_LIST; break;
    case RSX_PRIMITIVE_TRIANGLES:
        required = count - (count % 3u); out_topology = RSX_TOPOLOGY_TRIANGLE_LIST; break;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        required = count >= 3 ? (u64)(count - 2u) * 3u : 0;
        out_topology = RSX_TOPOLOGY_TRIANGLE_LIST; break;
    case RSX_PRIMITIVE_QUADS:
        required = (u64)(count / 4u) * 6u;
        out_topology = RSX_TOPOLOGY_TRIANGLE_LIST; break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        required = count >= 4 ? (u64)((count - 2u) / 2u) * 6u : 0;
        out_topology = RSX_TOPOLOGY_TRIANGLE_LIST; break;
    default:
        return 0;
    }
    if (!required || required > UINT32_MAX) return 0;
    if (topology) *topology = out_topology;
    if (!output) return (u32)required;
    if (capacity < required) return (u32)required;

    u32 o = 0;
    switch (primitive) {
    case RSX_PRIMITIVE_POINTS:
    case RSX_PRIMITIVE_LINES:
    case RSX_PRIMITIVE_TRIANGLES:
        for (u32 i = 0; i < required; ++i) output[o++] = index_at(source, first, i);
        break;
    case RSX_PRIMITIVE_LINE_STRIP:
        for (u32 i = 0; i + 1 < count; ++i) {
            output[o++] = index_at(source, first, i);
            output[o++] = index_at(source, first, i + 1);
        }
        break;
    case RSX_PRIMITIVE_LINE_LOOP:
        for (u32 i = 0; i < count; ++i) {
            output[o++] = index_at(source, first, i);
            output[o++] = index_at(source, first, (i + 1u) % count);
        }
        break;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 i = 0; i + 2 < count; ++i) {
            u32 a = index_at(source, first, i);
            u32 b = index_at(source, first, i + 1);
            u32 c = index_at(source, first, i + 2);
            if (i & 1u) { u32 t = a; a = b; b = t; }
            output[o++] = a; output[o++] = b; output[o++] = c;
        }
        break;
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 i = 1; i + 1 < count; ++i) {
            output[o++] = index_at(source, first, 0);
            output[o++] = index_at(source, first, i);
            output[o++] = index_at(source, first, i + 1);
        }
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 i = 0; i + 3 < count; i += 4) {
            u32 a = index_at(source, first, i), b = index_at(source, first, i + 1);
            u32 c = index_at(source, first, i + 2), d = index_at(source, first, i + 3);
            output[o++] = a; output[o++] = b; output[o++] = c;
            output[o++] = a; output[o++] = c; output[o++] = d;
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 i = 0; i + 3 < count; i += 2) {
            u32 a = index_at(source, first, i), b = index_at(source, first, i + 1);
            u32 c = index_at(source, first, i + 2), d = index_at(source, first, i + 3);
            output[o++] = a; output[o++] = b; output[o++] = c;
            output[o++] = c; output[o++] = b; output[o++] = d;
        }
        break;
    default: break;
    }
    return o;
}

u32 rsx_expand_primitive_indices_restart(u32 primitive, u32 first, u32 count,
                                         const u32* source,
                                         int restart_enable, u32 restart_index,
                                         u32* output, u32 capacity,
                                         rsx_portable_topology* topology)
{
    if (!restart_enable || !source ||
        primitive != RSX_PRIMITIVE_TRIANGLE_STRIP)
        return rsx_expand_primitive_indices(primitive, first, count, source,
                                            output, capacity, topology);

    u64 required = 0;
    u32 strip_vertices = 0;
    for (u32 i = 0; i < count; ++i) {
        if (source[i] == restart_index) {
            strip_vertices = 0;
            continue;
        }
        if (strip_vertices >= 2) required += 3;
        ++strip_vertices;
    }
    if (!required || required > UINT32_MAX) return 0;
    if (topology) *topology = RSX_TOPOLOGY_TRIANGLE_LIST;
    if (!output || capacity < required) return (u32)required;

    u32 previous[2] = {0, 0};
    strip_vertices = 0;
    u32 emitted = 0;
    for (u32 i = 0; i < count; ++i) {
        const u32 index = source[i];
        if (index == restart_index) {
            strip_vertices = 0;
            continue;
        }
        if (strip_vertices == 0) {
            previous[0] = index;
        } else if (strip_vertices == 1) {
            previous[1] = index;
        } else {
            if ((strip_vertices & 1u) == 0) {
                output[emitted++] = previous[0];
                output[emitted++] = previous[1];
            } else {
                output[emitted++] = previous[1];
                output[emitted++] = previous[0];
            }
            output[emitted++] = index;
            previous[0] = previous[1];
            previous[1] = index;
        }
        ++strip_vertices;
    }
    return emitted;
}

rsx_portable_format rsx_surface_format_from_gcm(u32 format)
{
    switch (format & 0x1fu) {
    case 0x0b: return RSX_FORMAT_RGBA16F;
    case 0x0c: return RSX_FORMAT_RGBA32F;
    case 0x0d: return RSX_FORMAT_R32F;
    default: return RSX_FORMAT_RGBA8;
    }
}

void rsx_texture_component_remap(u32 control1, u8 descriptors[4])
{
    static const u8 output_rsx_channel[4] = {1, 2, 3, 0};
    static const u8 source_to_host[4] = {3, 0, 1, 2};
    if (!descriptors) return;
    for (u32 output = 0; output < 4; ++output) {
        u32 channel = output_rsx_channel[output];
        u32 source = (control1 >> (channel * 2u)) & 3u;
        u32 action = (control1 >> (8u + channel * 2u)) & 3u;
        descriptors[output] = action == 2u ? source_to_host[source]
                            : action == 1u ? 5u : 4u;
    }
}

static u16 read_le16(const u8* source)
{
    return (u16)((u16)source[0] | ((u16)source[1] << 8));
}

static void decode_565(u16 value, u8 color[4])
{
    u32 r = (value >> 11) & 31u;
    u32 g = (value >> 5) & 63u;
    u32 b = value & 31u;
    color[0] = (u8)((r << 3) | (r >> 2));
    color[1] = (u8)((g << 2) | (g >> 4));
    color[2] = (u8)((b << 3) | (b >> 2));
    color[3] = 255;
}

static void decode_bc_colors(const u8* block, int four_color, u8 colors[4][4])
{
    u16 c0 = read_le16(block), c1 = read_le16(block + 2);
    decode_565(c0, colors[0]);
    decode_565(c1, colors[1]);
    if (four_color || c0 > c1) {
        for (u32 channel = 0; channel < 3; ++channel) {
            colors[2][channel] = (u8)((2u * colors[0][channel] +
                                       colors[1][channel]) / 3u);
            colors[3][channel] = (u8)((colors[0][channel] +
                                       2u * colors[1][channel]) / 3u);
        }
        colors[2][3] = colors[3][3] = 255;
    } else {
        for (u32 channel = 0; channel < 3; ++channel)
            colors[2][channel] = (u8)(((u32)colors[0][channel] +
                                       colors[1][channel]) / 2u);
        colors[2][3] = 255;
        memset(colors[3], 0, 4);
    }
}

int rsx_decode_bc_texture(rsx_texture_format format, const void* source_data,
                          u64 source_size, u32 source_pitch, u32 width, u32 height,
                          void* rgba8_data, u64 rgba8_capacity)
{
    if ((format != RSX_TEXTURE_BC1 && format != RSX_TEXTURE_BC2 &&
         format != RSX_TEXTURE_BC3) || !source_data || !rgba8_data ||
        !width || !height || width > 16384u || height > 16384u)
        return -1;
    u64 output_size = (u64)width * height * 4u;
    if (output_size > rgba8_capacity) return -1;
    u32 block_size = format == RSX_TEXTURE_BC1 ? 8u : 16u;
    u32 blocks_x = (width + 3u) / 4u;
    u32 blocks_y = (height + 3u) / 4u;
    u32 minimum_pitch = blocks_x * block_size;
    if (!source_pitch) source_pitch = minimum_pitch;
    if (source_pitch < minimum_pitch) return -1;
    if ((u64)(blocks_y - 1u) * source_pitch + minimum_pitch > source_size)
        return -1;
    const u8* source = (const u8*)source_data;
    u8* output = (u8*)rgba8_data;
    for (u32 block_y = 0; block_y < blocks_y; ++block_y) {
        for (u32 block_x = 0; block_x < blocks_x; ++block_x) {
            const u8* block = source + (u64)block_y * source_pitch +
                              block_x * block_size;
            const u8* color_block = block + (format == RSX_TEXTURE_BC1 ? 0u : 8u);
            u8 colors[4][4];
            decode_bc_colors(color_block, format != RSX_TEXTURE_BC1, colors);
            u32 color_indices = (u32)color_block[4] |
                                ((u32)color_block[5] << 8) |
                                ((u32)color_block[6] << 16) |
                                ((u32)color_block[7] << 24);
            u8 alpha[8] = {255, 255, 255, 255, 255, 255, 255, 255};
            u64 alpha_indices = 0;
            if (format == RSX_TEXTURE_BC3) {
                alpha[0] = block[0]; alpha[1] = block[1];
                if (alpha[0] > alpha[1]) {
                    for (u32 i = 1; i <= 6; ++i)
                        alpha[i + 1] = (u8)(((7u - i) * alpha[0] +
                                             i * alpha[1]) / 7u);
                } else {
                    for (u32 i = 1; i <= 4; ++i)
                        alpha[i + 1] = (u8)(((5u - i) * alpha[0] +
                                             i * alpha[1]) / 5u);
                    alpha[6] = 0; alpha[7] = 255;
                }
                for (u32 i = 0; i < 6; ++i)
                    alpha_indices |= (u64)block[2 + i] << (8u * i);
            }
            for (u32 y = 0; y < 4; ++y) for (u32 x = 0; x < 4; ++x) {
                u32 pixel = y * 4u + x;
                u32 destination_x = block_x * 4u + x;
                u32 destination_y = block_y * 4u + y;
                if (destination_x >= width || destination_y >= height) continue;
                const u8* color = colors[(color_indices >> (pixel * 2u)) & 3u];
                u8* destination = output + ((u64)destination_y * width +
                                             destination_x) * 4u;
                memcpy(destination, color, 4);
                if (format == RSX_TEXTURE_BC2)
                    destination[3] = (u8)(((block[pixel / 2u] >>
                                            ((pixel & 1u) * 4u)) & 15u) * 17u);
                else if (format == RSX_TEXTURE_BC3)
                    destination[3] = alpha[(alpha_indices >> (pixel * 3u)) & 7u];
            }
        }
    }
    return 0;
}

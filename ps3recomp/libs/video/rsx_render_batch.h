/* Portable, owning RSX render batches. No host graphics API types belong here. */
#ifndef PS3RECOMP_RSX_RENDER_BATCH_H
#define PS3RECOMP_RSX_RENDER_BATCH_H

#include "ps3emu/ps3types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_BATCH_MAX_TEXTURES 4u
#define RSX_BATCH_VP_CONSTANTS 514u
#define RSX_BATCH_FORMAT_VERSION 2u

#define RSX_TEXTURE_FLAG_UNNORMALIZED_COORDS 0x00000001u

typedef enum rsx_portable_format {
    RSX_FORMAT_INVALID = 0,
    RSX_FORMAT_RGBA8 = 1,
    RSX_FORMAT_RGBA16F = 2,
    RSX_FORMAT_RGBA32F = 3,
    RSX_FORMAT_R32F = 4,
    RSX_FORMAT_D24S8 = 5,
    RSX_FORMAT_D32F = 6,
} rsx_portable_format;

typedef enum rsx_texture_format {
    RSX_TEXTURE_INVALID = 0,
    RSX_TEXTURE_R8 = 1,
    RSX_TEXTURE_RGBA8 = 2,
    RSX_TEXTURE_BC1 = 3,
    RSX_TEXTURE_BC2 = 4,
    RSX_TEXTURE_BC3 = 5,
} rsx_texture_format;

typedef enum rsx_portable_topology {
    RSX_TOPOLOGY_POINT_LIST = 1,
    RSX_TOPOLOGY_LINE_LIST = 2,
    RSX_TOPOLOGY_TRIANGLE_LIST = 3,
} rsx_portable_topology;

typedef enum rsx_vertex_layout {
    RSX_VERTEX_LAYOUT_FALLBACK_36 = 1,
    RSX_VERTEX_LAYOUT_FLOAT4_X16 = 2,
} rsx_vertex_layout;

typedef enum rsx_render_op_type {
    RSX_RENDER_OP_CLEAR = 1,
    RSX_RENDER_OP_DRAW = 2,
} rsx_render_op_type;

typedef struct rsx_owned_blob {
    u8* data;
    u64 size;
    u64 hash;
    /* Internal shared ownership for immutable in-memory batches. This pointer
     * is never serialized into RSXB files. */
    u32* references;
} rsx_owned_blob;

typedef struct rsx_surface_ref {
    u32 raw_offset;
    u32 resolved_offset;
    u32 width;
    u32 height;
    u32 pitch;
    u32 display_buffer_id;
    rsx_portable_format format;
    u8 is_display;
    u8 is_depth;
    u8 reserved[2];
} rsx_surface_ref;

typedef struct rsx_texture_source {
    u32 raw_offset;
    u32 resolved_offset;
    u32 width;
    u32 height;
    u32 pitch;
    u32 address;
    u32 control1;
    u32 border_color;
    rsx_texture_format format;
    u32 flags;
    rsx_owned_blob payload;
} rsx_texture_source;

typedef struct rsx_pipeline_key {
    u64 vertex_shader_hash;
    u64 fragment_shader_hash;
    u32 topology;
    u32 vertex_layout;
    u32 color_format[4];
    u32 color_target_count;
    u32 depth_format;
    u32 color_write_mask;
    u32 blend_sfactor;
    u32 blend_dfactor;
    u32 blend_equation;
    u32 blend_color;
    u32 depth_func;
    u32 stencil_func;
    u32 stencil_ref;
    u32 stencil_mask;
    u32 stencil_fail;
    u32 stencil_zfail;
    u32 stencil_zpass;
    u32 cull_face;
    u32 front_face;
    u32 alpha_func;
    u32 alpha_ref;
    u8 blend_enable;
    u8 depth_test_enable;
    u8 depth_write_enable;
    u8 stencil_test_enable;
    u8 cull_enable;
    u8 alpha_test_enable;
    u8 fragment_32bit_exports;
    u8 reserved;
} rsx_pipeline_key;

typedef struct rsx_clear_op_data {
    u32 flags;
    float color[4];
    float depth;
    u8 stencil;
    u8 reserved[3];
} rsx_clear_op_data;

typedef struct rsx_draw_op_data {
    rsx_pipeline_key pipeline;
    rsx_texture_source textures[RSX_BATCH_MAX_TEXTURES];
    u32 texture_count;
    u32 vertex_count;
    u32 index_count;
    rsx_owned_blob vertex_data;
    rsx_owned_blob index_data;
    rsx_owned_blob vertex_constants;
    rsx_owned_blob vertex_shader;
    rsx_owned_blob fragment_shader;
} rsx_draw_op_data;

typedef struct rsx_render_op {
    rsx_render_op_type type;
    u32 sequence;
    rsx_surface_ref color[4];
    u32 color_count;
    rsx_surface_ref depth;
    u32 viewport[4];
    u32 scissor[4];
    union {
        rsx_clear_op_data clear;
        rsx_draw_op_data draw;
    } data;
} rsx_render_op;

typedef struct rsx_surface_init {
    rsx_surface_ref surface;
    rsx_owned_blob color_data;
    rsx_owned_blob depth_stencil_data;
} rsx_surface_init;

typedef struct rsx_render_batch {
    u64 serial;
    u32 display_buffer_id;
    u32 flags;
    rsx_render_op* operations;
    u32 operation_count;
    u32 operation_capacity;
    rsx_surface_init* surface_inits;
    u32 surface_init_count;
    u32 surface_init_capacity;
} rsx_render_batch;

typedef struct rsx_render_backend_ops {
    int (*init)(void* userdata, u32 width, u32 height, const char* title);
    int (*submit_batch)(void* userdata, const rsx_render_batch* batch);
    int (*snapshot_surfaces)(void* userdata, rsx_render_batch* seed_batch);
    int (*readback_display)(void* userdata, u32 display_buffer_id,
                            void* rgba8, u32 width, u32 height, u32 pitch);
    void (*shutdown)(void* userdata);
    const char* (*driver_name)(void* userdata);
} rsx_render_backend_ops;

void rsx_render_batch_init(rsx_render_batch* batch, u64 serial);
void rsx_render_batch_reset(rsx_render_batch* batch, u64 next_serial);
void rsx_render_batch_destroy(rsx_render_batch* batch);
int rsx_render_batch_clone(rsx_render_batch* destination,
                           const rsx_render_batch* source);
int rsx_render_batch_clone_shared(rsx_render_batch* destination,
                                  const rsx_render_batch* source);
rsx_render_op* rsx_render_batch_add_op(rsx_render_batch* batch,
                                       rsx_render_op_type type);
void rsx_render_batch_remove_last_op(rsx_render_batch* batch);
rsx_surface_init* rsx_render_batch_add_surface_init(rsx_render_batch* batch);
int rsx_owned_blob_copy(rsx_owned_blob* blob, const void* data, u64 size);
int rsx_owned_blob_allocate(rsx_owned_blob* blob, u64 size);
int rsx_owned_blob_share(rsx_owned_blob* destination,
                         const rsx_owned_blob* source);
void rsx_owned_blob_destroy(rsx_owned_blob* blob);
u64 rsx_blob_hash64(const void* data, u64 size);

/* Expand an RSX primitive into portable point/line/triangle-list indices.
 * source_indices may be NULL, in which case first+i is used. Returns the
 * required/output index count; zero means unsupported or invalid input. */
u32 rsx_expand_primitive_indices(u32 primitive, u32 first, u32 count,
                                 const u32* source_indices,
                                 u32* output_indices, u32 output_capacity,
                                 rsx_portable_topology* output_topology);

/* Indexed triangle strips reset their alternating winding after an RSX
 * primitive-restart marker. The non-restart helper remains the common path
 * for arrays and ordinary indexed primitives. */
u32 rsx_expand_primitive_indices_restart(u32 primitive, u32 first, u32 count,
                                         const u32* source_indices,
                                         int restart_enable, u32 restart_index,
                                         u32* output_indices,
                                         u32 output_capacity,
                                         rsx_portable_topology* output_topology);

rsx_portable_format rsx_surface_format_from_gcm(u32 gcm_format);

/* Decode one immutable BC payload into tightly packed RGBA8. */
int rsx_decode_bc_texture(rsx_texture_format format, const void* source,
                          u64 source_size, u32 source_pitch, u32 width, u32 height,
                          void* rgba8, u64 rgba8_capacity);

/* Decode RSX A/R/G/B CONTROL1 selectors into host R/G/B/A descriptors.
 * Values 0..3 select host RGBA, 4 forces zero, and 5 forces one. */
void rsx_texture_component_remap(u32 control1, u8 descriptors[4]);

#ifdef __cplusplus
}
#endif
#endif

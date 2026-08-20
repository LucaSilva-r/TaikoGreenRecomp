#include "rsx_batch_io.h"
#include "rsx_sdl_gpu_backend.h"
#include <ps3emu/host_sdl.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

static int make_directory(const char* path)
{
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0777);
#endif
}

static const char* value_after(const char* argument, const char* prefix)
{
    size_t length = strlen(prefix);
    return strncmp(argument, prefix, length) == 0 ? argument + length : NULL;
}

int main(int argc, char** argv)
{
    const char* backend = NULL;
    const char* input = NULL;
    const char* output_dir = NULL;
    const char* dump_texture_dir = NULL;
    u32 stop_after_op = ~(u32)0;
    u32 blend_override_op = ~(u32)0;
    u32 blend_override_sfactor = 0;
    u32 blend_override_dfactor = 0;
    int have_blend_override = 0;
    int inspect = 0, inspect_only = 0;
    for (int i = 1; i < argc; ++i) {
        const char* value;
        if ((value = value_after(argv[i], "--backend="))) backend = value;
        else if ((value = value_after(argv[i], "--input="))) input = value;
        else if ((value = value_after(argv[i], "--output-dir="))) output_dir = value;
        else if ((value = value_after(argv[i], "--dump-textures=")))
            dump_texture_dir = value;
        else if ((value = value_after(argv[i], "--stop-after-op="))) {
            char* end = NULL;
            unsigned long parsed = strtoul(value, &end, 0);
            if (!value[0] || !end || *end || parsed > ~(u32)0) {
                fprintf(stderr, "invalid operation index: %s\n", value);
                return 2;
            }
            stop_after_op = (u32)parsed;
        }
        else if ((value = value_after(argv[i], "--blend-override="))) {
            unsigned op, sfactor, dfactor;
            char tail;
            if (sscanf(value, "%u,%x,%x%c", &op, &sfactor, &dfactor,
                       &tail) != 3) {
                fprintf(stderr, "invalid blend override: %s\n", value);
                return 2;
            }
            blend_override_op = (u32)op;
            blend_override_sfactor = (u32)sfactor;
            blend_override_dfactor = (u32)dfactor;
            have_blend_override = 1;
        }
        else if (strcmp(argv[i], "--inspect") == 0) inspect = 1;
        else if (strcmp(argv[i], "--inspect-only") == 0)
            inspect = inspect_only = 1;
        else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!backend || strcmp(backend, "sdl_gpu") != 0 || !input || !output_dir) {
        fprintf(stderr,
                "usage: rsx_replay --backend=sdl_gpu --input=FILE "
                "--output-dir=DIR [--inspect|--inspect-only] "
                "[--stop-after-op=N] [--dump-textures=DIR] "
                "[--blend-override=OP,SFACTOR,DFACTOR]\n");
        return 2;
    }
    rsx_render_batch* batches = NULL;
    u32 count = 0;
    char error[256] = {0};
    if (rsxb_read_file(input, &batches, &count, error, sizeof(error)) != 0) {
        fprintf(stderr, "rsx_replay: %s\n", error);
        return 1;
    }
    if (make_directory(output_dir) != 0 && errno != EEXIST) {
        fprintf(stderr, "rsx_replay: cannot create %s: %s\n",
                output_dir, strerror(errno));
        rsxb_free_batches(batches, count);
        return 1;
    }
    if (dump_texture_dir &&
        make_directory(dump_texture_dir) != 0 && errno != EEXIST) {
        fprintf(stderr, "rsx_replay: cannot create %s: %s\n",
                dump_texture_dir, strerror(errno));
        rsxb_free_batches(batches, count);
        return 1;
    }
    if (dump_texture_dir) {
        for (u32 i = 0; i < count; ++i) {
            const rsx_render_batch* batch = &batches[i];
            for (u32 j = 0; j < batch->operation_count; ++j) {
                const rsx_render_op* op = &batch->operations[j];
                if (op->type != RSX_RENDER_OP_DRAW) continue;
                for (u32 unit = 0;
                     unit < op->data.draw.texture_count &&
                     unit < RSX_BATCH_MAX_TEXTURES; ++unit) {
                    const rsx_texture_source* texture =
                        &op->data.draw.textures[unit];
                    if (!texture->payload.data || !texture->payload.size)
                        continue;
                    char path[1024];
                    const char* extension = texture->format == RSX_TEXTURE_RGBA8
                        ? "rgba" : "bin";
                    int written = snprintf(
                        path, sizeof(path), "%s/tex_%08X_%ux%u_f%u.%s",
                        dump_texture_dir, texture->raw_offset,
                        texture->width, texture->height,
                        (u32)texture->format, extension);
                    if (written < 0 || (size_t)written >= sizeof(path))
                        continue;
                    FILE* file = fopen(path, "wb");
                    if (!file ||
                        fwrite(texture->payload.data, 1,
                               (size_t)texture->payload.size, file) !=
                            texture->payload.size) {
                        fprintf(stderr, "rsx_replay: texture dump failed: %s\n",
                                path);
                    }
                    if (file) fclose(file);
                }
            }
        }
    }
    if (inspect) {
        for (u32 i = 0; i < count; ++i) {
            const rsx_render_batch* batch = &batches[i];
            fprintf(stderr,
                    "[RSXB] batch=%u serial=%llu display=%u ops=%u inits=%u\n",
                    i, (unsigned long long)batch->serial,
                    batch->display_buffer_id, batch->operation_count,
                    batch->surface_init_count);
            for (u32 j = 0; j < batch->operation_count; ++j) {
                const rsx_render_op* op = &batch->operations[j];
                const rsx_surface_ref* color = op->color_count ? &op->color[0] : NULL;
                fprintf(stderr,
                        "[RSXB]   op=%u seq=%u %s target=%08X %ux%u fmt=%u "
                        "display=%u vp=%u,%u %ux%u sc=%u,%u %ux%u",
                        j, op->sequence,
                        op->type == RSX_RENDER_OP_CLEAR ? "clear" : "draw ",
                        color ? color->raw_offset : 0,
                        color ? color->width : 0, color ? color->height : 0,
                        color ? (u32)color->format : 0,
                        color ? color->is_display : 0,
                        op->viewport[0], op->viewport[1],
                        op->viewport[2], op->viewport[3],
                        op->scissor[0], op->scissor[1],
                        op->scissor[2], op->scissor[3]);
                if (op->type == RSX_RENDER_OP_CLEAR) {
                    fprintf(stderr, " flags=%X rgba=%.3f,%.3f,%.3f,%.3f\n",
                            op->data.clear.flags,
                            op->data.clear.color[0], op->data.clear.color[1],
                            op->data.clear.color[2], op->data.clear.color[3]);
                } else {
                    const rsx_draw_op_data* draw = &op->data.draw;
                    fprintf(stderr,
                            " verts=%u topo=%u layout=%u tex=%u vs=%016llX fs=%016llX "
                            "fsraw=%016llX/f%llu "
                            "blend=%u/%08X/%08X/%08X depth=%u/%u/%X "
                            "stencil=%u/%X/%u/%02X/%X,%X,%X "
                            "cull=%u/%X/%X alpha=%u/%X/%02X cmask=%X",
                            draw->vertex_count, draw->pipeline.topology,
                            draw->pipeline.vertex_layout, draw->texture_count,
                            (unsigned long long)draw->pipeline.vertex_shader_hash,
                            (unsigned long long)draw->pipeline.fragment_shader_hash,
                            (unsigned long long)draw->fragment_shader.hash,
                            (unsigned long long)draw->fragment_shader.size,
                            draw->pipeline.blend_enable,
                            draw->pipeline.blend_sfactor,
                            draw->pipeline.blend_dfactor,
                            draw->pipeline.blend_equation,
                            draw->pipeline.depth_test_enable,
                            draw->pipeline.depth_write_enable,
                            draw->pipeline.depth_func,
                            draw->pipeline.stencil_test_enable,
                            draw->pipeline.stencil_func,
                            draw->pipeline.stencil_ref,
                            draw->pipeline.stencil_mask,
                            draw->pipeline.stencil_fail,
                            draw->pipeline.stencil_zfail,
                            draw->pipeline.stencil_zpass,
                            draw->pipeline.cull_enable,
                            draw->pipeline.cull_face,
                            draw->pipeline.front_face,
                            draw->pipeline.alpha_test_enable,
                            draw->pipeline.alpha_func,
                            draw->pipeline.alpha_ref,
                            draw->pipeline.color_write_mask);
                    if (draw->texture_count) {
                        for (u32 unit = 0;
                             unit < draw->texture_count && unit < RSX_BATCH_MAX_TEXTURES;
                             ++unit) {
                            const rsx_texture_source* texture = &draw->textures[unit];
                            fprintf(stderr,
                                    " t%u=%08X/%ux%u/f%u/p%llu/c%08X",
                                    unit, texture->raw_offset,
                                    texture->width, texture->height,
                                    (u32)texture->format,
                                    (unsigned long long)texture->payload.size,
                                    texture->control1);
                        }
                    }
                    if (draw->vertex_constants.size >=
                        (u64)RSX_BATCH_VP_CONSTANTS * 4u * sizeof(float)) {
                        float epilogue[8];
                        memcpy(epilogue,
                               draw->vertex_constants.data + 512u * 4u * sizeof(float),
                               sizeof(epilogue));
                        fprintf(stderr,
                                " vps=%.3g,%.3g,%.3g,%.3g vpo=%.3g,%.3g,%.3g,%.3g",
                                epilogue[0], epilogue[1], epilogue[2], epilogue[3],
                                epilogue[4], epilogue[5], epilogue[6], epilogue[7]);
                    }
                    if (draw->pipeline.vertex_layout == RSX_VERTEX_LAYOUT_FLOAT4_X16 &&
                        draw->vertex_count && draw->vertex_data.size >= 256u &&
                        draw->vertex_constants.size >= 468u * 16u) {
                        float a0[4], a3[4], a8[4], c256[4], c257[4], c259[4], c467[4];
                        memcpy(a0, draw->vertex_data.data, sizeof(a0));
                        memcpy(a3, draw->vertex_data.data + 3u * 16u, sizeof(a3));
                        memcpy(a8, draw->vertex_data.data + 8u * 16u, sizeof(a8));
                        memcpy(c256, draw->vertex_constants.data + 256u * 16u, sizeof(c256));
                        memcpy(c257, draw->vertex_constants.data + 257u * 16u, sizeof(c257));
                        memcpy(c259, draw->vertex_constants.data + 259u * 16u, sizeof(c259));
                        memcpy(c467, draw->vertex_constants.data + 467u * 16u, sizeof(c467));
                        fprintf(stderr,
                                " a0=%.3g,%.3g,%.3g,%.3g "
                                "a3=%.3g,%.3g,%.3g,%.3g a8=%.3g,%.3g "
                                "c256=%.3g,%.3g c257=%.3g,%.3g "
                                "c259=%.3g,%.3g c467=%.3g,%.3g",
                                a0[0], a0[1], a0[2], a0[3],
                                a3[0], a3[1], a3[2], a3[3], a8[0], a8[1],
                                c256[0], c256[1], c257[0], c257[1],
                                c259[0], c259[1], c467[0], c467[1]);
                    }
                    fprintf(stderr, "\n");
                }
            }
        }
    }
    if (inspect_only) {
        rsxb_free_batches(batches, count);
        return 0;
    }
    if (ps3_host_sdl_init(PS3_HOST_SDL_VIDEO | PS3_HOST_SDL_GAMEPAD) != 0 ||
        rsx_sdl_gpu_backend_main_init(1280, 720, "RSX batch replay") != 0) {
        ps3_host_sdl_shutdown();
        rsxb_free_batches(batches, count);
        return 1;
    }
    int result = 0;
    for (u32 i = 0; i < count; ++i) {
        if (have_blend_override && i == 0 &&
            blend_override_op < batches[i].operation_count) {
            rsx_render_op* op = &batches[i].operations[blend_override_op];
            if (op->type != RSX_RENDER_OP_DRAW) {
                fprintf(stderr, "blend override operation is not a draw\n");
                result = 1;
                break;
            }
            op->data.draw.pipeline.blend_sfactor = blend_override_sfactor;
            op->data.draw.pipeline.blend_dfactor = blend_override_dfactor;
        }
        u32 saved_operation_count = batches[i].operation_count;
        if (stop_after_op != ~(u32)0 &&
            stop_after_op + 1u < batches[i].operation_count)
            batches[i].operation_count = stop_after_op + 1u;
        if (rsx_sdl_gpu_backend_submit_batch(&batches[i]) != 0) {
            batches[i].operation_count = saved_operation_count;
            result = 1;
            break;
        }
        do {
            rsx_sdl_gpu_backend_main_iterate(0);
        } while (rsx_sdl_gpu_backend_has_pending_batches());
        batches[i].operation_count = saved_operation_count;
        char path[1024];
        int written = snprintf(path, sizeof(path), "%s/frame_%06u.bmp",
                               output_dir, i);
        if (written < 0 || (size_t)written >= sizeof(path) ||
            rsx_sdl_gpu_backend_save_display_bmp(path) != 0) {
            result = 1;
            break;
        }
    }
    unsigned errors = rsx_sdl_gpu_backend_error_count();
    rsx_sdl_gpu_backend_main_shutdown();
    ps3_host_sdl_shutdown();
    rsxb_free_batches(batches, count);
    if (errors) {
        fprintf(stderr, "rsx_replay: renderer reported %u errors\n", errors);
        result = 1;
    }
    return result;
}

#include "rsx_batch_io.h"
#include "rsx_commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int check(int condition, const char* message)
{
    if (!condition) fprintf(stderr, "rsx batch test: %s\n", message);
    return condition;
}

static void put_fp_word(u8* output,u32 host_word)
{
    u32 swapped=(host_word<<16)|(host_word>>16);
    output[0]=(u8)(swapped>>24);output[1]=(u8)(swapped>>16);
    output[2]=(u8)(swapped>>8);output[3]=(u8)swapped;
}

static int test_primitives(void)
{
    u32 output[32]; rsx_portable_topology topology = 0;
    static const u32 quad_expected[] = {4,5,6,4,6,7};
    if (!check(rsx_expand_primitive_indices(RSX_PRIMITIVE_QUADS,4,4,NULL,
            output,32,&topology)==6, "quad expansion size")) return 0;
    if (!check(topology==RSX_TOPOLOGY_TRIANGLE_LIST &&
               memcmp(output,quad_expected,sizeof(quad_expected))==0,
               "quad expansion values")) return 0;
    static const u32 strip_expected[] = {0,1,2,2,1,3,2,3,4};
    if (!check(rsx_expand_primitive_indices(RSX_PRIMITIVE_TRIANGLE_STRIP,0,5,
            NULL,output,32,&topology)==9 &&
            memcmp(output,strip_expected,sizeof(strip_expected))==0,
            "triangle strip winding")) return 0;
    static const u32 restart_source[] = {4,5,6,0xffffu,7,8,9,10};
    static const u32 restart_expected[] = {4,5,6,7,8,9,9,8,10};
    if (!check(rsx_expand_primitive_indices_restart(
            RSX_PRIMITIVE_TRIANGLE_STRIP,0,8,restart_source,1,0xffffu,
            output,32,&topology)==9 &&
            memcmp(output,restart_expected,sizeof(restart_expected))==0,
            "triangle strip primitive restart")) return 0;
    static const u32 loop_expected[] = {2,3,3,4,4,2};
    if (!check(rsx_expand_primitive_indices(RSX_PRIMITIVE_LINE_LOOP,2,3,NULL,
            output,32,&topology)==6 &&
            memcmp(output,loop_expected,sizeof(loop_expected))==0,
            "line loop closure")) return 0;
    return 1;
}

static int test_texture_helpers(void)
{
    u8 remap[4];
    rsx_texture_component_remap(0xaae4u,remap);
    static const u8 identity[4]={0,1,2,3};
    if(!check(memcmp(remap,identity,4)==0,"identity component remap"))return 0;
    rsx_texture_component_remap(0x5500u,remap);
    static const u8 forced_one[4]={5,5,5,5};
    if(!check(memcmp(remap,forced_one,4)==0,"forced-one component remap"))return 0;

    u8 rgba[4*4*4];
    const u8 bc1_red[8]={0x00,0xf8,0xe0,0x07,0,0,0,0};
    if(!check(rsx_decode_bc_texture(RSX_TEXTURE_BC1,bc1_red,sizeof(bc1_red),
                                    8,4,4,rgba,sizeof(rgba))==0,
              "BC1 decode"))return 0;
    for(unsigned i=0;i<16;++i)
        if(!check(rgba[i*4]==255&&rgba[i*4+1]==0&&rgba[i*4+2]==0&&
                  rgba[i*4+3]==255,"BC1 red texel"))return 0;

    u8 bc2[16]={0};memset(bc2,0x88,8);
    memcpy(bc2+8,bc1_red,8);
    if(!check(rsx_decode_bc_texture(RSX_TEXTURE_BC2,bc2,sizeof(bc2),16,
                                    4,4,rgba,sizeof(rgba))==0&&rgba[3]==136,
              "BC2 explicit alpha"))return 0;

    u8 bc3[16]={255,0,0};u64 alpha_indices=0;
    for(unsigned i=0;i<16;++i)alpha_indices|=(u64)2u<<(i*3u);
    for(unsigned i=0;i<6;++i)bc3[2+i]=(u8)(alpha_indices>>(i*8u));
    memcpy(bc3+8,bc1_red,8);
    if(!check(rsx_decode_bc_texture(RSX_TEXTURE_BC3,bc3,sizeof(bc3),16,
                                    4,4,rgba,sizeof(rgba))==0&&rgba[3]==218,
              "BC3 interpolated alpha"))return 0;
    if(!check(rsx_decode_bc_texture(RSX_TEXTURE_BC1,bc1_red,7,8,4,4,
                                    rgba,sizeof(rgba))!=0,
              "truncated BC payload rejected"))return 0;
    return 1;
}

static int build_batch(rsx_render_batch* batch)
{
    static const u8 vertices[] = {1,2,3,4,5,6,7,8};
    static const u8 texture[] = {0xaa,0xbb,0xcc,0xdd};
    rsx_render_batch_init(batch,42);
    batch->display_buffer_id=1;
    rsx_render_op* clear=rsx_render_batch_add_op(batch,RSX_RENDER_OP_CLEAR);
    rsx_render_op* draw=rsx_render_batch_add_op(batch,RSX_RENDER_OP_DRAW);
    if(!clear||!draw)return 0;
    clear->color_count=1;clear->color[0].is_display=1;clear->color[0].width=1280;
    clear->color[0].height=720;clear->color[0].format=RSX_FORMAT_RGBA8;
    clear->data.clear.flags=0xf3;clear->data.clear.color[0]=0.25f;
    draw->color_count=1;draw->color[0]=clear->color[0];
    draw->data.draw.pipeline.topology=RSX_TOPOLOGY_TRIANGLE_LIST;
    draw->data.draw.pipeline.vertex_layout=RSX_VERTEX_LAYOUT_FLOAT4_X16;
    draw->data.draw.pipeline.color_target_count=1;
    draw->data.draw.pipeline.color_format[0]=RSX_FORMAT_RGBA8;
    draw->data.draw.texture_count=2;draw->data.draw.vertex_count=0;
    draw->data.draw.textures[0].format=RSX_TEXTURE_RGBA8;
    draw->data.draw.textures[0].width=1;draw->data.draw.textures[0].height=1;
    draw->data.draw.textures[0].pitch=4;
    draw->data.draw.textures[0].control1=0xaae4u; /* identity A/R/G/B remap */
    /* Render-to-texture aliases identify a persistent surface and therefore
     * intentionally carry no duplicate CPU payload. */
    draw->data.draw.textures[1].resolved_offset=0x2000;
    draw->data.draw.textures[1].format=RSX_TEXTURE_RGBA8;
    draw->data.draw.textures[1].width=1;draw->data.draw.textures[1].height=1;
    draw->data.draw.textures[1].pitch=4;
    if(rsx_owned_blob_copy(&draw->data.draw.vertex_data,vertices,sizeof(vertices))!=0||
       rsx_owned_blob_copy(&draw->data.draw.textures[0].payload,texture,sizeof(texture))!=0)
        return 0;
    rsx_surface_init* init=rsx_render_batch_add_surface_init(batch);
    float float_surface[4]={0.25f,0.5f,0.75f,1.0f};
    if(!init)return 0;
    init->surface.raw_offset=0x2000;init->surface.resolved_offset=0x2000;
    init->surface.width=1;init->surface.height=1;init->surface.pitch=16;
    init->surface.format=RSX_FORMAT_RGBA32F;
    if(rsx_owned_blob_copy(&init->color_data,float_surface,
                           sizeof(float_surface))!=0)return 0;
    return 1;
}

static int test_clone_and_io(void)
{
    rsx_render_batch source,clone,shared; if(!build_batch(&source))return 0;
    if(!check(rsx_render_batch_clone(&clone,&source)==0,"deep clone"))return 0;
    if(!check(rsx_render_batch_clone_shared(&shared,&source)==0,
              "shared immutable clone"))return 0;
    source.operations[1].data.draw.vertex_data.data[0]=99;
    if(!check(clone.operations[1].data.draw.vertex_data.data[0]==1,
              "clone retains immutable vertex bytes"))return 0;
    rsx_render_batch_destroy(&source);
    if(!check(shared.operations[1].data.draw.vertex_data.data[0]==99,
              "shared clone owns bytes after source release"))return 0;

    char path[]="/tmp/taiko-rsxb-test-XXXXXX";int fd=mkstemp(path);
    if(!check(fd>=0,"temporary capture"))return 0;close(fd);
    char error[160]={0};
    if(!check(rsxb_write_file(path,&clone,1,error,sizeof(error))==0,error))return 0;
    rsx_render_batch* loaded=NULL;u32 count=0;
    if(!check(rsxb_read_file(path,&loaded,&count,error,sizeof(error))==0,error))return 0;
    if(!check(count==1&&loaded[0].serial==42&&loaded[0].operation_count==2,
              "capture structure round trip"))return 0;
    if(!check(loaded[0].surface_init_count==1&&
              loaded[0].surface_inits[0].surface.format==RSX_FORMAT_RGBA32F&&
              loaded[0].surface_inits[0].color_data.size==16,
              "float surface initialization round trip"))return 0;
    if(!check(loaded[0].operations[1].data.draw.vertex_data.size==8&&
              loaded[0].operations[1].data.draw.vertex_data.data[0]==1,
              "capture blob round trip"))return 0;
    if(!check(loaded[0].operations[1].data.draw.textures[0].control1==0xaae4u,
              "texture state round trip"))return 0;
    if(!check(loaded[0].operations[1].data.draw.texture_count==2&&
              loaded[0].operations[1].data.draw.textures[1].resolved_offset==0x2000&&
              loaded[0].operations[1].data.draw.textures[1].payload.size==0,
              "render-surface texture alias round trip"))return 0;
    rsxb_free_batches(loaded,count);

    const char* fixture=getenv("RSXB_TEST_OUTPUT");
    if(fixture&&fixture[0]) {
        /* A clear-only deterministic fixture exercises replay initialization,
         * fixed-resolution presentation and BMP readback without guest data. */
        u32 operation_count=clone.operation_count;
        clone.operation_count=1;
        if(!check(rsxb_write_file(fixture,&clone,1,error,sizeof(error))==0,error))return 0;
        clone.operation_count=operation_count;
    }
    const char* draw_fixture=getenv("RSXB_DRAW_TEST_OUTPUT");
    if(draw_fixture&&draw_fixture[0]) {
        static const float triangle[27]={
            -0.75f,-0.75f,0.0f, 1,0,0,1, 0,0,
             0.75f,-0.75f,0.0f, 0,1,0,1, 1,0,
             0.0f, 0.75f,0.0f, 0,0,1,1, 0.5f,1
        };
        rsx_render_op* draw=&clone.operations[1];
        rsx_owned_blob_destroy(&draw->data.draw.vertex_data);
        if(!check(rsx_owned_blob_copy(&draw->data.draw.vertex_data,
                                      triangle,sizeof(triangle))==0,
                  "draw fixture vertices"))return 0;
        draw->data.draw.vertex_count=3;
        draw->data.draw.texture_count=1;
        draw->data.draw.pipeline.vertex_layout=RSX_VERTEX_LAYOUT_FALLBACK_36;
        u8 fragment_program[16];
        put_fp_word(fragment_program+0,(0x17u<<24)|(0xfu<<9)|(4u<<13)|1u);
        put_fp_word(fragment_program+4,1u|(0u<<9)|(1u<<11)|(2u<<13)|(3u<<15)|
                                          (7u<<18));
        put_fp_word(fragment_program+8,0);put_fp_word(fragment_program+12,0);
        if(!check(rsx_owned_blob_copy(&draw->data.draw.fragment_shader,
                                      fragment_program,sizeof(fragment_program))==0,
                  "draw fixture fragment program"))return 0;
        draw->data.draw.pipeline.fragment_shader_hash=
            draw->data.draw.fragment_shader.hash;
        draw->viewport[2]=1280;draw->viewport[3]=720;
        draw->scissor[2]=1280;draw->scissor[3]=720;
        if(!check(rsxb_write_file(draw_fixture,&clone,1,error,sizeof(error))==0,
                  error))return 0;
    }
    const char* vp_fixture=getenv("RSXB_VP_TEST_OUTPUT");
    if(vp_fixture&&vp_fixture[0]) {
        rsx_render_op* draw=&clone.operations[1];
        float vertices[3][16][4]={0};
        u8 vertex_program[16]={0};vertex_program[12]=1; /* D3.end */
        float constants[RSX_BATCH_VP_CONSTANTS][4]={0};
        constants[512][0]=constants[512][1]=constants[512][2]=1.0f;
        draw->data.draw.pipeline.vertex_layout=RSX_VERTEX_LAYOUT_FLOAT4_X16;
        draw->data.draw.vertex_count=3;
        rsx_owned_blob_destroy(&draw->data.draw.vertex_data);
        rsx_owned_blob_destroy(&draw->data.draw.vertex_shader);
        rsx_owned_blob_destroy(&draw->data.draw.fragment_shader);
        rsx_owned_blob_destroy(&draw->data.draw.vertex_constants);
        if(!check(rsx_owned_blob_copy(&draw->data.draw.vertex_data,
                                      vertices,sizeof(vertices))==0&&
                  rsx_owned_blob_copy(&draw->data.draw.vertex_shader,
                                      vertex_program,sizeof(vertex_program))==0&&
                  rsx_owned_blob_copy(&draw->data.draw.vertex_constants,
                                      constants,sizeof(constants))==0,
                  "VP fixture blobs"))return 0;
        draw->data.draw.pipeline.vertex_shader_hash=draw->data.draw.vertex_shader.hash;
        draw->data.draw.pipeline.fragment_shader_hash=0;
        draw->viewport[2]=1280;draw->viewport[3]=720;
        draw->scissor[2]=1280;draw->scissor[3]=720;
        if(!check(rsxb_write_file(vp_fixture,&clone,1,error,sizeof(error))==0,
                  error))return 0;
    }

    FILE* file=fopen(path,"r+b");if(!check(file!=NULL,"open capture to corrupt"))return 0;
    fseek(file,-1,SEEK_END);int byte=fgetc(file);fseek(file,-1,SEEK_END);
    fputc(byte^0x80,file);fclose(file);
    loaded=NULL;count=0;
    if(!check(rsxb_read_file(path,&loaded,&count,error,sizeof(error))!=0,
              "corrupt checksum rejected"))return 0;
    unlink(path);rsx_render_batch_destroy(&shared);rsx_render_batch_destroy(&clone);
    return 1;
}

int main(void)
{
    if(!test_primitives()||!test_texture_helpers()||!test_clone_and_io())return 1;
    puts("portable RSX batch tests passed");return 0;
}

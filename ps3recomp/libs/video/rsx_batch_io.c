#include "rsx_batch_io.h"
#include "rsx_vp_decompiler.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RSXB_HEADER_SIZE 64u
#define RSXB_ENDIAN_MARKER 0x01020304u
static const u8 k_magic[8] = {'R','S','X','B',0x0d,0x0a,0x1a,0x00};

typedef struct writer { u8* data; u64 size, capacity; int failed; } writer;
typedef struct reader { const u8* data; u64 size, offset, allocation; int failed; } reader;

static void set_error(char* error, size_t size, const char* format, ...)
{
    if (!error || !size) return;
    va_list args; va_start(args, format); vsnprintf(error, size, format, args); va_end(args);
}

static int reserve(writer* out, u64 extra)
{
    if (out->failed || extra > RSXB_MAX_FILE_SIZE ||
        out->size > RSXB_MAX_FILE_SIZE - extra) return out->failed = 1;
    u64 needed = out->size + extra;
    if (needed <= out->capacity) return 0;
    u64 next = out->capacity ? out->capacity : 4096;
    while (next < needed) {
        if (next > RSXB_MAX_FILE_SIZE / 2) { next = RSXB_MAX_FILE_SIZE; break; }
        next *= 2;
    }
    u8* grown = (u8*)realloc(out->data, (size_t)next);
    if (!grown) return out->failed = 1;
    out->data = grown; out->capacity = next; return 0;
}
static void put_bytes(writer* out, const void* data, u64 size)
{ if (!reserve(out, size)) { memcpy(out->data + out->size, data, (size_t)size); out->size += size; } }
static void put_u32(writer* out, u32 value)
{ u8 b[4]={(u8)value,(u8)(value>>8),(u8)(value>>16),(u8)(value>>24)}; put_bytes(out,b,4); }
static void put_u64(writer* out, u64 value)
{ put_u32(out,(u32)value); put_u32(out,(u32)(value>>32)); }
static void put_float(writer* out, float value)
{ u32 word; memcpy(&word,&value,4); put_u32(out,word); }

static const u8* take(reader* in, u64 size)
{
    if (in->failed || size > in->size - in->offset) { in->failed = 1; return NULL; }
    const u8* result=in->data+in->offset; in->offset+=size; return result;
}
static u32 get_u32(reader* in)
{ const u8* b=take(in,4); return b?(u32)b[0]|((u32)b[1]<<8)|((u32)b[2]<<16)|((u32)b[3]<<24):0; }
static u64 get_u64(reader* in) { u64 lo=get_u32(in), hi=get_u32(in); return lo|(hi<<32); }
static float get_float(reader* in) { u32 word=get_u32(in); float value; memcpy(&value,&word,4); return value; }

static u32 crc32(const void* data, u64 size)
{
    u32 crc=0xffffffffu; const u8* p=(const u8*)data;
    for (u64 i=0;i<size;++i) { crc^=p[i]; for(int bit=0;bit<8;++bit) crc=(crc>>1)^(0xedb88320u&-(int)(crc&1)); }
    return ~crc;
}

static void write_surface(writer* out, const rsx_surface_ref* s)
{
    put_u32(out,s->raw_offset); put_u32(out,s->resolved_offset); put_u32(out,s->width);
    put_u32(out,s->height); put_u32(out,s->pitch); put_u32(out,s->display_buffer_id);
    put_u32(out,s->format); put_u32(out,(u32)s->is_display|((u32)s->is_depth<<8));
}
static void read_surface(reader* in, rsx_surface_ref* s)
{
    memset(s,0,sizeof(*s)); s->raw_offset=get_u32(in); s->resolved_offset=get_u32(in);
    s->width=get_u32(in); s->height=get_u32(in); s->pitch=get_u32(in);
    s->display_buffer_id=get_u32(in); s->format=(rsx_portable_format)get_u32(in);
    u32 flags=get_u32(in); s->is_display=flags&1u; s->is_depth=(flags>>8)&1u;
    if (s->format>RSX_FORMAT_D32F || s->width>16384 || s->height>16384) in->failed=1;
}
static void write_blob(writer* out, const rsx_owned_blob* blob)
{
    u64 hash=blob->size
        ? (blob->hash?blob->hash:rsx_blob_hash64(blob->data,blob->size)):0;
    put_u64(out,blob->size); put_u64(out,hash);
    put_bytes(out,blob->data,blob->size);
}
static void read_blob(reader* in, rsx_owned_blob* blob)
{
    u64 size=get_u64(in), hash=get_u64(in);
    if (size>RSXB_MAX_BLOB_SIZE || size>RSXB_MAX_FILE_SIZE-in->allocation) { in->failed=1; return; }
    if (!size) { if (hash) in->failed=1; return; }
    const u8* data=take(in,size); if (!data) return;
    if (rsx_blob_hash64(data,size)!=hash || rsx_owned_blob_copy(blob,data,size)!=0) { in->failed=1; return; }
    in->allocation+=size;
}

#define PIPE_U32_FIELDS(X) \
 X(topology) X(vertex_layout) X(color_target_count) X(depth_format) X(color_write_mask) \
 X(blend_sfactor) X(blend_dfactor) X(blend_equation) X(blend_color) X(depth_func) \
 X(stencil_func) X(stencil_ref) X(stencil_mask) X(stencil_fail) X(stencil_zfail) \
 X(stencil_zpass) X(cull_face) X(front_face) X(alpha_func) X(alpha_ref)
static void write_pipeline(writer* out,const rsx_pipeline_key* p)
{
    put_u64(out,p->vertex_shader_hash); put_u64(out,p->fragment_shader_hash);
    for(u32 i=0;i<4;++i) put_u32(out,p->color_format[i]);
#define W(field) put_u32(out,p->field);
    PIPE_U32_FIELDS(W)
#undef W
    put_u32(out,(u32)p->blend_enable|((u32)p->depth_test_enable<<1)|
      ((u32)p->depth_write_enable<<2)|((u32)p->stencil_test_enable<<3)|
      ((u32)p->cull_enable<<4)|((u32)p->alpha_test_enable<<5)|
      ((u32)p->fragment_32bit_exports<<6));
}
static void read_pipeline(reader* in,rsx_pipeline_key* p)
{
    memset(p,0,sizeof(*p)); p->vertex_shader_hash=get_u64(in); p->fragment_shader_hash=get_u64(in);
    for(u32 i=0;i<4;++i) p->color_format[i]=get_u32(in);
#define R(field) p->field=get_u32(in);
    PIPE_U32_FIELDS(R)
#undef R
    u32 f=get_u32(in); p->blend_enable=f&1;p->depth_test_enable=(f>>1)&1;
    p->depth_write_enable=(f>>2)&1;p->stencil_test_enable=(f>>3)&1;p->cull_enable=(f>>4)&1;
    p->alpha_test_enable=(f>>5)&1;p->fragment_32bit_exports=(f>>6)&1;
    if(p->topology>RSX_TOPOLOGY_TRIANGLE_LIST||p->vertex_layout>RSX_VERTEX_LAYOUT_PACKED||p->color_target_count>4)in->failed=1;
}

static void write_texture(writer* out,const rsx_texture_source* t)
{
    put_u32(out,t->raw_offset);put_u32(out,t->resolved_offset);put_u32(out,t->width);
    put_u32(out,t->height);put_u32(out,t->pitch);put_u32(out,t->address);put_u32(out,t->control1);
    put_u32(out,t->border_color);put_u32(out,t->format);put_u32(out,t->flags);
    write_blob(out,&t->payload);
}
static void read_texture(reader* in,rsx_texture_source* t)
{
    memset(t,0,sizeof(*t));t->raw_offset=get_u32(in);t->resolved_offset=get_u32(in);
    t->width=get_u32(in);t->height=get_u32(in);t->pitch=get_u32(in);t->address=get_u32(in);
    t->control1=get_u32(in);t->border_color=get_u32(in);t->format=(rsx_texture_format)get_u32(in);
    t->flags=get_u32(in);
    if(t->format>RSX_TEXTURE_BC3||t->width>16384||t->height>16384)in->failed=1;
    if(t->flags&~RSX_TEXTURE_FLAG_UNNORMALIZED_COORDS)in->failed=1;
    read_blob(in,&t->payload);
    /* Render-to-texture bindings intentionally carry no duplicate payload.
     * Their resolved offset and dimensions identify a persistent surface
     * captured in the same batch; the consumer resolves that alias before
     * considering a standalone texture upload. */
    if(in->failed||t->format==RSX_TEXTURE_INVALID||!t->payload.size)return;
    u32 rows=t->height,row_bytes=t->width;
    if(t->format==RSX_TEXTURE_RGBA8)row_bytes*=4u;
    else if(t->format>=RSX_TEXTURE_BC1){u32 block=t->format==RSX_TEXTURE_BC1?8u:16u;row_bytes=((t->width+3u)/4u)*block;rows=(t->height+3u)/4u;}
    u32 pitch=t->pitch?t->pitch:row_bytes;
    if(!t->width||!t->height||pitch<row_bytes||
       (u64)(rows-1u)*pitch+row_bytes>t->payload.size)in->failed=1;
}

static void write_op(writer* out,const rsx_render_op* op)
{
    put_u32(out,op->type);put_u32(out,op->sequence);put_u32(out,op->color_count);
    for(u32 i=0;i<4;++i) write_surface(out,&op->color[i]);
    write_surface(out,&op->depth);
    for(u32 i=0;i<4;++i) put_u32(out,op->viewport[i]);
    for(u32 i=0;i<4;++i) put_u32(out,op->scissor[i]);
    if(op->type==RSX_RENDER_OP_CLEAR) {
        put_u32(out,op->data.clear.flags);
        for(u32 i=0;i<4;++i) put_float(out,op->data.clear.color[i]);
        put_float(out,op->data.clear.depth);put_u32(out,op->data.clear.stencil);
    } else {
      const rsx_draw_op_data* d=&op->data.draw;
      write_pipeline(out,&d->pipeline);put_u32(out,d->texture_count);
      for(u32 i=0;i<4;++i) write_texture(out,&d->textures[i]);
      put_u32(out,d->vertex_count);put_u32(out,d->index_count);
      write_blob(out,&d->vertex_data);write_blob(out,&d->index_data);
      write_blob(out,&d->vertex_constants);write_blob(out,&d->vertex_shader);
      write_blob(out,&d->fragment_shader);
    }
}
static void read_op(reader* in,rsx_render_op* op)
{
    op->type=(rsx_render_op_type)get_u32(in);op->sequence=get_u32(in);op->color_count=get_u32(in);
    if(op->type<RSX_RENDER_OP_CLEAR||op->type>RSX_RENDER_OP_DRAW||op->color_count>4){in->failed=1;return;}
    for(u32 i=0;i<4;++i) read_surface(in,&op->color[i]);
    read_surface(in,&op->depth);
    for(u32 i=0;i<4;++i) op->viewport[i]=get_u32(in);
    for(u32 i=0;i<4;++i) op->scissor[i]=get_u32(in);
    if(op->type==RSX_RENDER_OP_CLEAR) {
      op->data.clear.flags=get_u32(in);
      for(u32 i=0;i<4;++i) op->data.clear.color[i]=get_float(in);
      op->data.clear.depth=get_float(in);op->data.clear.stencil=(u8)get_u32(in);
    } else {
      rsx_draw_op_data* d=&op->data.draw;
      read_pipeline(in,&d->pipeline);d->texture_count=get_u32(in);
      if(d->texture_count>4)in->failed=1;
      for(u32 i=0;i<4;++i) read_texture(in,&d->textures[i]);
      d->vertex_count=get_u32(in);d->index_count=get_u32(in);
      read_blob(in,&d->vertex_data);read_blob(in,&d->index_data);
      read_blob(in,&d->vertex_constants);read_blob(in,&d->vertex_shader);
      read_blob(in,&d->fragment_shader);
      u64 stride=36u;
      if(d->pipeline.vertex_layout==RSX_VERTEX_LAYOUT_FLOAT4_X16) stride=256u;
      else if(d->pipeline.vertex_layout==RSX_VERTEX_LAYOUT_PACKED){
        u32 m=d->vertex_shader.size?rsx_vp_input_mask(d->vertex_shader.data,(u32)d->vertex_shader.size):0xFFFFu;
        stride=0; for(u32 b=0;b<16;++b) if(m&(1u<<b)) stride+=16u;
      }
      if(d->vertex_count>RSXB_MAX_OPERATIONS||
         (u64)d->vertex_count*stride>d->vertex_data.size||
         d->vertex_constants.size>(u64)RSX_BATCH_VP_CONSTANTS*16u||
         d->vertex_shader.size>16384u||d->fragment_shader.size>4096u)in->failed=1;
    }
}

static void write_batch(writer* out,const rsx_render_batch* b)
{
    put_u64(out,b->serial);put_u32(out,b->display_buffer_id);put_u32(out,b->flags);
    put_u32(out,b->operation_count);put_u32(out,b->surface_init_count);
    for(u32 i=0;i<b->operation_count;++i)write_op(out,&b->operations[i]);
    for(u32 i=0;i<b->surface_init_count;++i){write_surface(out,&b->surface_inits[i].surface);write_blob(out,&b->surface_inits[i].color_data);write_blob(out,&b->surface_inits[i].depth_stencil_data);}
}
static void read_batch(reader* in,rsx_render_batch* b)
{
    u64 serial=get_u64(in);rsx_render_batch_init(b,serial);b->display_buffer_id=get_u32(in);b->flags=get_u32(in);
    u32 operations=get_u32(in),inits=get_u32(in);if(operations>RSXB_MAX_OPERATIONS||inits>65536){in->failed=1;return;}
    for(u32 i=0;i<operations&&!in->failed;++i){rsx_render_op* op=rsx_render_batch_add_op(b,RSX_RENDER_OP_CLEAR);if(!op){in->failed=1;break;}read_op(in,op);}
    for(u32 i=0;i<inits&&!in->failed;++i){
      rsx_surface_init* init=rsx_render_batch_add_surface_init(b);if(!init){in->failed=1;break;}
      read_surface(in,&init->surface);read_blob(in,&init->color_data);read_blob(in,&init->depth_stencil_data);
      u32 texel=init->surface.format==RSX_FORMAT_RGBA16F?8u:
                init->surface.format==RSX_FORMAT_RGBA32F?16u:4u;
      u32 row=init->surface.width*texel,pitch=init->surface.pitch?init->surface.pitch:row;
      const rsx_owned_blob* blob=init->surface.is_depth?&init->depth_stencil_data:&init->color_data;
      if(!init->surface.width||!init->surface.height||pitch<row||!blob->size||
         (u64)(init->surface.height-1u)*pitch+row>blob->size||
         (init->surface.is_depth?init->color_data.size:init->depth_stencil_data.size))in->failed=1;
    }
}

int rsxb_write_file(const char* path,const rsx_render_batch* batches,u32 count,char* error,size_t error_size)
{
    if(!path||(!batches&&count)||count>RSXB_MAX_BATCHES){set_error(error,error_size,"invalid capture arguments");return -1;}
    writer out={0};u8 zero[RSXB_HEADER_SIZE]={0};put_bytes(&out,zero,sizeof(zero));
    for(u32 i=0;i<count&&!out.failed;++i)write_batch(&out,&batches[i]);
    if(out.failed){free(out.data);set_error(error,error_size,"capture exceeds limits or memory");return -1;}
    memcpy(out.data,k_magic,8);u64 payload=out.size-RSXB_HEADER_SIZE;
#define PATCH32(off,v) do{u32 _v=(u32)(v);out.data[off]=(u8)_v;out.data[off+1]=(u8)(_v>>8);out.data[off+2]=(u8)(_v>>16);out.data[off+3]=(u8)(_v>>24);}while(0)
    PATCH32(8,RSX_BATCH_FORMAT_VERSION);PATCH32(12,RSXB_HEADER_SIZE);PATCH32(16,RSXB_ENDIAN_MARKER);PATCH32(20,count);
    PATCH32(24,(u32)out.size);PATCH32(28,(u32)(out.size>>32));PATCH32(32,(u32)payload);PATCH32(36,(u32)(payload>>32));
    PATCH32(40,crc32(out.data+RSXB_HEADER_SIZE,payload));PATCH32(44,0);PATCH32(44,crc32(out.data,RSXB_HEADER_SIZE));
#undef PATCH32
    FILE* file=fopen(path,"wb");if(!file){set_error(error,error_size,"open %s: %s",path,strerror(errno));free(out.data);return -1;}
    int ok=fwrite(out.data,1,(size_t)out.size,file)==out.size&&fclose(file)==0;free(out.data);
    if(!ok){set_error(error,error_size,"write %s failed",path);return -1;}return 0;
}

int rsxb_read_file(const char* path,rsx_render_batch** batches,u32* count,char* error,size_t error_size)
{
    if(!path||!batches||!count){set_error(error,error_size,"invalid read arguments");return -1;}*batches=NULL;*count=0;
    FILE* file=fopen(path,"rb");if(!file){set_error(error,error_size,"open %s: %s",path,strerror(errno));return -1;}
    if(fseek(file,0,SEEK_END)!=0){fclose(file);set_error(error,error_size,"seek failed");return -1;}long end=ftell(file);
    if(end<(long)RSXB_HEADER_SIZE||(u64)end>RSXB_MAX_FILE_SIZE){fclose(file);set_error(error,error_size,"invalid capture size");return -1;}
    rewind(file);u8* data=(u8*)malloc((size_t)end);if(!data){fclose(file);set_error(error,error_size,"out of memory");return -1;}
    if(fread(data,1,(size_t)end,file)!=(size_t)end||fclose(file)!=0){free(data);set_error(error,error_size,"read failed");return -1;}
    if(memcmp(data,k_magic,8)!=0){free(data);set_error(error,error_size,"bad RSXB magic");return -1;}
    reader header={data,(u64)end,8,0,0};u32 version=get_u32(&header),header_size=get_u32(&header),marker=get_u32(&header),n=get_u32(&header);
    u64 total=get_u64(&header),payload=get_u64(&header);u32 payload_crc=get_u32(&header),header_crc=get_u32(&header);
    u8 saved[4];memcpy(saved,data+44,4);memset(data+44,0,4);u32 calculated_header=crc32(data,RSXB_HEADER_SIZE);memcpy(data+44,saved,4);
    if(version!=RSX_BATCH_FORMAT_VERSION||header_size!=RSXB_HEADER_SIZE||marker!=RSXB_ENDIAN_MARKER||n>RSXB_MAX_BATCHES||total!=(u64)end||payload!=total-header_size||header_crc!=calculated_header||payload_crc!=crc32(data+header_size,payload)){
        free(data);set_error(error,error_size,"invalid, corrupt, or unsupported RSXB header");return -1;}
    rsx_render_batch* result=(rsx_render_batch*)calloc(n?n:1,sizeof(*result));if(!result){free(data);set_error(error,error_size,"out of memory");return -1;}
    reader in={data+header_size,payload,0,0,0};u32 made=0;for(;made<n&&!in.failed;++made)read_batch(&in,&result[made]);
    if(in.failed||in.offset!=in.size){rsxb_free_batches(result,made+(made<n?1u:0u));free(data);set_error(error,error_size,"truncated, oversized, or corrupt RSXB payload");return -1;}
    free(data);*batches=result;*count=n;return 0;
}

void rsxb_free_batches(rsx_render_batch* batches,u32 count)
{ if(!batches)return;for(u32 i=0;i<count;++i)rsx_render_batch_destroy(&batches[i]);free(batches); }

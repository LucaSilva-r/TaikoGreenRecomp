/*
 * ps3recomp - D3D12 RSX Backend
 *
 * Translates RSX GPU state to D3D12 rendering commands.
 *
 * Phase 1 implementation:
 *   - Win32 window + D3D12 device + swap chain
 *   - Clear render target to RSX clear color
 *   - Present with vsync
 *   - Basic vertex-colored triangle rendering
 *
 * This file is C with COM calls (D3D12 is a COM API).
 * We use the C interface (__uuidof not available in C, so we
 * define GUIDs manually).
 */

#ifdef _WIN32

#include "rsx_d3d12_backend.h"
#include "rsx_primitives.h"
#include "rsx_recorder.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* D3D12 headers */
#include <d3d12.h>
#include <d3d12sdklayers.h>   /* ID3D12Debug / ID3D12InfoQueue */
#include <dxgi1_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

/* We need these GUIDs — define them here to avoid uuid.lib dependency */
#include <initguid.h>

/* Link libraries */
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define FRAME_COUNT         4   /* avoid vkd3d backbuffer-fence serialization */
#define UPLOAD_FRAME_COUNT  4   /* independently fenced upload/descriptor slices */
#define MAX_VERTICES     65536  /* per-frame vertex buffer (dbgfont submits
                                 * ~7.5k verts/frame; leave generous headroom) */
#define MAX_DRAWS         1024  /* per-frame draw records */
#define FP_SNAPSHOT_MAX   4096  /* maximum guest fragment-program bytecode */
/* Per-draw VP constant-buffer slot: vp_c[512] + posscale + posoffset
 * (514 vec4 = 8224 B) rounded up to D3D12's 256-byte CBV alignment.
 * Constants are snapshotted at RECORD time -- wave's passes each set their
 * own texScale/offset uniforms, so one per-frame snapshot ran every pass
 * with the LAST pass's constants. */
#define VP_CB_STRIDE      8448
#define VERTEX_STRIDE       36  /* bytes per host vertex: pos3 + col4 + uv2 */

typedef struct {
    u32 vb_byte_offset; /* offset into vb_mapped where this draw's verts live */
    u32 cb_slot;        /* original per-draw upload slot; stable if record moves */
    float tie_vz, tie_c258z, tie_c259z; /* CPU-side RTT_UNREVERSE key */
    u32 vertex_count;
    u32 topology;       /* D3D_PRIMITIVE_TOPOLOGY_* */
    int textured;       /* 1 = sample the bound font/atlas texture (dbgfont) */
    int is_vp;          /* 1 = real VP path: vb_byte_offset indexes vp_vb (float4) */
    /* VP path per-draw shader/texture state, captured at draw_arrays time. */
    u32 fp_addr;        /* SET_SHADER_PROGRAM value (guest FP ucode location)   */
    u32 fp_hash;        /* FP ucode hash at record time (PSO identity) */
    u16 fp_size;        /* bytes retained in s_fp_snapshots[] */
    u16 fp_snapshot;    /* immutable submission-time snapshot slot */
    u32 fifo_off;       /* FIFO ring offset this draw was decoded from */
    u32 seq;            /* global record order (trace probe) */
    int fp_exp32;       /* SET_SHADER_CONTROL 32-bit-exports bit at draw time   */
    u32 alpha_ctl;      /* alpha test: enable<<16 | (func&0xFF)<<8 | ref */
    u32 cmask;          /* D3D write mask from SET_COLOR_MASK at draw time
                         * (wave's sim passes write single lanes of the height
                         * maps; ignoring the mask stomped persistent state) */
    /* Per-unit textures (t0-t3): decompiled FPs sample up to 4 units
     * (demosaic's interpolation passes read 3). Captured at draw time. */
    struct {
        u32 off;        /* resolved vm offset (guest upload source), 0 = none */
        u32 raw;        /* raw RSX offset (offscreen-RT matching) */
        u32 w, h, fmt;  /* dims + RSX base format */
        u32 pitch;      /* CONTROL3 linear source row pitch */
        u32 address;    /* TEXTURE_ADDRESS: S/T/R wrap modes */
        u32 control1;   /* TEXTURE_CONTROL1: post-format component remap */
        u32 border_color;
        int set;
    } tex[4];
    int tex_rt[4];      /* pre-pass: OffRT index sampled by unit, -1 = none */
    int tex_slot;       /* legacy single-slot path (atlas); -1 = none */
    int vs_idx;         /* VPVSEntry slot for this draw's vertex program (-1 = primary) */
    int blend;          /* guest blend enable at draw time */
    u32 blend_sf;       /* low16 RGB, high16 alpha */
    u32 blend_df;       /* low16 RGB, high16 alpha */
    u32 blend_eq;       /* low16 RGB, high16 alpha */
    u32 blend_color;    /* guest 8-bit ARGB constant */
    int depth_test;     /* guest depth-test enable at draw time */
    int depth_mask;     /* guest depth-write enable at draw time */
    u32 depth_func;     /* CELL_GCM comparison enum (0x200..0x207) */
    int cull_enable;    /* guest face-culling state at draw time */
    u32 cull_face;      /* CELL_GCM_CULL_FRONT/BACK */
    u32 front_face;     /* CELL_GCM_CW/CCW */
    int stencil_test;
    u32 stencil_func, stencil_ref, stencil_mask;
    u32 stencil_fail, stencil_zfail, stencil_zpass;
    /* Render-to-texture: which colour surface this op targets. 0 = a display
     * buffer (the swap-chain backbuffer); else the resolved vm offset of an
     * offscreen surface (demosaic chains its effect passes through local-
     * memory buffers and composites from them). */
    u32 rt_off;
    u32 rt_raw;         /* surface offset BEFORE the display-buffer collapse:
                         * rt_off is 0 for every registered display buffer, so
                         * draws into buffer A and buffer B are indistinguishable
                         * there. Trace-only. */
    u32 rt_off2;        /* second colour target (MRT1), 0 = none */
    u32 rt_w, rt_h;     /* surface clip dims at record time (offscreen RT size) */
    u32 rt_fmt;         /* RSX surface colour format (SET_SURFACE_FORMAT [4:0]) */
    /* Guest viewport rect at draw time (target pixels). Sub-viewport layouts
     * (wave's debug tiles) position quads with this, not with constants --
     * forcing full-target viewports drew every tile window-sized. */
    u32 vp_x, vp_y, vp_w, vp_h;
    u32 sc_x, sc_y, sc_w, sc_h; /* guest scissor, independent of viewport */
    /* Ordered clear op (offscreen surfaces only; display clears stay the
     * frame-start backbuffer clear). is_clear records also set is_vp so the
     * legacy replay pass skips them. */
    int   is_clear;
    float cc[4];
} D3D12DrawRecord;

/* Fragment programs may contain inline constants that the guest patches at
 * one address between draw submissions.  Rendering is deferred until the
 * batch boundary, so retaining only fp_addr would make every recorded draw
 * see the last material written there.  Keep the exact program bytes present
 * at submission time in a parallel arena; draw-record reordering only moves
 * the small slot number. */
typedef struct {
    u32 size;
    u32 hash;
    u8  ucode[FP_SNAPSHOT_MAX];
} FPSnapshot;

/* Offscreen render target (render-to-texture). Persistent across frames --
 * pass N's output is sampled by pass N+1 and possibly by later frames. */
typedef struct {
    ID3D12Resource*       res;
    ID3D12Resource*       up;          /* init-upload staging (kept alive) */
    u32                   off, w, h;   /* raw RSX offset + dims */
    u32                   dxgi;        /* DXGI_FORMAT of the resource */
    D3D12_RESOURCE_STATES st;          /* tracked resource state */
    int                   used;        /* referenced this frame */
} OffRT;
#define MAX_OFF_RTS  16  /* demosaic double-buffers its 6-surface pass chain */
#define RT_SRV_BASE  5   /* SRV heap slots 5..20 hold offscreen-RT SRVs */
/* Per-draw SRV windows: each VP draw gets 4 consecutive descriptors (t0-t3)
 * so multi-unit fragment programs see all their textures (demosaic's
 * interpolation passes sample 3 units).  Keep one complete window set per
 * in-flight frame.  Shader-visible descriptors are GPU-consumed memory: the
 * CPU must not rewrite frame N's descriptors while its command list may still
 * be executing. */
#define DRAW_SRV_BASE 32
#define DRAW_SRV_PARITY_STRIDE (MAX_DRAWS * 4)
#define SRV_HEAP_SIZE (DRAW_SRV_BASE + UPLOAD_FRAME_COUNT * DRAW_SRV_PARITY_STRIDE)

/* Samplers are immutable once placed in this shader-visible heap.  Cache a
 * four-unit sampler table for each distinct per-draw RSX address-state tuple;
 * this avoids needing MAX_DRAWS*4 sampler descriptors (D3D12 shader-visible
 * sampler heaps are limited to 2048 descriptors) while still allowing every
 * draw to select wrap/clamp independently. */
#define VP_SAMPLER_SETS 64
typedef struct {
    u32 address[4];
    u32 border_color[4];
    int used;
} VPSamplerSet;

/* Per-frame VP texture slot: a guest texture uploaded for this frame's VP
 * draws (re-uploaded every frame -- gcm/cube's plasma animates in guest
 * memory).  The resource is exposed through each draw's own SRV window. */
typedef struct {
    ID3D12Resource* res;
    ID3D12Resource* up;
    u32 off, w, h, fmt, pitch; /* current uploaded contents */
    u64 source_hash;    /* raw guest source fingerprint at the last upload */
    u64 upload_fence;   /* last submission which reads this staging resource */
    int hash_valid;
    int uploaded;       /* staging resource was written in the current batch */
    int used;           /* referenced this frame */
} VPTexSlot;
/* Song Select uses 133 distinct t0 textures in one 451-op frame (plus the
 * offscreen passes' additional units).  Returning -1 here binds a null SRV,
 * which used to make every late Lumen group -- header, timer, footer and
 * nameplate -- disappear once the first 64 assets had been uploaded. */
#define VP_TEX_SLOTS 1024

/* Decompiled-VS cache: one entry per distinct vertex-program ucode seen at
 * draw time (hashed). Apps switch VPs between draws (gcm/cube: its MVP cube VP
 * vs dbgfont-gcm's text VP); compiling only the first left later draws
 * transformed by the wrong program (text offscreen). */
typedef struct {
    u32 hash;               /* FNV-1a of the ucode */
    ID3DBlob* vs;
    int uses_c03;
} VPVSEntry;
#define VP_VS_CACHE 16  /* wave uses 5+ distinct VPs; at 4 the cache
                         * thrashed every frame and eviction shifted slots
                         * under recorded vs_idx values -- the display mesh
                         * drew with the WRONG vertex program (sub-rect +
                         * edge slivers instead of fullscreen) */

/* Compiled guest-FP pipeline cache: decompiled VS (by cache slot) + decompiled
 * PS (fragment ucode at fp_addr). */
typedef struct {
    u32 fp_addr;
    int vs_idx;             /* VPVSEntry slot this PSO's VS came from */
    u32 vs_hash;            /* validates the slot hasn't been evicted */
    u32 gen;
    int blend;              /* guest blend enable at draw time (PSO key) */
    u32 blend_sf, blend_df, blend_eq;
    int depth_test;         /* guest depth-test enable (PSO key) */
    int depth_mask;         /* guest depth-write enable (PSO key) */
    u32 depth_func;         /* guest comparison function (PSO key) */
    int cull_enable;
    u32 cull_face, front_face;
    int nrt;                /* bound colour target count (PSO key)       */
    u32 rtfmt;              /* DXGI format of the colour targets (PSO key) */
    int exp32;              /* 32-bit-exports control bit (PSO key)       */
    u32 ucode_hash;         /* FNV-1a of the FP ucode: apps re-patch inline
                             * constants per frame (wave's stamp position/
                             * amplitude) -- address-only keying served the
                             * stale compile forever. */
    u32 cmask;              /* colour write mask (PSO key) */
    ID3D12PipelineState* pso;
} VPFPEntry;
#define VP_FP_CACHE 16

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    /* Window */
    HWND hwnd;
    u32  width;
    u32  height;
    int  window_closed;

    /* D3D12 core */
    ID3D12Device*              device;
    ID3D12CommandQueue*        cmd_queue;
    IDXGISwapChain3*           swap_chain;
    ID3D12DescriptorHeap*      rtv_heap;
    u32                        rtv_descriptor_size;
    ID3D12Resource*            render_targets[FRAME_COUNT];
    ID3D12CommandAllocator*    cmd_allocators[FRAME_COUNT];
    ID3D12GraphicsCommandList* cmd_list;

    /* Synchronization */
    ID3D12Fence* fence;
    HANDLE       fence_event;
    u64          fence_next;
    u64          fence_values[FRAME_COUNT];
    u32          frame_index;

    /* Pipeline */
    ID3D12RootSignature*  root_signature;
    ID3D12PipelineState*  pipeline_state;         /* triangle class — default */
    ID3D12PipelineState*  pipeline_state_lines;   /* line class */
    ID3D12PipelineState*  pipeline_state_points;  /* point class */

    /* Depth/stencil */
    ID3D12DescriptorHeap* dsv_heap;
    ID3D12Resource*       depth_buffer;

    /* Dynamic vertex buffer (upload heap) */
    ID3D12Resource*       vertex_buffer;
    D3D12_VERTEX_BUFFER_VIEW vb_view;
    void*                 vb_mapped;      /* persistently mapped */
    u32                   vb_offset;      /* current write position */

    int                   pipeline_ready; /* 1 if root sig + PSO created */

    /* Debug: copy the presented backbuffer to disk as BMP for the first N
     * frames (enabled by env CELLMARK_DUMP). Lets us verify what actually
     * rendered without racing the window/process lifetime. */
    ID3D12Resource*       readback_buf;
    u32                   readback_pitch;
    int                   dump_frames_left;

    /* Textured pipeline (dbgfont / 2D atlas quads). The font atlas is an 8-bit
     * coverage texture uploaded as R8_UNORM and sampled in the pixel shader. */
    ID3D12PipelineState*  pipeline_state_tex;   /* triangle + texture + blend */
    ID3D12DescriptorHeap* srv_heap;             /* shader-visible, 1 SRV slot  */
    ID3D12DescriptorHeap* sampler_heap;         /* shader-visible s0-s3 tables */
    ID3D12Resource*       tex_resource;         /* current atlas texture       */
    ID3D12Resource*       tex_upload;           /* staging buffer for uploads  */
    u32                   tex_w, tex_h;         /* dims of tex_resource         */
    int                   tex_ready;            /* 1 once an atlas is uploaded  */
    u32                   tex_src_offset;       /* guest RSX offset of atlas    */
    int                   tex_bound;            /* a texture is bound for draws */
    int                   tex_dirty;            /* re-upload needed this frame  */

    /* Real RSX vertex-program path: the captured VP is decompiled to HLSL and
     * used as the vertex shader, fed the raw float4 attrib0 + the vp_c[]
     * constant bank. This produces exact position + texcoord (vs. the frac
     * approximation). Used for the 2D/dbgfont quad draws. */
    ID3D12RootSignature*  vp_root_sig;          /* CBV(b0) + SRV table + sampler */
    ID3D12PipelineState*  pipeline_state_vp;    /* decompiled VS + atlas PS      */
    ID3D12PipelineState*  pipeline_state_vp_color; /* decompiled VS + colour PS (untextured 3D) */
    ID3D12Resource*       vp_vb;                /* raw float4 attrib0, per-frame */
    void*                 vp_vb_mapped;
    ID3D12Resource*       vp_vb_gpu;            /* immutable GPU-side frame copy */
    u32                   vp_vb_offset;
    ID3D12Resource*       vp_cb;                /* per-draw VP constant slots    */
    void*                 vp_cb_mapped;
    ID3D12Resource*       vp_cb_gpu;
    ID3D12Resource*       vp_fpcb;              /* per-draw FP texscale (b1)     */
    void*                 vp_fpcb_mapped;
    ID3D12Resource*       vp_fpcb_gpu;
    int                   vp_ready;             /* VS+PSO compiled ok            */
    u32                   vp_compiled_bytes;    /* ucode size when last compiled */
    ID3DBlob*             vp_vs_blob;           /* kept for guest-FP PSO builds  */
    u32                   vp_gen;               /* bumped per VP recompile       */
    int                   vp_uses_c03;          /* VS references vp_c[0..3]      */
    VPTexSlot             vp_tex[VP_TEX_SLOTS]; /* per-frame VP texture slots    */
    VPVSEntry             vp_vs[VP_VS_CACHE];   /* per-draw decompiled VS cache  */
    int                   vp_vs_n;
    VPFPEntry             vp_fp[VP_FP_CACHE];   /* guest-FP PSO cache            */
    int                   vp_fp_n;
    u32                   srv_inc;              /* CBV_SRV_UAV descriptor size   */
    u32                   sampler_inc;
    VPSamplerSet          sampler_sets[VP_SAMPLER_SETS];
    int                   sampler_set_n;
    /* VP path: latest texture bound per unit (t0-t3). */
    struct {
        u32 off, raw, w, h, fmt, pitch;
        u32 address, control1, border_color;
        int set;
    } cur_texs[4];

    /* Render-to-texture: offscreen RT pool + their RTV heap. */
    OffRT                 off_rt[MAX_OFF_RTS];
    ID3D12DescriptorHeap* rt_rtv_heap;          /* MAX_OFF_RTS RTVs (CPU only)   */

    /* Frame-parity double buffering for the per-draw upload streams (vp_vb
     * vertices, vp_cb constants, vp_fpcb texscales): records for frame N+1
     * are written while frame N's GPU work may still be reading -- writes go
     * to the other half. Toggled at the end of render_frame. */
    int                   vp_parity;

    /* Per-frame draw recording */
    int                   frame_recording; /* 1 if cmd list is open for recording */
    u32                   draw_count;      /* draws this frame */
    D3D12DrawRecord       draws[MAX_DRAWS];

    /* Pointer to current RSX state (set before draw calls) */
    const rsx_state*      current_rsx_state;

    /* Current frame state */
    float clear_color[4];  /* RGBA float */

    /* Stats */
    u64 frame_count;
    u64 last_fps_time;
    u32 fps;

    int initialized;
} D3D12State;

static D3D12State s_d3d;
static FPSnapshot s_fp_snapshots[MAX_DRAWS];
typedef struct {
    ID3D12Resource* res;
    u32 fmt;
    UINT mapping;
    int valid;
} SRVCacheEntry;
static SRVCacheEntry s_srv_cache[SRV_HEAP_SIZE];
static void srv_cache_invalidate_resource(ID3D12Resource* res);
char g_rsx_title_base[128] = "ps3recomp";
static u32 s_dbg_last_draws = 0;
/* F9 hotkey capture: arms a bounded BMP dump + per-draw RTT trace at the
 * moment the user sees the broken screen, instead of dumping the whole boot. */
static volatile int s_rtt_hotkey_frames = 0;
/* Player Entry model isolation.  These are deliberately hotkey-only and
 * scoped to Don-chan's material FP (0x01AA8E41): F10 separates face culling
 * from shader/texture failures, then F11 separates depth rejection. */
static volatile int s_dbg_body_nocull = 0;
static volatile int s_dbg_body_flip_front = 0;
static volatile int s_dbg_body_depth_always = 0;
static volatile int s_dbg_body_fixed_vs = 0;
static volatile int s_dbg_body_fixed_ps = 0;
static volatile int s_dbg_body_direct_rt = 0;
static volatile int s_dbg_body_isolate_offrt = 0;

/* CELL_GCM_TEXTURE_* address values, matching RPCS3's fragment_texture
 * decoding. D3D12 has no distinction for RSX's two ordinary clamp variants;
 * RPCS3 maps both to edge clamp as well. */
static D3D12_TEXTURE_ADDRESS_MODE vp_texture_address_mode(u32 mode)
{
    switch (mode & 0xFu) {
    case 1: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 3: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 6:
    case 7:
    case 8: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}

/* TEXTURE_CONTROL1 stores selectors/actions in RSX A/R/G/B order, while both
 * the D3D resource and HLSL use R/G/B/A.  Decode in RSX order, then permute
 * both the output channel and selected source into D3D order. */
static u32 vp_texture_component_mapping(u32 control1)
{
    static const u32 output_rsx_channel[4] = {1, 2, 3, 0}; /* D3D RGBA -> RSX RGBA indices */
    static const u32 source_to_d3d[4] = {3, 0, 1, 2};      /* RSX A,R,G,B -> D3D A,R,G,B */
    u32 m[4];
    for (u32 i = 0; i < 4; i++) {
        const u32 channel = output_rsx_channel[i];
        const u32 source = (control1 >> (channel * 2)) & 3u;
        const u32 action = (control1 >> (8 + channel * 2)) & 3u;
        if (action == 2u)
            m[i] = D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0 +
                   source_to_d3d[source];
        else if (action == 1u)
            m[i] = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
        else
            m[i] = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
    }
    return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(m[0], m[1], m[2], m[3]);
}

/* Return the immutable four-sampler table matching this draw. */
static int vp_sampler_set_for_draw(const D3D12DrawRecord* dr)
{
    u32 address[4], border[4];
    for (int u = 0; u < 4; u++) {
        address[u] = dr->tex[u].set
            ? (dr->tex[u].address & 0x000F0F0Fu)
            : 0x00030303u;
        /* Border colour is irrelevant unless an axis actually requests the
         * ordinary BORDER mode. Normalizing it keeps animation-time colour
         * state from creating redundant sampler-table identities. */
        int uses_border = ((address[u] & 0xFu) == 4) ||
                          (((address[u] >> 8) & 0xFu) == 4) ||
                          (((address[u] >> 16) & 0xFu) == 4);
        border[u] = (dr->tex[u].set && uses_border)
            ? dr->tex[u].border_color : 0;
    }

    for (int i = 0; i < s_d3d.sampler_set_n; i++) {
        int match = 1;
        for (int u = 0; u < 4; u++) {
            if (s_d3d.sampler_sets[i].address[u] != address[u] ||
                s_d3d.sampler_sets[i].border_color[u] != border[u]) {
                match = 0;
                break;
            }
        }
        if (match) return i;
    }

    if (!s_d3d.sampler_heap || !s_d3d.device ||
        s_d3d.sampler_set_n >= VP_SAMPLER_SETS) {
        static int warned = 0;
        if (!warned) {
            printf("[D3D12] sampler-table cache exhausted/unavailable; using set 0\n");
            warned = 1;
        }
        return s_d3d.sampler_set_n ? 0 : -1;
    }

    int idx = s_d3d.sampler_set_n++;
    VPSamplerSet* set = &s_d3d.sampler_sets[idx];
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.sampler_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
        s_d3d.sampler_heap, &h);
    h.ptr += (u64)idx * 4 * s_d3d.sampler_inc;
    for (int u = 0; u < 4; u++) {
        set->address[u] = address[u];
        set->border_color[u] = border[u];

        D3D12_SAMPLER_DESC sd = {0};
        sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = vp_texture_address_mode(address[u]);
        sd.AddressV = vp_texture_address_mode(address[u] >> 8);
        sd.AddressW = vp_texture_address_mode(address[u] >> 16);
        sd.MaxAnisotropy = 1;
        sd.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sd.MinLOD = 0.0f;
        sd.MaxLOD = D3D12_FLOAT32_MAX;
        sd.BorderColor[0] = (float)((border[u] >> 16) & 0xFFu) / 255.0f;
        sd.BorderColor[1] = (float)((border[u] >> 8) & 0xFFu) / 255.0f;
        sd.BorderColor[2] = (float)(border[u] & 0xFFu) / 255.0f;
        sd.BorderColor[3] = (float)((border[u] >> 24) & 0xFFu) / 255.0f;
        s_d3d.device->lpVtbl->CreateSampler(s_d3d.device, &sd, h);
        h.ptr += s_d3d.sampler_inc;
    }
    set->used = 1;
    printf("[D3D12] sampler set %d: addr=%05X,%05X,%05X,%05X\n",
           idx, address[0], address[1], address[2], address[3]);
    return idx;
}
/* Global record counter across every draw/clear record site, so the
 * trace shows one monotonic submission order. */
static u32 s_draw_seq = 0;

/* Exported for guest-side probes in lifted code (e.g. qsort), so CPU-side
 * events can be correlated with the draw submission order. */
u32 rsx_dbg_draw_seq(void) { return s_draw_seq; }
int rsx_dbg_trace_armed(void) { return (getenv("RTT_DUMP") || s_rtt_hotkey_frames > 0) ? 1 : 0; }
int rsx_dbg_capture_left(void) { return s_d3d.dump_frames_left; }
/* Set when the accumulating batch contains a DISPLAY clear: that clear is the
 * frame boundary and the next display clear presents the batch, so a flip
 * mid-batch must not present a partial frame (Taiko credits flickered
 * text/gradient/icon as alternating one-layer presents). Screens that never
 * clear the display (Taiko's boot test screen) leave this 0 and the flip
 * presents as usual. Cleared when render_frame consumes the batch. */
static volatile int s_batch_has_display_clear = 0;
/* Native SPURS output arenas are shared by a model's fill and outline draws.
 * FIFO decoding can span several ticker calls, but those calls still build one
 * presented RSX batch.  Draining the job chain at every ticker let a newer
 * animation generation overwrite the arena between fill and outline.  Drain
 * once, immediately before this batch's first indexed draw, then keep that
 * generation frozen until render_frame consumes the batch. */
static int s_spurs_drained_for_batch = 0;
/* The parity upload heaps are filled while FIFO methods are decoded, before
 * render_frame() is entered.  Waiting only inside render_frame is therefore
 * too late: two presents later the CPU can start overwriting parity N while
 * the GPU is still copying/reading parity N from the earlier frame.  Protect
 * the first upload write of each batch; render_frame keeps a fallback wait for
 * clear-only batches which never record vertices. */
static int s_upload_heaps_safe_for_batch = 0;
static u64 s_upload_parity_fence[UPLOAD_FRAME_COUNT];
/* RenderDoc capture aid for one-frame faults. F11 pauses after each completed
 * present. While paused, F12 is still RenderDoc's capture hotkey and also
 * releases exactly one present, so every stepped frame can be captured
 * without predicting the corrupt frame in advance. Interlocked access keeps
 * this safe whether the window pump and RSX presenter share a host thread or
 * not. */
static volatile LONG s_dbg_step_mode = 0;
static volatile LONG s_dbg_step_tokens = 0;
static u64 s_dbg_present_serial = 0;
/* Flips gated by that flag since the last display clear: several in a row
 * means the title stopped clearing (mode change) and the flip takes the
 * frame boundary back. Reset by every display clear. */
static volatile int s_gated_flips = 0;

/* RSX_PROFILE=1: low-overhead rolling timings for the synchronous host frame
 * path. Keep this opt-in so normal title behaviour is unchanged. */
typedef struct {
    u64 frames, draws, tex_uploads, tex_bytes;
    u64 srv_writes, srv_skips;
    u64 upload_wait_calls;
    double upload_wait_ms, upload_wait_max_ms;
    u64 vs_compiles, fp_compiles;
    double stage_ms[7];
    double stage_max_ms[7];
    double frame_ms, frame_max_ms;
    ULONGLONG window_start_ms;
} RSXProfile;
static RSXProfile s_prof;
static u64 s_prof_frame_tex_uploads;
static u64 s_prof_frame_tex_bytes;

static int rsx_profile_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* e = getenv("RSX_PROFILE");
        enabled = (e && e[0] != '0') ? 1 : 0;
    }
    return enabled;
}

static double qpc_elapsed_ms(LARGE_INTEGER begin, LARGE_INTEGER end)
{
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    return (double)(end.QuadPart - begin.QuadPart) * 1000.0 /
           (double)freq.QuadPart;
}

static void rsx_profile_frame(u32 draws, const LARGE_INTEGER* t)
{
    static const char* names[7] = {
        "gpuwait", "setup", "prepare", "record", "submit", "present", "move"
    };
    double total = qpc_elapsed_ms(t[0], t[7]);
    s_prof.frames++;
    s_prof.draws += draws;
    s_prof.tex_uploads += s_prof_frame_tex_uploads;
    s_prof.tex_bytes += s_prof_frame_tex_bytes;
    s_prof.frame_ms += total;
    if (total > s_prof.frame_max_ms) s_prof.frame_max_ms = total;
    for (int i = 0; i < 7; i++) {
        double ms = qpc_elapsed_ms(t[i], t[i + 1]);
        s_prof.stage_ms[i] += ms;
        if (ms > s_prof.stage_max_ms[i]) s_prof.stage_max_ms[i] = ms;
    }

    ULONGLONG now = GetTickCount64();
    if (!s_prof.window_start_ms) s_prof.window_start_ms = now;
    if (now - s_prof.window_start_ms < 1000) return;

    double elapsed = (double)(now - s_prof.window_start_ms);
    double n = s_prof.frames ? (double)s_prof.frames : 1.0;
    fprintf(stderr,
            "[RSXPROF] fps=%.2f frames=%llu draws=%.1f frame=%.3f/%.3fms "
            "tex=%.1f/frame %.2fMiB/s shaders=VS%llu/FP%llu\n",
            s_prof.frames * 1000.0 / elapsed,
            (unsigned long long)s_prof.frames, s_prof.draws / n,
            s_prof.frame_ms / n, s_prof.frame_max_ms,
            s_prof.tex_uploads / n,
            s_prof.tex_bytes * 1000.0 / elapsed / (1024.0 * 1024.0),
            (unsigned long long)s_prof.vs_compiles,
            (unsigned long long)s_prof.fp_compiles);
    fprintf(stderr, "[RSXPROF] descriptors write=%.1f/frame skip=%.1f/frame\n",
            s_prof.srv_writes / n, s_prof.srv_skips / n);
    fprintf(stderr, "[RSXPROF] upload-reuse wait=%.3fms/frame max=%.3fms calls=%llu\n",
            s_prof.upload_wait_ms / n, s_prof.upload_wait_max_ms,
            (unsigned long long)s_prof.upload_wait_calls);
    fprintf(stderr, "[RSXPROF] stages avg/max:");
    for (int i = 0; i < 7; i++)
        fprintf(stderr, " %s=%.3f/%.3f", names[i],
                s_prof.stage_ms[i] / n, s_prof.stage_max_ms[i]);
    fprintf(stderr, "%s", "\n");

    memset(&s_prof, 0, sizeof(s_prof));
    s_prof.window_start_ms = now;
}

/* ---------------------------------------------------------------------------
 * Win32 window
 * -----------------------------------------------------------------------*/

static HANDLE s_window_thread;

#define WM_RSX_UPDATE_TITLE (WM_APP + 1)

static LRESULT CALLBACK d3d12_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_RSX_UPDATE_TITLE: {
        char* title = (char*)lp;
        if (title) {
            SetWindowTextA(hwnd, title);
            free(title);
        }
        return 0;
    }
    case WM_CLOSE:
        s_d3d.window_closed = 1;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_F9) {
            extern void taiko_lumen_trace_arm(void);
            taiko_lumen_trace_arm();
            s_d3d.dump_frames_left += 120;
            s_rtt_hotkey_frames = 5;
            fprintf(stderr, "[HOTKEY] F9: dumping next 120 frames + tracing 5\n");
        }
        if (wp == VK_F11 && !(lp & (1u << 30))) {
            if (InterlockedCompareExchange(&s_dbg_step_mode, 0, 0)) {
                InterlockedExchange(&s_dbg_step_mode, 0);
                InterlockedIncrement(&s_dbg_step_tokens);
                fprintf(stderr, "[FRAMESTEP] disabled; running freely\n");
            } else {
                InterlockedExchange(&s_dbg_step_tokens, 0);
                InterlockedExchange(&s_dbg_step_mode, 1);
                fprintf(stderr,
                        "[FRAMESTEP] enabled; pausing after each present "
                        "(F12=capture+step, F11=resume)\n");
            }
        }
        if (wp == VK_F12 && !(lp & (1u << 30)) &&
            InterlockedCompareExchange(&s_dbg_step_mode, 0, 0)) {
            InterlockedIncrement(&s_dbg_step_tokens);
            fprintf(stderr, "[FRAMESTEP] F12 released one frame\n");
        }
        if (wp == VK_F8) {
            s_dbg_body_nocull = !s_dbg_body_nocull;
            s_d3d.vp_gen++;
            fprintf(stderr, "[HOTKEY] F8: Don-chan body culling %s\n",
                    s_dbg_body_nocull ? "disabled" : "restored");
        }
        if (wp == VK_F7) {
            s_dbg_body_flip_front = !s_dbg_body_flip_front;
            s_d3d.vp_gen++;
            fprintf(stderr, "[HOTKEY] F7: Don-chan body front winding %s\n",
                    s_dbg_body_flip_front ? "flipped" : "restored");
        }
        if (wp == VK_F6) {
            s_dbg_body_fixed_vs = !s_dbg_body_fixed_vs;
            s_d3d.vp_gen++;
            fprintf(stderr, "[HOTKEY] F6: Don-chan body vertex shader %s\n",
                    s_dbg_body_fixed_vs ? "fullscreen probe" : "restored");
        }
        if (wp == VK_F5) {
            s_dbg_body_fixed_ps = (s_dbg_body_fixed_ps + 1) % 3;
            s_d3d.vp_gen++;
            fprintf(stderr, "[HOTKEY] F5: Don-chan body pixel shader %s\n",
                    s_dbg_body_fixed_ps == 1 ? "direct texture probe" :
                    s_dbg_body_fixed_ps == 2 ? "UV colour probe" : "restored");
        }
        if (wp == VK_F4) {
            s_dbg_body_direct_rt = !s_dbg_body_direct_rt;
            fprintf(stderr, "[HOTKEY] F4: Don-chan body target %s\n",
                    s_dbg_body_direct_rt ? "direct backbuffer probe" : "restored");
        }
        if (wp == VK_F3) {
            s_dbg_body_isolate_offrt = !s_dbg_body_isolate_offrt;
            fprintf(stderr, "[HOTKEY] F3: Don-chan offscreen pass %s\n",
                    s_dbg_body_isolate_offrt
                        ? "stops after body draws" : "restored");
        }
        if (wp == VK_F10) {
            if (!(lp & (1u << 30))) {
                const char* path = getenv("RSX_BATCH_CAPTURE");
                const char* frames = getenv("RSX_BATCH_CAPTURE_FRAMES");
                rsx_recorder_arm_capture(path && path[0] ? path : "rsx_capture.rsxb",
                    frames ? (u32)strtoul(frames, NULL, 0) : 1u);
                fprintf(stderr, "[HOTKEY] F10: armed portable RSX capture\n");
            }
        }
        /* F11 is reserved for frame stepping. Keep this old depth probe on a
         * separate key: forcing ALWAYS makes the expanded outline shell draw
         * through Don-chan's fill and looks exactly like mesh corruption. */
        if (wp == VK_F2) {
            s_dbg_body_depth_always = !s_dbg_body_depth_always;
            s_d3d.vp_gen++;
            fprintf(stderr, "[HOTKEY] F2: Don-chan body depth %s\n",
                    s_dbg_body_depth_always ? "ALWAYS" : "restored");
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void debug_frame_step_wait(void)
{
    if (!InterlockedCompareExchange(&s_dbg_step_mode, 0, 0)) return;

    fprintf(stderr,
            "[FRAMESTEP] paused after present %llu; "
            "F12=capture+step, F11=resume\n",
            (unsigned long long)s_dbg_present_serial);
    fflush(stderr);

    while (InterlockedCompareExchange(&s_dbg_step_mode, 0, 0) &&
           !s_d3d.window_closed) {
        LONG tokens = InterlockedCompareExchange(&s_dbg_step_tokens, 0, 0);
        if (tokens > 0) {
            InterlockedDecrement(&s_dbg_step_tokens);
            break;
        }

        /* If render_frame() happens to run on the window-owning thread, keep
         * dispatching hotkeys while paused. On the usual ticker thread this
         * queue is empty and the normal owner continues pumping it. */
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                s_d3d.window_closed = 1;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(1);
    }
}

static HWND create_window(u32 width, u32 height, const char* title)
{
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = d3d12_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "ps3recomp_d3d12";
    RegisterClassExA(&wc);

    RECT wr = {0, 0, (LONG)width, (LONG)height};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    return CreateWindowExA(
        0, "ps3recomp_d3d12",
        title ? title : "ps3recomp (D3D12)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, GetModuleHandle(NULL), NULL);
}

typedef struct {
    u32 width, height;
    const char* title;
    HANDLE ready;
} D3D12WindowThreadArgs;

static DWORD WINAPI d3d12_window_thread(LPVOID opaque)
{
    D3D12WindowThreadArgs* args = (D3D12WindowThreadArgs*)opaque;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    s_d3d.hwnd = create_window(args->width, args->height, args->title);
    SetEvent(args->ready);
    if (!s_d3d.hwnd) return 1;

    MSG msg;
    while (!s_d3d.window_closed) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                s_d3d.window_closed = 1;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        /* GetMessage's wineserver wait can contend with vkd3d presentation
         * even from this separate owner thread.  Input remains queued while
         * sleeping; a 20 Hz pump keeps keys/resize responsive without keeping
         * the Wine queue machinery continuously active. */
        if (!s_d3d.window_closed) Sleep(50);
    }
    s_d3d.window_closed = 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * D3D12 initialization
 * -----------------------------------------------------------------------*/

static int init_d3d12(u32 width, u32 height)
{
    HRESULT hr;

    /* Enable debug layer in debug builds */
    /* Debug layer: on in debug builds, or in any build when D3D12_DBG is set
     * (so a Release run can capture exact PSO/validation errors). */
    if (
#ifdef NDEBUG
        getenv("D3D12_DBG")
#else
        1
#endif
    ) {
        ID3D12Debug* debug_controller = NULL;
        hr = D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&debug_controller);
        if (SUCCEEDED(hr) && debug_controller) {
            debug_controller->lpVtbl->EnableDebugLayer(debug_controller);
            debug_controller->lpVtbl->Release(debug_controller);
            printf("[D3D12] Debug layer enabled\n");
        } else {
            printf("[D3D12] Debug layer requested but unavailable (0x%08lX) -- "
                   "install 'Graphics Tools' optional feature\n", hr);
        }
    }

    /* Create DXGI factory */
    IDXGIFactory4* factory = NULL;
    hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateDXGIFactory1 failed (0x%08lX)\n", hr);
        return -1;
    }

    /* Create D3D12 device. On a dual-GPU laptop a NULL adapter usually lands on
     * the integrated GPU; explicitly pick the high-performance (discrete)
     * adapter via IDXGIFactory6. Set CELLMARK_IGPU to force the low-power one
     * (for A/B testing a suspected iGPU-driver stall). */
    {
        IDXGIFactory6* factory6 = NULL;
        if (SUCCEEDED(factory->lpVtbl->QueryInterface(
                factory, &IID_IDXGIFactory6, (void**)&factory6))) {
            DXGI_GPU_PREFERENCE pref = getenv("CELLMARK_IGPU")
                ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
                : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
            IDXGIAdapter1* adapter = NULL;
            for (UINT ai = 0; factory6->lpVtbl->EnumAdapterByGpuPreference(
                     factory6, ai, pref, &IID_IDXGIAdapter1, (void**)&adapter)
                     != DXGI_ERROR_NOT_FOUND; ai++) {
                DXGI_ADAPTER_DESC1 ad;
                adapter->lpVtbl->GetDesc1(adapter, &ad);
                if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                    SUCCEEDED(D3D12CreateDevice((IUnknown*)adapter,
                        D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                        (void**)&s_d3d.device))) {
                    printf("[D3D12] adapter: %ls (VRAM %llu MB)\n", ad.Description,
                           (unsigned long long)(ad.DedicatedVideoMemory >> 20));
                    adapter->lpVtbl->Release(adapter);
                    break;
                }
                adapter->lpVtbl->Release(adapter);
                adapter = NULL;
            }
            factory6->lpVtbl->Release(factory6);
        }
    }
    if (!s_d3d.device) {
        hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
                               &IID_ID3D12Device, (void**)&s_d3d.device);
        if (FAILED(hr)) {
            printf("[D3D12] ERROR: D3D12CreateDevice failed (0x%08lX)\n", hr);
            factory->lpVtbl->Release(factory);
            return -1;
        }
        printf("[D3D12] Device created on default adapter (feature level 11.0)\n");
    }

    /* Create command queue */
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = s_d3d.device->lpVtbl->CreateCommandQueue(
        s_d3d.device, &queue_desc, &IID_ID3D12CommandQueue, (void**)&s_d3d.cmd_queue);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateCommandQueue failed\n");
        factory->lpVtbl->Release(factory);
        return -1;
    }

    /* Create swap chain */
    DXGI_SWAP_CHAIN_DESC1 sc_desc = {0};
    sc_desc.Width = width;
    sc_desc.Height = height;
    sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = FRAME_COUNT;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* swap_chain1 = NULL;
    hr = factory->lpVtbl->CreateSwapChainForHwnd(
        factory, (IUnknown*)s_d3d.cmd_queue,
        s_d3d.hwnd, &sc_desc, NULL, NULL, &swap_chain1);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateSwapChainForHwnd failed (0x%08lX)\n", hr);
        factory->lpVtbl->Release(factory);
        return -1;
    }

    /* Disable Alt+Enter fullscreen toggle */
    factory->lpVtbl->MakeWindowAssociation(factory, s_d3d.hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->lpVtbl->Release(factory);

    /* Query SwapChain3 interface */
    hr = swap_chain1->lpVtbl->QueryInterface(
        swap_chain1, &IID_IDXGISwapChain3, (void**)&s_d3d.swap_chain);
    swap_chain1->lpVtbl->Release(swap_chain1);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: QueryInterface for SwapChain3 failed\n");
        return -1;
    }

    s_d3d.frame_index = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);

    /* Create RTV descriptor heap */
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {0};
    rtv_heap_desc.NumDescriptors = FRAME_COUNT;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
        s_d3d.device, &rtv_heap_desc, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.rtv_heap);
    if (FAILED(hr)) return -1;

    s_d3d.rtv_descriptor_size = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    /* Create RTVs for each frame */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &rtv_handle);

    for (u32 i = 0; i < FRAME_COUNT; i++) {
        hr = s_d3d.swap_chain->lpVtbl->GetBuffer(
            s_d3d.swap_chain, i, &IID_ID3D12Resource, (void**)&s_d3d.render_targets[i]);
        if (FAILED(hr)) return -1;

        s_d3d.device->lpVtbl->CreateRenderTargetView(
            s_d3d.device, s_d3d.render_targets[i], NULL, rtv_handle);
        rtv_handle.ptr += s_d3d.rtv_descriptor_size;
    }

    /* ---------------------------------------------------------------
     * Depth/stencil buffer
     * 24-bit depth + 8-bit stencil (DXGI_FORMAT_D24_UNORM_S8_UINT).
     * One shared depth texture across both frames — RSX games on PS3
     * typically use a single zeta surface.
     * ---------------------------------------------------------------*/
    {
        D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {0};
        dsv_heap_desc.NumDescriptors = 1;
        dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
            s_d3d.device, &dsv_heap_desc, &IID_ID3D12DescriptorHeap,
            (void**)&s_d3d.dsv_heap);
        if (FAILED(hr)) {
            printf("[D3D12] DSV heap creation failed\n");
            return -1;
        }

        D3D12_HEAP_PROPERTIES heap_props = {0};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC depth_desc = {0};
        depth_desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        /* Sized to cover the LARGEST render target, not just the window:
         * D3D12 clips rasterization to the smallest bound attachment, and
         * this DSV is shared by every pass -- with a window-sized depth
         * buffer, draws into larger offscreen RTs (wave's 1920x1080 input
         * image) were silently cropped to the window rect. */
        depth_desc.Width              = 2048;
        depth_desc.Height             = 2048;
        depth_desc.DepthOrArraySize   = 1;
        depth_desc.MipLevels          = 1;
        depth_desc.Format             = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_desc.SampleDesc.Count   = 1;
        depth_desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depth_desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE depth_clear = {0};
        depth_clear.Format                       = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_clear.DepthStencil.Depth           = 1.0f;
        depth_clear.DepthStencil.Stencil         = 0;

        hr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &heap_props, D3D12_HEAP_FLAG_NONE,
            &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear,
            &IID_ID3D12Resource, (void**)&s_d3d.depth_buffer);
        if (FAILED(hr)) {
            printf("[D3D12] Depth buffer creation failed (0x%08lX)\n", hr);
            return -1;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {0};
        dsv_desc.Format         = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsv_desc.ViewDimension  = D3D12_DSV_DIMENSION_TEXTURE2D;

        D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
        s_d3d.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.dsv_heap, &dsv_handle);
        s_d3d.device->lpVtbl->CreateDepthStencilView(
            s_d3d.device, s_d3d.depth_buffer, &dsv_desc, dsv_handle);

        printf("[D3D12] Depth buffer created (%ux%u D24S8)\n", width, height);
    }

    /* Create command allocators and command list */
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        hr = s_d3d.device->lpVtbl->CreateCommandAllocator(
            s_d3d.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void**)&s_d3d.cmd_allocators[i]);
        if (FAILED(hr)) return -1;
    }

    hr = s_d3d.device->lpVtbl->CreateCommandList(
        s_d3d.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        s_d3d.cmd_allocators[0], NULL,
        &IID_ID3D12GraphicsCommandList, (void**)&s_d3d.cmd_list);
    if (FAILED(hr)) return -1;

    /* Close the command list (it starts in recording state) */
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);

    /* Create fence */
    hr = s_d3d.device->lpVtbl->CreateFence(
        s_d3d.device, 0, D3D12_FENCE_FLAG_NONE,
        &IID_ID3D12Fence, (void**)&s_d3d.fence);
    if (FAILED(hr)) return -1;

    s_d3d.fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    s_d3d.fence_next = 0;
    memset(s_d3d.fence_values, 0, sizeof(s_d3d.fence_values));

    /* ---------------------------------------------------------------
     * Create root signature with 16 root constants (one mat4 MVP at b0).
     * Visible only to the vertex shader — pixel shader doesn't need it.
     * ---------------------------------------------------------------*/
    {
        /* param 0: mat4 MVP as 16 root constants at b0 (vertex shader).
         * param 1: one SRV (t0) descriptor table for the atlas texture (pixel).
         * static sampler s0: linear clamp. The plain (untextured) PSO simply
         * never references t0/s0, so it ignores them. */
        D3D12_DESCRIPTOR_RANGE srv_range = {0};
        srv_range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors     = 1;
        srv_range.BaseShaderRegister = 0;   /* t0 */
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER root_params[2] = {0};
        root_params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        root_params[0].Constants.ShaderRegister = 0;   /* b0 */
        root_params[0].Constants.RegisterSpace  = 0;
        root_params[0].Constants.Num32BitValues = 16;  /* mat4 */
        root_params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;
        root_params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_params[1].DescriptorTable.NumDescriptorRanges = 1;
        root_params[1].DescriptorTable.pDescriptorRanges   = &srv_range;
        root_params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp = {0};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister   = 0;   /* s0 */
        samp.MaxLOD           = D3D12_FLOAT32_MAX;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc = {0};
        rs_desc.NumParameters     = 2;
        rs_desc.pParameters       = root_params;
        rs_desc.NumStaticSamplers = 1;
        rs_desc.pStaticSamplers   = &samp;
        rs_desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ID3DBlob* signature_blob = NULL;
        ID3DBlob* error_blob = NULL;
        hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                          &signature_blob, &error_blob);
        if (FAILED(hr)) {
            printf("[D3D12] Root signature serialization failed: %s\n",
                   error_blob ? (const char*)error_blob->lpVtbl->GetBufferPointer(error_blob) : "?");
            if (error_blob) error_blob->lpVtbl->Release(error_blob);
            return -1;
        }

        hr = s_d3d.device->lpVtbl->CreateRootSignature(
            s_d3d.device, 0,
            signature_blob->lpVtbl->GetBufferPointer(signature_blob),
            signature_blob->lpVtbl->GetBufferSize(signature_blob),
            &IID_ID3D12RootSignature, (void**)&s_d3d.root_signature);
        signature_blob->lpVtbl->Release(signature_blob);
        if (FAILED(hr)) {
            printf("[D3D12] Root signature creation failed\n");
            return -1;
        }
    }

    /* ---------------------------------------------------------------
     * Compile shaders and create PSO
     * ---------------------------------------------------------------*/
    {
        /* Basic vertex-colored shader.
         * The MVP matrix arrives via root constants as 4 vec4 columns
         * (PS3/OpenGL column-major convention). We multiply explicitly so
         * we don't depend on HLSL's matrix packing — matches PS3 semantics
         * `gl_Position = MVP * vec4(pos, 1.0)`. */
        static const char vs_hlsl[] =
            "cbuffer cb0 : register(b0) {\n"
            "    float4 mvp_col0;\n"
            "    float4 mvp_col1;\n"
            "    float4 mvp_col2;\n"
            "    float4 mvp_col3;\n"
            "};\n"
            "struct VSInput  { float3 pos : POSITION; float4 col : COLOR; };\n"
            "struct VSOutput { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
            "VSOutput main(VSInput i) {\n"
            "    VSOutput o;\n"
            "    float4 p = float4(i.pos, 1.0);\n"
            "    o.pos = mvp_col0 * p.x + mvp_col1 * p.y + mvp_col2 * p.z + mvp_col3 * p.w;\n"
            "    o.col = i.col;\n"
            "    return o;\n"
            "}\n";
        static const char ps_hlsl[] =
            "struct PSInput { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
            "float4 main(PSInput i) : SV_TARGET { return i.col; }\n";

        ID3DBlob* vs_blob = NULL;
        ID3DBlob* ps_blob = NULL;
        ID3DBlob* err = NULL;

        hr = D3DCompile(vs_hlsl, sizeof(vs_hlsl) - 1, "vs_basic", NULL, NULL,
                        "main", "vs_5_0", 0, 0, &vs_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] VS compile failed: %s\n",
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "unknown");
            if (err) err->lpVtbl->Release(err);
        }

        hr = D3DCompile(ps_hlsl, sizeof(ps_hlsl) - 1, "ps_basic", NULL, NULL,
                        "main", "ps_5_0", 0, 0, &ps_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] PS compile failed: %s\n",
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "unknown");
            if (err) err->lpVtbl->Release(err);
        }

        if (vs_blob && ps_blob) {
            D3D12_INPUT_ELEMENT_DESC input_layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {0};
            pso_desc.pRootSignature = s_d3d.root_signature;
            pso_desc.VS.pShaderBytecode = vs_blob->lpVtbl->GetBufferPointer(vs_blob);
            pso_desc.VS.BytecodeLength = vs_blob->lpVtbl->GetBufferSize(vs_blob);
            pso_desc.PS.pShaderBytecode = ps_blob->lpVtbl->GetBufferPointer(ps_blob);
            pso_desc.PS.BytecodeLength = ps_blob->lpVtbl->GetBufferSize(ps_blob);
            pso_desc.InputLayout.pInputElementDescs = input_layout;
            pso_desc.InputLayout.NumElements = 3;
            pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
                D3D12_COLOR_WRITE_ENABLE_ALL;
            pso_desc.SampleMask = UINT_MAX;
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso_desc.NumRenderTargets = 1;
            pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
            /* Depth test enabled, write enabled, LESS func.
             * Games with depth_test_enable=0 in RSX state can still render —
             * LESS just means new-z must be less than existing — but future
             * work should mirror RSX depth state into a PSO cache. */
            pso_desc.DepthStencilState.DepthEnable    = TRUE;
            pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            pso_desc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            pso_desc.DepthStencilState.StencilEnable  = FALSE;
            pso_desc.SampleDesc.Count = 1;

            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state);
            if (SUCCEEDED(hr)) {
                s_d3d.pipeline_ready = 1;
                printf("[D3D12] Pipeline state created (triangle class)\n");
            } else {
                printf("[D3D12] PSO TRIANGLE creation failed (0x%08lX)\n", hr);
            }

            /* Line-class PSO — same shader, LINE topology type. */
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_lines);
            if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (line class)\n");
            else printf("[D3D12] PSO LINE creation failed (0x%08lX)\n", hr);

            /* Point-class PSO. */
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_points);
            if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (point class)\n");
            else printf("[D3D12] PSO POINT creation failed (0x%08lX)\n", hr);

            vs_blob->lpVtbl->Release(vs_blob);
            ps_blob->lpVtbl->Release(ps_blob);
        }

        /* -----------------------------------------------------------------
         * Textured triangle PSO (dbgfont / 2D atlas quads). Same MVP VS but
         * carrying UV; the PS samples the single-channel atlas as coverage
         * and modulates the vertex color, with straight alpha blending so
         * glyph edges composite over the framebuffer. Depth off (2D overlay).
         * ----------------------------------------------------------------*/
        {
            static const char vs_tex[] =
                "cbuffer cb0 : register(b0){float4 c0;float4 c1;float4 c2;float4 c3;};\n"
                "struct VSIn { float3 pos:POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "struct VSOut{ float4 pos:SV_POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "VSOut main(VSIn i){ VSOut o; float4 p=float4(i.pos,1.0);\n"
                "  o.pos=c0*p.x+c1*p.y+c2*p.z+c3*p.w; o.col=i.col; o.uv=i.uv; return o; }\n";
            static const char ps_tex[] =
                "Texture2D tex : register(t0);\n"
                "SamplerState smp : register(s0);\n"
                "struct PSIn{ float4 pos:SV_POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "float4 main(PSIn i):SV_TARGET{\n"
                /* dbgfont texcoords are compressed ~10x (U) / ~8x (V) vs. the
                 * atlas; scale to recover glyph cells. Exact per-glyph offset
                 * still being calibrated. */
                "  float2 uv2 = float2(i.uv.x*10.0, i.uv.y*8.0 + 0.59);\n"
                "  float cov = tex.Sample(smp, uv2).r;\n"
                "  return float4(i.col.rgb, i.col.a * cov); }\n";

            ID3DBlob *vtb = NULL, *ptb = NULL, *e2 = NULL;
            hr = D3DCompile(vs_tex, sizeof(vs_tex) - 1, "vs_tex", NULL, NULL, "main", "vs_5_0", 0, 0, &vtb, &e2);
            if (FAILED(hr)) printf("[D3D12] VS(tex) compile failed: %s\n", e2 ? (const char*)e2->lpVtbl->GetBufferPointer(e2) : "?");
            hr = D3DCompile(ps_tex, sizeof(ps_tex) - 1, "ps_tex", NULL, NULL, "main", "ps_5_0", 0, 0, &ptb, &e2);
            if (FAILED(hr)) printf("[D3D12] PS(tex) compile failed: %s\n", e2 ? (const char*)e2->lpVtbl->GetBufferPointer(e2) : "?");

            if (vtb && ptb) {
                D3D12_INPUT_ELEMENT_DESC il[] = {
                    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                };
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
                pd.pRootSignature = s_d3d.root_signature;
                pd.VS.pShaderBytecode = vtb->lpVtbl->GetBufferPointer(vtb);
                pd.VS.BytecodeLength  = vtb->lpVtbl->GetBufferSize(vtb);
                pd.PS.pShaderBytecode = ptb->lpVtbl->GetBufferPointer(ptb);
                pd.PS.BytecodeLength  = ptb->lpVtbl->GetBufferSize(ptb);
                pd.InputLayout.pInputElementDescs = il;
                pd.InputLayout.NumElements = 3;
                pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pd.BlendState.RenderTarget[0].BlendEnable    = TRUE;
                pd.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
                pd.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
                pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
                pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                pd.SampleMask = UINT_MAX;
                pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                pd.NumRenderTargets = 1;
                pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
                pd.DepthStencilState.DepthEnable = FALSE;
                pd.DepthStencilState.StencilEnable = FALSE;
                pd.SampleDesc.Count = 1;
                hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                    s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_tex);
                if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (textured class)\n");
                else printf("[D3D12] PSO TEX creation failed (0x%08lX)\n", hr);
            }
            if (vtb) vtb->lpVtbl->Release(vtb);
            if (ptb) ptb->lpVtbl->Release(ptb);
        }

        /* SRV descriptor heap (shader-visible). Layout: slot 0 = legacy atlas,
         * slots 5-20 = legacy offscreen-RT views, and DRAW_SRV_BASE onward =
         * two parity-isolated sets of four descriptors per recorded draw.
         * All slots start null so any 4-wide table window is valid on tier-1
         * hardware. */
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
            hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = SRV_HEAP_SIZE;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
                s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.srv_heap);
            if (FAILED(hr)) printf("[D3D12] SRV heap creation failed (0x%08lX)\n", hr);
            else {
                s_d3d.srv_inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
                    s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                D3D12_SHADER_RESOURCE_VIEW_DESC nv = {0};
                nv.Format = DXGI_FORMAT_R8_UNORM;
                nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                nv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                nv.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE hh;
                s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &hh);
                for (int _i = 0; _i < SRV_HEAP_SIZE; _i++) {
                    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, NULL, &nv, hh);
                    hh.ptr += s_d3d.srv_inc;
                }
            }
        }

        /* Shader-visible sampler tables for the VP/FP path.  Descriptors are
         * populated lazily and never overwritten, so recorded command lists
         * and an in-flight previous frame can safely share the heap. */
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            hd.NumDescriptors = VP_SAMPLER_SETS * 4;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
                s_d3d.device, &hd, &IID_ID3D12DescriptorHeap,
                (void**)&s_d3d.sampler_heap);
            if (FAILED(hr)) {
                printf("[D3D12] sampler heap creation failed (0x%08lX)\n", hr);
            } else {
                s_d3d.sampler_inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
                    s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
            }
        }

        /* RTV heap for offscreen render targets (CPU-visible only). */
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
            hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = MAX_OFF_RTS;
            hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
                s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.rt_rtv_heap);
            if (FAILED(hr)) printf("[D3D12] offscreen RTV heap creation failed (0x%08lX)\n", hr);
        }
    }

    /* ---------------------------------------------------------------
     * Create dynamic vertex buffer (upload heap, 4MB)
     * ---------------------------------------------------------------*/
    {
        D3D12_HEAP_PROPERTIES heap_props = {0};
        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC buf_desc = {0};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Width = MAX_VERTICES * VERTEX_STRIDE;
        buf_desc.Height = 1;
        buf_desc.DepthOrArraySize = 1;
        buf_desc.MipLevels = 1;
        buf_desc.SampleDesc.Count = 1;
        buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &heap_props, D3D12_HEAP_FLAG_NONE,
            &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.vertex_buffer);
        if (SUCCEEDED(hr)) {
            D3D12_RANGE read_range = {0, 0};
            s_d3d.vertex_buffer->lpVtbl->Map(
                s_d3d.vertex_buffer, 0, &read_range, &s_d3d.vb_mapped);
            s_d3d.vb_view.BufferLocation =
                s_d3d.vertex_buffer->lpVtbl->GetGPUVirtualAddress(s_d3d.vertex_buffer);
            s_d3d.vb_view.StrideInBytes = VERTEX_STRIDE;
            s_d3d.vb_view.SizeInBytes = MAX_VERTICES * VERTEX_STRIDE;
            printf("[D3D12] Vertex buffer created (%u KB)\n",
                   (MAX_VERTICES * VERTEX_STRIDE) / 1024);
        }
    }

    /* ---------------------------------------------------------------
     * Real vertex-program path resources: root signature (CBV b0 for the
     * vp_c[] bank + SRV t0-t3 + dynamic samplers s0-s3), a raw-float4 vertex buffer,
     * and the constant-bank buffer. The PSO itself is built lazily once the
     * game uploads its VP microcode (render_frame).
     * ---------------------------------------------------------------*/
    {
        /* 4-descriptor SRV table (t0-t3) so decompiled fragment programs can
         * sample up to 4 texture units; the hardcoded atlas/colour PSs use only
         * t0 and are unaffected.  A matching dynamic sampler table carries
         * each draw's RSX S/T/R address modes to s0-s3. */
        D3D12_DESCRIPTOR_RANGE srv_range = {0};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = 4;
        srv_range.BaseShaderRegister = 0;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE sampler_range = {0};
        sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        sampler_range.NumDescriptors = 4;
        sampler_range.BaseShaderRegister = 0;
        sampler_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rp[4] = {0};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b0 = vp_c bank */
        rp[0].Descriptor.ShaderRegister = 0;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges = &srv_range;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b1 = FP texscale */
        rp[2].Descriptor.ShaderRegister = 1;
        rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[3].DescriptorTable.NumDescriptorRanges = 1;
        rp[3].DescriptorTable.pDescriptorRanges = &sampler_range;
        rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rd = {0};
        rd.NumParameters = 4; rd.pParameters = rp;
        rd.NumStaticSamplers = 0; rd.pStaticSamplers = NULL;
        rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* sig = NULL; ID3DBlob* err = NULL;
        hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (SUCCEEDED(hr)) {
            s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0,
                sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                &IID_ID3D12RootSignature, (void**)&s_d3d.vp_root_sig);
            sig->lpVtbl->Release(sig);
        } else if (err) { printf("[D3D12] VP root sig: %s\n", (const char*)err->lpVtbl->GetBufferPointer(err)); err->lpVtbl->Release(err); }

        /* raw float4 vertex buffer + constant bank (both UPLOAD, persistently mapped) */
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_RANGE nr = {0,0};

        bd.Width = (u64)MAX_VERTICES * 256 * UPLOAD_FRAME_COUNT;  /* generic VP vertex: 16 float4 slots,
                                          * ring-buffered by frame parity so a
                                          * new frame's upload never overwrites
                                          * vertices the in-flight GPU frame is
                                          * still reading (was: torn/missing
                                          * triangles mixing stale+new verts) */
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_vb)))
            s_d3d.vp_vb->lpVtbl->Map(s_d3d.vp_vb, 0, &nr, &s_d3d.vp_vb_mapped);

        bd.Width = (u64)VP_CB_STRIDE * MAX_DRAWS * UPLOAD_FRAME_COUNT;
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_cb)))
            s_d3d.vp_cb->lpVtbl->Map(s_d3d.vp_cb, 0, &nr, &s_d3d.vp_cb_mapped);

        bd.Width = (u64)256 * MAX_DRAWS * UPLOAD_FRAME_COUNT;
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_fpcb)))
            s_d3d.vp_fpcb->lpVtbl->Map(s_d3d.vp_fpcb, 0, &nr, &s_d3d.vp_fpcb_mapped);

        /* Do not vertex-fetch or constant-fetch directly from the persistently
         * mapped upload heaps.  On the native D3D12 path that is legal, but
         * under vkd3d-proton we observed rare whole-model tears where CPU
         * fingerprints of those heaps were byte-identical to good frames.
         * Explicit copies into DEFAULT resources make the submitted frame an
         * immutable GPU-side snapshot and establish an ordinary COPY_DEST ->
         * VERTEX/CB barrier before any shader can consume it. */
        D3D12_HEAP_PROPERTIES hgpu = {0}; hgpu.Type = D3D12_HEAP_TYPE_DEFAULT;
        bd.Width = (u64)MAX_VERTICES * 256 * UPLOAD_FRAME_COUNT;
        s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hgpu,
            D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.vp_vb_gpu);
        bd.Width = (u64)VP_CB_STRIDE * MAX_DRAWS * UPLOAD_FRAME_COUNT;
        s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hgpu,
            D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.vp_cb_gpu);
        bd.Width = (u64)256 * MAX_DRAWS * UPLOAD_FRAME_COUNT;
        s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hgpu,
            D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.vp_fpcb_gpu);

        printf("[D3D12] VP path resources: rootsig=%p vb=%p cb=%p gpu=%p/%p/%p\n",
               (void*)s_d3d.vp_root_sig, (void*)s_d3d.vp_vb, (void*)s_d3d.vp_cb,
               (void*)s_d3d.vp_vb_gpu, (void*)s_d3d.vp_cb_gpu,
               (void*)s_d3d.vp_fpcb_gpu);
    }

    printf("[D3D12] Initialization complete (%ux%u, %u buffers, pipeline=%s)\n",
           width, height, FRAME_COUNT,
           s_d3d.pipeline_ready ? "ready" : "NOT ready");
    return 0;
}

/* ---------------------------------------------------------------------------
 * Frame sync helpers
 * -----------------------------------------------------------------------*/

static void wait_for_fence(u64 value)
{
    if (value && s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) < value) {
        s_d3d.fence->lpVtbl->SetEventOnCompletion(
            s_d3d.fence, value, s_d3d.fence_event);
        WaitForSingleObject(s_d3d.fence_event, INFINITE);
    }
}

static u64 signal_fence(void)
{
    const u64 value = ++s_d3d.fence_next;
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_d3d.fence, value);
    return value;
}

static void wait_for_gpu(void)
{
    /* A D3D12 fence is one global monotonically increasing timeline.  The old
     * code incremented one value per swap-chain buffer and then copied values
     * between slots.  That happened to be monotonic in the common two-buffer
     * cadence, but debug waits and non-alternating backbuffer acquisition could
     * signal duplicate or stale values, making allocator/resource reuse depend
     * on swap-chain history. */
    wait_for_fence(signal_fence());
}

static void protect_upload_heaps_for_batch(void)
{
    if (s_upload_heaps_safe_for_batch) return;
    /* Vertex and constant upload heaps have disjoint in-flight slices.  Wait
     * only for the previous submission which used the slice about to be
     * overwritten; a fresh wait_for_gpu() here serialized every frame behind
     * all GPU work and caused 40-90 ms first-draw stalls under vkd3d. */
    LARGE_INTEGER t0 = {0}, t1 = {0}, freq = {0};
    const int profiling = rsx_profile_enabled();
    if (profiling) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
    }
    wait_for_fence(s_upload_parity_fence[s_d3d.vp_parity]);
    if (profiling) {
        QueryPerformanceCounter(&t1);
        const double ms = (t1.QuadPart - t0.QuadPart) *
                          1000.0 / (double)freq.QuadPart;
        s_prof.upload_wait_calls++;
        s_prof.upload_wait_ms += ms;
        if (ms > s_prof.upload_wait_max_ms) s_prof.upload_wait_max_ms = ms;
    }
    s_upload_heaps_safe_for_batch = 1;
}

static u64 move_to_next_frame(void)
{
    const u32 submitted_frame = s_d3d.frame_index;
    s_d3d.fence_values[submitted_frame] = signal_fence();

    s_d3d.frame_index = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);
    wait_for_fence(s_d3d.fence_values[s_d3d.frame_index]);
    return s_d3d.fence_values[submitted_frame];
}

/* ---------------------------------------------------------------------------
 * Render a frame (clear + present)
 * -----------------------------------------------------------------------*/

/* Write the mapped readback buffer (R8G8B8A8, row pitch = readback_pitch) out
 * as a 24-bit bottom-up BMP. Debug-only. */
static void dump_backbuffer_bmp(void)
{
    if (!s_d3d.readback_buf) return;
    void* mapped = NULL;
    D3D12_RANGE rr = {0, (SIZE_T)s_d3d.readback_pitch * s_d3d.height};
    if (FAILED(s_d3d.readback_buf->lpVtbl->Map(s_d3d.readback_buf, 0, &rr, &mapped)) || !mapped)
        return;

    static int idx = 0;
    char path[512];
    const char* dir = getenv("CELLMARK_DUMP_DIR");   /* default: current dir */
    snprintf(path, sizeof(path), "%s%sframe_%03d.bmp",
             dir ? dir : "", dir ? "/" : "", idx++);
    FILE* f = fopen(path, "wb");
    if (f) {
        u32 w = s_d3d.width, h = s_d3d.height;
        u32 padded = (w * 3 + 3) & ~3u;
        u32 imgsz  = padded * h;
        u32 filesz = 54 + imgsz;
        unsigned char hdr[54] = {0};
        hdr[0] = 'B'; hdr[1] = 'M';
        hdr[2]=filesz&0xFF; hdr[3]=(filesz>>8)&0xFF; hdr[4]=(filesz>>16)&0xFF; hdr[5]=(filesz>>24)&0xFF;
        hdr[10]=54; hdr[14]=40;
        hdr[18]=w&0xFF; hdr[19]=(w>>8)&0xFF; hdr[20]=(w>>16)&0xFF; hdr[21]=(w>>24)&0xFF;
        hdr[22]=h&0xFF; hdr[23]=(h>>8)&0xFF; hdr[24]=(h>>16)&0xFF; hdr[25]=(h>>24)&0xFF;
        hdr[26]=1; hdr[28]=24;
        hdr[34]=imgsz&0xFF; hdr[35]=(imgsz>>8)&0xFF; hdr[36]=(imgsz>>16)&0xFF; hdr[37]=(imgsz>>24)&0xFF;
        fwrite(hdr, 1, 54, f);
        unsigned char* row = (unsigned char*)malloc(padded);
        if (row) {
            memset(row, 0, padded);
            for (int y = (int)h - 1; y >= 0; y--) {   /* BMP is bottom-up */
                unsigned char* src = (unsigned char*)mapped + (u32)y * s_d3d.readback_pitch;
                for (u32 x = 0; x < w; x++) {
                    row[x*3+0] = src[x*4+2]; /* B */
                    row[x*3+1] = src[x*4+1]; /* G */
                    row[x*3+2] = src[x*4+0]; /* R */
                }
                fwrite(row, 1, padded, f);
            }
            free(row);
        }
        fclose(f);
        printf("[D3D12] dumped %s (%ux%u)\n", path, s_d3d.width, s_d3d.height);
    }
    D3D12_RANGE wr = {0, 0};
    s_d3d.readback_buf->lpVtbl->Unmap(s_d3d.readback_buf, 0, &wr);
}

/* Decompile the captured RSX vertex program to HLSL and build the VP PSO
 * (decompiled VS + atlas alpha-test PS). One-shot per program. */
static void compile_vp(void)
{
    extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!st || st->vp_ucode_bytes < 16 || !s_d3d.vp_root_sig) return;

    static char hlsl[64 * 1024];
    int ni = rsx_vp_decompile(st->vp_ucode, st->vp_ucode_bytes, hlsl, sizeof hlsl);
    /* Does this VP read vp_c[0..3]? Gates the garbage-projection fallback:
     * programs keeping their MVP elsewhere (gcm/cube: c[256..259]) must not
     * have c[0..3] stomped nor the viewport z-lane overridden. */
    s_d3d.vp_uses_c03 = (strstr(hlsl, "vp_c[0]") || strstr(hlsl, "vp_c[1]") ||
                         strstr(hlsl, "vp_c[2]") || strstr(hlsl, "vp_c[3]")) ? 1 : 0;
    if (ni <= 0) { printf("[VP] decompile failed (%d)\n", ni); s_d3d.vp_compiled_bytes = st->vp_ucode_bytes; return; }
    if (getenv("VP_DUMP")) {
        FILE* vf = fopen("vp_dump.hlsl", "w");
        if (vf) { fwrite(hlsl, 1, strlen(hlsl), vf); fclose(vf);
                  printf("[VP] dumped vp_dump.hlsl (%d instrs)\n", ni); }
        printf("[VPRAW] first 3 instrs (%u ucode bytes):\n", st->vp_ucode_bytes);
        for (u32 _q = 0; _q < 48 && _q < st->vp_ucode_bytes; _q += 16)
            printf("[VPRAW]  d0=%02X%02X%02X%02X d1=%02X%02X%02X%02X d2=%02X%02X%02X%02X d3=%02X%02X%02X%02X\n",
                st->vp_ucode[_q+0],st->vp_ucode[_q+1],st->vp_ucode[_q+2],st->vp_ucode[_q+3],
                st->vp_ucode[_q+4],st->vp_ucode[_q+5],st->vp_ucode[_q+6],st->vp_ucode[_q+7],
                st->vp_ucode[_q+8],st->vp_ucode[_q+9],st->vp_ucode[_q+10],st->vp_ucode[_q+11],
                st->vp_ucode[_q+12],st->vp_ucode[_q+13],st->vp_ucode[_q+14],st->vp_ucode[_q+15]);
    }

    /* Pixel shader mirrors dbgfont's FP: sample the atlas coverage at TEXCOORD0
     * (VP output o[7]), alpha-test at 0.5, output the vertex color (o[1]). */
    static const char ps[] =
        "Texture2D tex : register(t0); SamplerState smp : register(s0);\n"
        "struct PSIn{ float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;\n"
        "  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
        "  float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7; };\n"
        "float4 main(PSIn i):SV_TARGET{ float cov = tex.Sample(smp, i.t0.xy).r;\n"
        "  if (cov <= 0.5) discard; return float4(i.col0.rgb, 1); }\n";

    ID3DBlob *vb=NULL,*pb=NULL,*e=NULL;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "vp", NULL, NULL, "main", "vs_5_0", 0, 0, &vb, &e);
    if (FAILED(hr)) { printf("[VP] VS compile FAIL: %s\n", e?(const char*)e->lpVtbl->GetBufferPointer(e):"?"); if(e)e->lpVtbl->Release(e); s_d3d.vp_compiled_bytes=st->vp_ucode_bytes; return; }
    hr = D3DCompile(ps, sizeof(ps)-1, "vpps", NULL, NULL, "main", "ps_5_0", 0, 0, &pb, &e);
    if (FAILED(hr)) { printf("[VP] PS compile FAIL: %s\n", e?(const char*)e->lpVtbl->GetBufferPointer(e):"?"); if(e)e->lpVtbl->Release(e); if(vb)vb->lpVtbl->Release(vb); s_d3d.vp_compiled_bytes=st->vp_ucode_bytes; return; }

    /* The decompiled VS declares inputs a0:ATTR0 .. a15:ATTR15 and the HLSL
     * compiler keeps whichever the program body reads. D3D12 requires EVERY VS
     * input to have a matching input-layout element (by semantic name+index),
     * so declare all 16 ATTR slots. (The old single "POSITION" element matched
     * NO VS input once the decompiler switched to ATTR semantics -> PSO failed
     * with E_INVALIDARG for every VP, blanking cellmark's text and vkcube.)
     * The VP path uploads only attrib0 to vp_vb (one float4/vertex), so every
     * slot reads that same float4 at offset 0; attributes other than position
     * therefore alias attrib0 for now (colours/uv wrong until the VP path
     * uploads multiple attributes), but geometry is correct and the PSO is
     * valid. */
    D3D12_INPUT_ELEMENT_DESC il[16];
    for (int _e = 0; _e < 16; _e++) {
        il[_e].SemanticName = "ATTR";
        il[_e].SemanticIndex = _e;
        il[_e].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        il[_e].InputSlot = 0;
        /* 256-byte generic VP vertex: every ATTRi is a float4 slot at i*16
         * (read_vp_vertex converts each enabled RSX attrib by type; disabled
         * slots hold (0,0,0,1)). Covers tiny3d (a0/a3/a8), SDK gcm samples
         * (a0/a1/a2), dbgfont -- no aliasing. */
        il[_e].AlignedByteOffset = (UINT)(_e * 16);
        il[_e].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        il[_e].InstanceDataStepRate = 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_d3d.vp_root_sig;
    pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb); pd.VS.BytecodeLength = vb->lpVtbl->GetBufferSize(vb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb); pd.PS.BytecodeLength = pb->lpVtbl->GetBufferSize(pb);
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = 16;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    /* Depth test ON (LESS_EQUAL, matching the guest's rsxtiny_DepthTestFunc 515 /
     * CELL_GCM_LEQUAL). Without it the cube's faces drew in submission order and
     * back faces overwrote the front -> you saw through the near face to the
     * darker interior. The depth buffer is bound + cleared to 1.0 each frame. */
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.SampleDesc.Count = 1;
    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_vp);

    /* Second PSO: same VS but a COLOUR-only PS (no texture sample, no alpha
     * discard) for untextured 3D geometry (vkcube's cube). The atlas PS above
     * samples a texture and discards where coverage<=0.5, which blanks any draw
     * with no atlas bound. */
    {
        static const char ps_col[] =
            "struct PSIn{ float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;\n"
            "  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
            "  float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7; };\n"
            "float4 main(PSIn i):SV_TARGET{ return float4(i.col0.rgb, 1); }\n";
        ID3DBlob *pcb=NULL,*ec=NULL;
        if (SUCCEEDED(D3DCompile(ps_col, sizeof(ps_col)-1, "vppsc", NULL, NULL,
                                 "main", "ps_5_0", 0, 0, &pcb, &ec)) && pcb) {
            pd.PS.pShaderBytecode = pcb->lpVtbl->GetBufferPointer(pcb);
            pd.PS.BytecodeLength  = pcb->lpVtbl->GetBufferSize(pcb);
            HRESULT hr2 = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pd, &IID_ID3D12PipelineState,
                (void**)&s_d3d.pipeline_state_vp_color);
            printf(SUCCEEDED(hr2) ? "[VP] colour pipeline ready\n"
                                  : "[VP] colour PSO FAIL (0x%08lX)\n", hr2);
            pcb->lpVtbl->Release(pcb);
        }
        if (ec) ec->lpVtbl->Release(ec);
    }
    /* Keep the VS bytecode for guest-FP PSO builds (vp_get_fp_pso); bump the
     * generation so cached FP PSOs built against the old VS are rebuilt. */
    if (s_d3d.vp_vs_blob) s_d3d.vp_vs_blob->lpVtbl->Release(s_d3d.vp_vs_blob);
    s_d3d.vp_vs_blob = vb;
    s_d3d.vp_gen++;
    pb->lpVtbl->Release(pb);
    s_d3d.vp_compiled_bytes = st->vp_ucode_bytes;
    if (SUCCEEDED(hr)) { s_d3d.vp_ready = 1; printf("[VP] pipeline ready (%d instrs)\n", ni); }
    else {
        printf("[VP] PSO creation FAIL (0x%08lX)\n", hr);
        /* Drain the debug layer's message queue to get the EXACT validation
         * reason (E_INVALIDARG is otherwise opaque). One-shot. */
        ID3D12InfoQueue* iq = NULL;
        if (SUCCEEDED(s_d3d.device->lpVtbl->QueryInterface(
                s_d3d.device, &IID_ID3D12InfoQueue, (void**)&iq)) && iq) {
            UINT64 n = iq->lpVtbl->GetNumStoredMessages(iq);
            for (UINT64 mi = 0; mi < n; mi++) {
                SIZE_T len = 0;
                iq->lpVtbl->GetMessage(iq, mi, NULL, &len);
                D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
                if (m && SUCCEEDED(iq->lpVtbl->GetMessage(iq, mi, m, &len)))
                    printf("[VP][DBG] %s\n", m->pDescription);
                free(m);
            }
            iq->lpVtbl->Release(iq);
        }
    }
}

/* Build (or fetch) a PSO pairing the current decompiled VS with the guest's
 * FRAGMENT program at fp_addr: read the FP ucode from guest memory, decompile
 * to HLSL (rsx_fp_decompiler), compile, and cache. This replaces the two
 * hardcoded pixel shaders for draws whose FP we can translate -- e.g.
 * gcm/cube's plasma FP `c = tex2D(t0, uv).x; out = (c, 0, c, 1)`. */
#include "rsx_fp_decompiler.h"

/* Hash + compile the CURRENT rsx_state vertex program into the VS cache;
 * returns the cache slot (or -1). Called at draw-record time so each draw
 * carries the VP that was loaded when it was submitted. */
static u32 vp_hash_ucode(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h ? h : 1u;
}

static u32 vp_hash_extend(u32 h, const u8* p, u32 n)
{
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* Hash the exact source rows which vp_upload_tex_slot will consume.  This is
 * intentionally row-aware: linear RSX textures may have padding in CONTROL3,
 * while swizzled textures are tightly packed.  Used only by the bounded F9
 * capture diagnostic below. */
static u32 vp_texture_source_hash(const D3D12DrawRecord* dr, int unit)
{
    extern uint8_t* vm_base;
    if (!dr || unit < 0 || unit >= 4 || !vm_base) return 0;
    const u32 off = dr->tex[unit].off;
    const u32 w = dr->tex[unit].w, h = dr->tex[unit].h;
    const u32 fmt = dr->tex[unit].fmt;
    const u32 base_fmt = fmt & 0x9Fu;
    if (!off || !w || !h || w > 4096 || h > 4096) return 0;

    const int bc1 = base_fmt == 0x86;
    const int bc = bc1 || base_fmt == 0x87 || base_fmt == 0x88;
    const u32 bpp = (base_fmt == 0x85 || base_fmt == 0x9E) ? 4u : 1u;
    const u32 row_bytes = bc ? ((w + 3) / 4) * (bc1 ? 8u : 16u) : w * bpp;
    const u32 rows = bc ? (h + 3) / 4 : h;
    const u32 row_pitch = ((fmt & 0x20u) && dr->tex[unit].pitch >= row_bytes)
        ? dr->tex[unit].pitch : row_bytes;
    u32 hash = 2166136261u;
    for (u32 y = 0; y < rows; y++)
        hash = vp_hash_extend(hash, vm_base + off + (u64)y * row_pitch, row_bytes);
    return hash ? hash : 1u;
}

static u32 vp_vertex_attr_hash(const u8* draw_vb, u32 vertex_count, u32 attr)
{
    if (!draw_vb || attr >= 16) return 0;
    u32 hash = 2166136261u;
    for (u32 v = 0; v < vertex_count; v++)
        hash = vp_hash_extend(hash, draw_vb + (u64)v * 256 + attr * 16, 16);
    return hash ? hash : 1u;
}

/* Capture a fragment program while the FIFO is processing this draw.  The
 * title reuses one local-memory address and patches constants between draws;
 * replaying from guest memory at the end of the batch is therefore too late. */
static void fp_snapshot_draw(D3D12DrawRecord* dr, u32 slot)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);

    if (!dr) return;
    dr->fp_hash = 0;
    dr->fp_size = 0;
    dr->fp_snapshot = (u16)(slot < MAX_DRAWS ? slot : 0);
    if (!dr->fp_addr || !vm_base || slot >= MAX_DRAWS) return;

    FPSnapshot* snap = &s_fp_snapshots[slot];
    snap->size = 0;
    snap->hash = 0;
    u32 off = cellGcmResolveLocated((dr->fp_addr & 0x3u) == 1,
                                    dr->fp_addr & ~0x3u);
    if (off == 0xFFFFFFFFu) return;

    const u8* src = vm_base + off;
    u32 size = rsx_fp_program_size(src, FP_SNAPSHOT_MAX);
    if (size == 0) size = 64;
    if (size > FP_SNAPSHOT_MAX) return;

    memcpy(snap->ucode, src, size);
    snap->hash = vp_hash_ucode(snap->ucode, size);
    snap->size = size;
    dr->fp_hash = snap->hash;
    dr->fp_size = (u16)size;
}

static const u8* fp_snapshot_ucode(const D3D12DrawRecord* dr)
{
    if (!dr || !dr->fp_size || dr->fp_snapshot >= MAX_DRAWS) return NULL;
    const FPSnapshot* snap = &s_fp_snapshots[dr->fp_snapshot];
    if (snap->size != dr->fp_size || snap->hash != dr->fp_hash) return NULL;
    return snap->ucode;
}

/* Hash the fragment ucode a draw points at. Inline constants live INSIDE the
 * ucode, so this changes when the guest patches them. Taken at record time and
 * again at replay: a mismatch means the batch replayed a draw with another
 * draw's constants. */
static u32 fp_ucode_hash(u32 fp_addr)
{
    extern uint8_t* vm_base;
    /* Trace-only. Scanning up to 4 KB of guest memory per draw on the FIFO
     * drain thread perturbs demand-commit and made the flaky fumen-XML boot
     * race fire every time, so stay out of the way unless tracing is armed. */
    static int en = -1;
    if (en < 0) en = getenv("RTT_DUMP") ? 1 : 0;
    if (!en && s_rtt_hotkey_frames <= 0) return 0;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    if (!fp_addr || !vm_base) return 0;
    u32 off = cellGcmResolveLocated((fp_addr & 0x3u) == 1, fp_addr & ~0x3u);
    if (off == 0xFFFFFFFFu) return 0;
    u32 usz = rsx_fp_program_size(vm_base + off, 4096);
    if (usz == 0) usz = 64;
    return vp_hash_ucode(vm_base + off, usz);
}

static int vp_get_vs(const rsx_state* st)
{
    extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
    extern u32 rsx_vp_program_size_instrs(const u8*, u32);
    if (!st || st->vp_ucode_bytes < 16) return -1;
    /* vp_ucode_bytes is a high-water mark for the upload bank.  A shorter
     * program loaded later leaves old instructions after its END marker;
     * hashing those unreachable bytes produced many cache identities for the
     * same shader and eventually shifted recorded cache slots on eviction. */
    u32 instrs = rsx_vp_program_size_instrs(st->vp_ucode, st->vp_ucode_bytes);
    u32 program_bytes = instrs ? instrs * 16u : st->vp_ucode_bytes;
    u32 hash = vp_hash_ucode(st->vp_ucode, program_bytes);
    for (int i = 0; i < s_d3d.vp_vs_n; i++)
        if (s_d3d.vp_vs[i].hash == hash) return i;

    s_prof.vs_compiles++;
    static char hlsl[262144];
    int ni = rsx_vp_decompile(st->vp_ucode, program_bytes, hlsl, sizeof hlsl);
    if (ni <= 0) return -1;
    if (getenv("VP_DUMP")) { static int _d=0; if (_d++ < 4) {
        FILE* f = fopen("vp2_dump.hlsl", _d==1 ? "w" : "a");
        if (f) { fprintf(f, "/* per-draw VS hash pending, %d instrs */%s%s", ni, hlsl, "\n"); fclose(f); } } }
    /* VP_DUMP_ALL is intentionally one file per bytecode hash.  Cache slots
     * are eviction-dependent and therefore useless when correlating an F9
     * trace with a later run; the hash remains stable for identical ucode. */
    if (getenv("VP_DUMP_ALL")) {
        char path[64];
        snprintf(path, sizeof(path), "vp_%08X.hlsl", hash);
        FILE* f = fopen(path, "w");
        if (f) {
            fprintf(f, "/* hash=0x%08X, %d instructions */\n%s\n", hash, ni, hlsl);
            fclose(f);
        }
    }
    ID3DBlob* vb = NULL; ID3DBlob* e = NULL;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "guest_vp2", NULL, NULL,
                            "main", "vs_5_0", 0, 0, &vb, &e);
    if (e) e->lpVtbl->Release(e);
    if (FAILED(hr) || !vb) {
        static int _e=0; if (_e++<4) printf("[VP] per-draw VS compile FAIL (hash=0x%08X)\n", hash);
        return -1;
    }
    int slot;
    if (s_d3d.vp_vs_n < VP_VS_CACHE) slot = s_d3d.vp_vs_n++;
    else {  /* evict slot 0 */
        if (s_d3d.vp_vs[0].vs) s_d3d.vp_vs[0].vs->lpVtbl->Release(s_d3d.vp_vs[0].vs);
        memmove(&s_d3d.vp_vs[0], &s_d3d.vp_vs[1], sizeof(VPVSEntry)*(VP_VS_CACHE-1));
        slot = VP_VS_CACHE - 1;
    }
    s_d3d.vp_vs[slot].hash = hash;
    s_d3d.vp_vs[slot].vs   = vb;
    s_d3d.vp_vs[slot].uses_c03 =
        (strstr(hlsl, "vp_c[0]") || strstr(hlsl, "vp_c[1]") ||
         strstr(hlsl, "vp_c[2]") || strstr(hlsl, "vp_c[3]")) ? 1 : 0;
    { static int _n=0; if (_n++<6) printf("[VP] per-draw VS cached (hash=0x%08X, %d instrs, slot %d)\n", hash, ni, slot); }
    return slot;
}

static D3D12_BLEND rsx_blend_factor(u32 factor, int alpha)
{
    switch (factor & 0xFFFFu) {
    case 0x0000: return D3D12_BLEND_ZERO;
    case 0x0001: return D3D12_BLEND_ONE;
    case 0x0300: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D12_BLEND_SRC_ALPHA;
    case 0x0303: return D3D12_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D12_BLEND_DEST_ALPHA;
    case 0x0305: return D3D12_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 0x0308: return alpha ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA_SAT;
    case 0x8001:
    case 0x8003: return D3D12_BLEND_BLEND_FACTOR;
    case 0x8002:
    case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
    default: return D3D12_BLEND_ONE;
    }
}

static D3D12_BLEND_OP rsx_blend_op(u32 equation)
{
    switch (equation & 0xFFFFu) {
    case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
    case 0x800B:
    case 0xF005: return D3D12_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D12_BLEND_OP_MIN;
    case 0x8008: return D3D12_BLEND_OP_MAX;
    default: return D3D12_BLEND_OP_ADD;
    }
}

static ID3D12PipelineState* vp_get_fp_pso(int vs_idx, u32 fp_addr,
                                          const u8* fp_ucode, u32 fp_size,
                                          u32 fp_hash, int blend,
                                          u32 blend_sf, u32 blend_df, u32 blend_eq,
                                          int depth_test, int depth_mask, u32 depth_func,
                                          int cull_enable, u32 cull_face, u32 front_face,
                                          int nrt, DXGI_FORMAT rtfmt, int exp32, u32 cmask)
{
    const int dbg_body_fp = ((fp_addr & ~3u) == 0x01AA8E40u);
    if (nrt < 1) nrt = 1; if (nrt > 4) nrt = 4;
    if (rtfmt == 0) rtfmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);
    if (!fp_addr || ((!fp_ucode || !fp_size) && !vm_base)) return NULL;
    { static int _off=-1; if(_off<0)_off=getenv("FP_OFF")?1:0; if(_off) return NULL; }
    /* Resolve the VS: the draw's cached per-VP blob, else the primary. */
    ID3DBlob* vsb = NULL; u32 vs_hash = 0;
    if (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n) {
        vsb = s_d3d.vp_vs[vs_idx].vs; vs_hash = s_d3d.vp_vs[vs_idx].hash;
    }
    if (!vsb) { vsb = s_d3d.vp_vs_blob; vs_idx = -1; }
    if (!vsb) return NULL;

    /* Body-only execution probe.  It keeps the real FP, PSO state, target,
     * draw ordering, and vertex count, but replaces post-VS geometry with a
     * repeated fullscreen triangle.  A visible result proves the draw/PSO/RT
     * path executes and narrows the fault to the guest VS or its inputs. */
    ID3DBlob* dbg_vsb = NULL;
    if (dbg_body_fp && s_dbg_body_fixed_vs) {
        static const char dbg_vs_hlsl[] =
            "struct O { float4 position:SV_POSITION; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;"
            " float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;"
            " float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7; };"
            "O main(uint id:SV_VertexID) { O o; uint k=id%3;"
            " float2 p=(k==0)?float2(-1,-1):((k==1)?float2(-1,3):float2(3,-1));"
            " o.position=float4(p,0,1); o.col0=0; o.col1=0; o.fog=0;"
            " o.tc0=0; o.tc1=0; o.tc2=0; o.tc3=0; o.tc4=0; o.tc5=0; o.tc6=0; o.tc7=0; return o; }";
        ID3DBlob* err = NULL;
        HRESULT dhr = D3DCompile(dbg_vs_hlsl, sizeof(dbg_vs_hlsl) - 1,
                                 "body_probe_vs", NULL, NULL, "main", "vs_5_0",
                                 0, 0, &dbg_vsb, &err);
        if (err) err->lpVtbl->Release(err);
        if (FAILED(dhr) || !dbg_vsb) {
            fprintf(stderr, "[BODY-PROBE] fullscreen VS compile failed: 0x%08lX\n", dhr);
            return NULL;
        }
        vsb = dbg_vsb;
    }

    /* SET_SHADER_PROGRAM low bits = location+1 (same as textures): 1 = LOCAL
     * (VRAM), 2 = MAIN (gcm/cube emits 0x00B90001 for its VRAM-resident FP). */
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    u32 off = 0xFFFFFFFFu;
    const u8* up = fp_ucode;
    u32 usz = fp_size;
    u32 uhash = fp_hash;
    if (!up || !usz) {
        off = cellGcmResolveLocated((fp_addr & 0x3u) == 1, fp_addr & ~0x3u);
        if (off == 0xFFFFFFFFu) return NULL;
        up = vm_base + off;
        usz = rsx_fp_program_size(up, FP_SNAPSHOT_MAX);
        if (usz == 0) usz = 64;
    }
    if (!uhash) uhash = vp_hash_ucode(up, usz);

    for (int i = 0; i < s_d3d.vp_fp_n; i++)
        if (s_d3d.vp_fp[i].fp_addr == fp_addr && s_d3d.vp_fp[i].vs_idx == vs_idx &&
            s_d3d.vp_fp[i].vs_hash == vs_hash && s_d3d.vp_fp[i].gen == s_d3d.vp_gen &&
            s_d3d.vp_fp[i].blend == blend &&
            s_d3d.vp_fp[i].blend_sf == blend_sf &&
            s_d3d.vp_fp[i].blend_df == blend_df &&
            s_d3d.vp_fp[i].blend_eq == blend_eq &&
            s_d3d.vp_fp[i].depth_test == depth_test &&
            s_d3d.vp_fp[i].depth_mask == depth_mask &&
            s_d3d.vp_fp[i].depth_func == depth_func &&
            s_d3d.vp_fp[i].cull_enable == cull_enable &&
            s_d3d.vp_fp[i].cull_face == cull_face &&
            s_d3d.vp_fp[i].front_face == front_face &&
            s_d3d.vp_fp[i].nrt == nrt &&
            s_d3d.vp_fp[i].rtfmt == (u32)rtfmt && s_d3d.vp_fp[i].exp32 == exp32 &&
            s_d3d.vp_fp[i].ucode_hash == uhash && s_d3d.vp_fp[i].cmask == cmask)
            return s_d3d.vp_fp[i].pso;
    s_prof.fp_compiles++;
    static char hlsl[32768];
    int n = rsx_fp_decompile(up, usz, hlsl, sizeof(hlsl), exp32);
    if (n <= 0) { static int _e=0; if(_e++<16) printf("[FP] decompile fail (fp=0x%08X)\n", fp_addr); return NULL; }
    if (getenv("FP_DUMP")) { static int _d=0; if (_d++ < 4) {
        FILE* f = fopen("fp_dump.hlsl", _d==1 ? "w" : "a");
        if (f) {
            fprintf(f, "/* fp_addr=0x%08X vmoff=0x%08X raw:", fp_addr, off);
            for (u32 _b = 0; _b < 32 && _b < usz; _b++) fprintf(f, " %02X", up[_b]);
            fprintf(f, " */\n%s\n", hlsl); fclose(f);
        } } }

    /* Debug FP_ONE=<hex fp_addr>: force that program's colour output to
     * all-ones (e.g. wave's colour-detect -> full mask -> island borders
     * everywhere -> the water sim must visibly radiate if it works). */
    { const char* f1 = getenv("FP_ONE");
      if (f1 && (u32)strtoul(f1, NULL, 16) == fp_addr) {
          static const char old_assign[] = "_po.c0 = r[0];";
          static const char new_assign[] = "_po.c0 = (1).xxxx;";
          char* rp = strstr(hlsl, old_assign);
          if (rp) {
              size_t old_len = sizeof(old_assign) - 1;
              size_t new_len = sizeof(new_assign) - 1;
              size_t tail_len = strlen(rp + old_len) + 1;
              if (strlen(hlsl) + (new_len - old_len) < sizeof(hlsl)) {
                  memmove(rp + new_len, rp + old_len, tail_len);
                  memcpy(rp, new_assign, new_len);
                  static int logged = 0;
                  if (!logged++)
                      printf("[FP] forced all-ones output (fp=0x%08X)\n", fp_addr);
              }
          }
      } }
    /* FP_FORCE=1: replace the translated body with solid magenta -- isolates
     * geometry/transform problems from texture/blend problems. */
    if (getenv("FP_FORCE")) {
        static const char old_assign[] = "_po.c0 = r[0];";
        static const char new_assign[] = "_po.c0 = float4(1,0,1,1);";
        char* rp = strstr(hlsl, old_assign);
        if (rp) {
            size_t old_len = sizeof(old_assign) - 1;
            size_t new_len = sizeof(new_assign) - 1;
            size_t tail_len = strlen(rp + old_len) + 1;
            if (strlen(hlsl) + (new_len - old_len) < sizeof(hlsl)) {
                memmove(rp + new_len, rp + old_len, tail_len);
                memcpy(rp, new_assign, new_len);
                printf("[FP] forced solid-magenta output (fp=0x%08X)\n", fp_addr);
            }
        }
    }
    static const char dbg_tex_ps_hlsl[] =
        "struct I { float4 position:SV_POSITION; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;"
        " float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;"
        " float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7; };"
        "Texture2D t:register(t0); SamplerState s:register(s0);"
        "float4 main(I i):SV_Target0{return t.Sample(s,i.tc0.xy);}";
    static const char dbg_uv_ps_hlsl[] =
        "struct I { float4 position:SV_POSITION; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;"
        " float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;"
        " float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7; };"
        "float4 main(I i):SV_Target0{return float4(frac(i.tc0.xy),0,1);}";
    const char* ps_source = (dbg_body_fp && s_dbg_body_fixed_ps)
        ? (s_dbg_body_fixed_ps == 1 ? dbg_tex_ps_hlsl : dbg_uv_ps_hlsl)
        : hlsl;
    ID3DBlob* pb = NULL; ID3DBlob* e = NULL;
    HRESULT hr = D3DCompile(ps_source, strlen(ps_source), "guest_fp", NULL, NULL,
                            "main", "ps_5_0", 0, 0, &pb, &e);
    if (FAILED(hr) || !pb) {
        static int _e2=0; if (_e2++<16)
            printf("[FP] PS compile FAIL (fp=0x%08X): %s\n", fp_addr,
                   e ? (const char*)e->lpVtbl->GetBufferPointer(e) : "?");
        if (e) e->lpVtbl->Release(e);
        return NULL;
    }
    if (e) e->lpVtbl->Release(e);

    D3D12_INPUT_ELEMENT_DESC il[16];
    for (int _i = 0; _i < 16; _i++) {
        il[_i].SemanticName = "ATTR"; il[_i].SemanticIndex = _i;
        il[_i].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; il[_i].InputSlot = 0;
        il[_i].AlignedByteOffset = (UINT)(_i * 16);
        il[_i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        il[_i].InstanceDataStepRate = 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_d3d.vp_root_sig;
    pd.VS.pShaderBytecode = vsb->lpVtbl->GetBufferPointer(vsb);
    pd.VS.BytecodeLength  = vsb->lpVtbl->GetBufferSize(vsb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb);
    pd.PS.BytecodeLength  = pb->lpVtbl->GetBufferSize(pb);
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = 16;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    if (cull_enable && !(dbg_body_fp && s_dbg_body_nocull)) {
        /* RSX/GL enums: FRONT=0x404, BACK=0x405.  The generated VS has
         * already folded the RSX viewport transform into clip space. */
        pd.RasterizerState.CullMode = ((cull_face & 0xFFFu) == 0x404u)
            ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK;
        pd.RasterizerState.FrontCounterClockwise =
            rsx_to_d3d12_front_ccw(front_face) ? TRUE : FALSE;
        if (dbg_body_fp && s_dbg_body_flip_front)
            pd.RasterizerState.FrontCounterClockwise =
                !pd.RasterizerState.FrontCounterClockwise;
    }
    /* Blend per the guest's state at draw time: dbgfont text needs straight
     * alpha; demosaic's effect passes write data in all four channels with
     * blending OFF (alpha-blending them compounds to black). */
    pd.BlendState.RenderTarget[0].BlendEnable    = blend ? TRUE : FALSE;
    pd.BlendState.RenderTarget[0].SrcBlend       = rsx_blend_factor(blend_sf, 0);
    pd.BlendState.RenderTarget[0].DestBlend      = rsx_blend_factor(blend_df, 0);
    pd.BlendState.RenderTarget[0].BlendOp        = rsx_blend_op(blend_eq);
    pd.BlendState.RenderTarget[0].SrcBlendAlpha  = rsx_blend_factor(blend_sf >> 16, 1);
    pd.BlendState.RenderTarget[0].DestBlendAlpha = rsx_blend_factor(blend_df >> 16, 1);
    pd.BlendState.RenderTarget[0].BlendOpAlpha   = rsx_blend_op(blend_eq >> 16);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = (UINT)nrt;
    for (int _r = 0; _r < nrt; _r++) {
        pd.RTVFormats[_r] = rtfmt;
        /* Zero-init leaves RenderTargetWriteMask 0 on secondary targets --
         * every MRT-B write would be masked off. Mirror RT0's blend state. */
        pd.BlendState.RenderTarget[_r] = pd.BlendState.RenderTarget[0];
    }
    /* Guest colour write mask (RGBA nibble, already D3D-ordered). */
    for (int _r = 0; _r < nrt; _r++)
        pd.BlendState.RenderTarget[_r].RenderTargetWriteMask = (UINT8)(cmask ? cmask : 0xF);
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pd.DepthStencilState.DepthEnable = depth_test ? TRUE : FALSE;
    pd.DepthStencilState.DepthWriteMask = depth_mask ?
        D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    switch (depth_func & 0xFFu) {
    case 0x00: pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_NEVER; break;
    case 0x01: pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS; break;
    case 0x02: pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL; break;
    case 0x03:
        /* Keep the spec-correct LEQUAL default.  VP_LEQUAL_LESS is a title
         * compatibility option for engines that submit equal-Z backdrop
         * quads after their foreground (Taiko's Lumen UI does this). */
        pd.DepthStencilState.DepthFunc = getenv("VP_LEQUAL_LESS")
            ? D3D12_COMPARISON_FUNC_LESS
            : D3D12_COMPARISON_FUNC_LESS_EQUAL;
        break;
    case 0x04: pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER; break;
    case 0x05: pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_NOT_EQUAL; break;
    case 0x06: pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL; break;
    default:   pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS; break;
    }
    /* Diagnostic only: retain the game's FIFO and depth writes, but make the
     * comparison pass.  This separates bad RSX->D3D depth translation from
     * CPU/Lumen ordering without introducing another reorder workaround. */
    if (getenv("VP_FORCE_DEPTH_ALWAYS"))
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    if (dbg_body_fp && s_dbg_body_depth_always)
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.SampleDesc.Count = 1;

    ID3D12PipelineState* pso = NULL;
    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
        s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&pso);
    pb->lpVtbl->Release(pb);
    if (dbg_vsb) dbg_vsb->lpVtbl->Release(dbg_vsb);
    if (FAILED(hr)) {
        static int _e3=0; if (_e3++<16) printf("[FP] PSO FAIL (fp=0x%08X, 0x%08lX)\n", fp_addr, hr);
        return NULL;
    }
    { static int _ok=0; if (_ok++<32) printf("[FP] guest FP pipeline ready (fp=0x%08X)\n", fp_addr); }

    /* insert (evict oldest when full) */
    if (s_d3d.vp_fp_n >= VP_FP_CACHE) {
        if (s_d3d.vp_fp[0].pso) s_d3d.vp_fp[0].pso->lpVtbl->Release(s_d3d.vp_fp[0].pso);
        memmove(&s_d3d.vp_fp[0], &s_d3d.vp_fp[1], sizeof(VPFPEntry) * (VP_FP_CACHE - 1));
        s_d3d.vp_fp_n = VP_FP_CACHE - 1;
    }
    s_d3d.vp_fp[s_d3d.vp_fp_n].fp_addr = fp_addr;
    s_d3d.vp_fp[s_d3d.vp_fp_n].vs_idx  = vs_idx;
    s_d3d.vp_fp[s_d3d.vp_fp_n].vs_hash = vs_hash;
    s_d3d.vp_fp[s_d3d.vp_fp_n].gen     = s_d3d.vp_gen;
    s_d3d.vp_fp[s_d3d.vp_fp_n].blend   = blend;
    s_d3d.vp_fp[s_d3d.vp_fp_n].blend_sf = blend_sf;
    s_d3d.vp_fp[s_d3d.vp_fp_n].blend_df = blend_df;
    s_d3d.vp_fp[s_d3d.vp_fp_n].blend_eq = blend_eq;
    s_d3d.vp_fp[s_d3d.vp_fp_n].depth_test = depth_test;
    s_d3d.vp_fp[s_d3d.vp_fp_n].depth_mask = depth_mask;
    s_d3d.vp_fp[s_d3d.vp_fp_n].depth_func = depth_func;
    s_d3d.vp_fp[s_d3d.vp_fp_n].cull_enable = cull_enable;
    s_d3d.vp_fp[s_d3d.vp_fp_n].cull_face = cull_face;
    s_d3d.vp_fp[s_d3d.vp_fp_n].front_face = front_face;
    s_d3d.vp_fp[s_d3d.vp_fp_n].nrt     = nrt;
    s_d3d.vp_fp[s_d3d.vp_fp_n].rtfmt   = (u32)rtfmt;
    s_d3d.vp_fp[s_d3d.vp_fp_n].exp32   = exp32;
    s_d3d.vp_fp[s_d3d.vp_fp_n].ucode_hash = uhash;
    s_d3d.vp_fp[s_d3d.vp_fp_n].cmask   = cmask;
    s_d3d.vp_fp[s_d3d.vp_fp_n].pso     = pso;
    s_d3d.vp_fp_n++;
    return pso;
}

/* RSX textures without CELL_GCM_TEXTURE_LN use Morton/Z-order texels.  This
 * is the same bit walk used by the hardware (and RPCS3): interleave X and Y
 * while both dimensions have bits, then append the remaining axis for a
 * rectangular power-of-two texture. */
static u32 rsx_swizzled_index_2d(u32 x, u32 y, u32 w, u32 h)
{
    u32 log_w = 0, log_h = 0;
    for (u32 p = 1; p < w; p <<= 1) log_w++;
    for (u32 p = 1; p < h; p <<= 1) log_h++;

    u32 off = 0, shift = 0;
    while (log_w || log_h) {
        if (log_w) {
            off |= (x & 1u) << shift++;
            x >>= 1;
            log_w--;
        }
        if (log_h) {
            off |= (y & 1u) << shift++;
            y >>= 1;
            log_h--;
        }
    }
    return off;
}

/* Content identity for guest textures.  Guest addresses are allocator-owned
 * and can be recycled across screens, so offset/dimensions alone are not a
 * safe persistent cache key.  This raw-source scan also handles animated
 * textures: changed bytes naturally force an upload, while static linear and
 * swizzled UI assets avoid mapping/copying/barriers every frame. */
static u64 vp_source_hash(u32 off, u32 row_bytes, u32 rows, u32 row_pitch)
{
    extern uint8_t* vm_base;
    u64 hash = 0x9E3779B185EBCA87ULL ^ ((u64)row_bytes << 32) ^ rows;
    for (u32 y = 0; y < rows; ++y) {
        const u8* src = vm_base + off + (u64)y * row_pitch;
        u32 x = 0;
        for (; x + 8 <= row_bytes; x += 8) {
            u64 word;
            memcpy(&word, src + x, sizeof(word));
            hash ^= word + 0x9E3779B97F4A7C15ULL + (hash << 6) + (hash >> 2);
            hash *= 0xD6E8FEB86659FD93ULL;
        }
        if (x < row_bytes) {
            u64 tail = 0;
            memcpy(&tail, src + x, row_bytes - x);
            hash ^= tail + 0xA0761D6478BD642FULL + (hash << 6) + (hash >> 2);
            hash *= 0xE7037ED1A0B428DBULL;
        }
    }
    return hash ? hash : 1;
}

/* Upload the guest texture for a VP draw into a per-frame texture slot
 * (re-uploaded every frame: gcm/cube's plasma animates in guest memory).
 * Returns the slot index (SRV at heap 1+slot) or -1. Must run while the
 * command list is open, before the draw passes. */
static int vp_upload_tex_slot(u32 off, u32 w, u32 h, u32 fmt, u32 src_pitch)
{
    extern uint8_t* vm_base;
    if (!off || !w || !h || !vm_base || !s_d3d.srv_heap) return -1;
    u32 base_fmt = fmt & 0x9F;
    int d8   = (base_fmt == 0x9E);                /* D8R8G8B8: no alpha */
    int argb = (base_fmt == 0x85) || d8;          /* A8R8G8B8 */
    int bc1  = (base_fmt == 0x86);                /* DXT1 */
    int bc2  = (base_fmt == 0x87);                /* DXT23 / DXT3 */
    int bc3  = (base_fmt == 0x88);                /* DXT45 / DXT5 */
    int bc = bc1 || bc2 || bc3;
    u32 bpp = argb ? 4u : 1u;
    DXGI_FORMAT dxfmt = bc1 ? DXGI_FORMAT_BC1_UNORM :
                            (bc2 ? DXGI_FORMAT_BC2_UNORM :
                            (bc3 ? DXGI_FORMAT_BC3_UNORM :
                            (argb ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8_UNORM)));
    /* BC2 stores one 16-byte block per 4x4 texels. D3D's placed footprint
     * still uses the texture's texel dimensions, but RowPitch and the upload
     * buffer size are measured in compressed block rows. */
    u32 src_row_bytes = bc ? ((w + 3) / 4) * (bc1 ? 8u : 16u) : w * bpp;
    u32 copy_rows = bc ? (h + 3) / 4 : h;
    /* CONTROL3 is meaningful for linear textures. A zero/undersized value is
     * legal while state is being assembled, so retain tight-row fallback. */
    u32 guest_row_pitch = ((fmt & 0x20u) && src_pitch >= src_row_bytes)
        ? src_pitch : src_row_bytes;
    int slot = -1, pristine = -1, reusable = -1;
    for (int i = 0; i < VP_TEX_SLOTS; i++) {
        /* Keep a texture identity attached to the same resource across
         * frames.  Assigning slots solely by this frame's first-use order made
         * every later texture move whenever an animated UI layer inserted a
         * draw.  That caused long resource-reuse/reupload cascades precisely at
         * Lumen timeline boundaries.  Contents are still copied every frame,
         * so dynamic textures remain dynamic. */
        if (s_d3d.vp_tex[i].res && s_d3d.vp_tex[i].up &&
            s_d3d.vp_tex[i].off == off &&
            s_d3d.vp_tex[i].w == w && s_d3d.vp_tex[i].h == h &&
            s_d3d.vp_tex[i].fmt == fmt && s_d3d.vp_tex[i].pitch == src_pitch) {
            if (s_d3d.vp_tex[i].used)
                return i;                         /* already uploaded this frame */
            if (s_d3d.vp_tex[i].hash_valid) {
                const u64 source_hash = vp_source_hash(
                    off, src_row_bytes, copy_rows, guest_row_pitch);
                if (source_hash == s_d3d.vp_tex[i].source_hash) {
                    s_d3d.vp_tex[i].used = 1;
                    return i;                     /* unchanged since last frame */
                }
            }
            slot = i;
            break;
        }
        if (!s_d3d.vp_tex[i].res && pristine < 0) pristine = i;
        if (!s_d3d.vp_tex[i].used && reusable < 0) reusable = i;
    }
    if (slot < 0) slot = pristine >= 0 ? pristine : reusable;
    if (slot < 0) {
        static int warned = 0;
        if (warned++ < 8)
            fprintf(stderr,
                    "[VPTEX] texture pool exhausted (%d slots), dropping off=0x%X %ux%u fmt=0x%X\n",
                    VP_TEX_SLOTS, off, w, h, fmt);
        return -1;
    }
    VPTexSlot* t = &s_d3d.vp_tex[slot];
    /* Texture upload heaps are not parity-sliced.  They need their own reuse
     * fence, but only when content changed and an actual staging write occurs. */
    wait_for_fence(t->upload_fence);
    t->upload_fence = 0;
    u32 pitch = (src_row_bytes + 255) & ~255u;
    int fresh = 0;

    s_prof_frame_tex_uploads++;
    s_prof_frame_tex_bytes += (u64)src_row_bytes * copy_rows;

    /* Taiko's Lumen UI stacks several equal-Z quads.  If one of the tiny
     * mask textures loses its BC3 alpha, a later quad looks exactly like a
     * draw-order failure.  Keep this opt-in dump close to the source bytes so
     * it also tells us whether the corruption precedes D3D12 upload. */
    if (getenv("TEX_ALPHA_DUMP") && bc3 && w <= 16 && h <= 16) {
        static u32 dumped_off[32];
        static u32 dumped_n;
        int seen = 0;
        for (u32 i = 0; i < dumped_n; i++)
            if (dumped_off[i] == off) { seen = 1; break; }
        if (!seen && dumped_n < 32) {
            const u8* src = vm_base + off;
            u32 blocks_x = (w + 3) / 4, blocks_y = (h + 3) / 4;
            u32 alpha_zero = 0, alpha_full = 0, alpha_mid = 0;
            dumped_off[dumped_n++] = off;
            fprintf(stderr, "[BC3] off=0x%X %ux%u fmt=0x%X pitch=%u bytes:",
                    off, w, h, fmt, guest_row_pitch);
            for (u32 by = 0; by < blocks_y; by++) {
                const u8* row = src + (u64)by * guest_row_pitch;
                for (u32 bx = 0; bx < blocks_x; bx++) {
                    const u8* b = row + bx * 16;
                    u8 ap[8];
                    ap[0] = b[0]; ap[1] = b[1];
                    if (ap[0] > ap[1]) {
                        for (u32 k = 1; k <= 6; k++)
                            ap[k + 1] = (u8)(((7 - k) * ap[0] + k * ap[1]) / 7);
                    } else {
                        for (u32 k = 1; k <= 4; k++)
                            ap[k + 1] = (u8)(((5 - k) * ap[0] + k * ap[1]) / 5);
                        ap[6] = 0; ap[7] = 255;
                    }
                    u64 bits = 0;
                    for (u32 k = 0; k < 6; k++) bits |= (u64)b[2 + k] << (8 * k);
                    for (u32 k = 0; k < 16; k++) {
                        u8 a = ap[(bits >> (3 * k)) & 7];
                        if (a == 0) alpha_zero++;
                        else if (a == 255) alpha_full++;
                        else alpha_mid++;
                    }
                    fprintf(stderr, "\n[BC3]  block %u,%u a=%u,%u raw=",
                            bx, by, b[0], b[1]);
                    for (u32 k = 0; k < 16; k++) fprintf(stderr, "%02X", b[k]);
                }
            }
            fprintf(stderr, "\n[BC3]  alpha texels zero=%u mid=%u full=%u\n",
                    alpha_zero, alpha_mid, alpha_full);
        }
    }
    /* TEX_RAW_DUMP: one raw dump per distinct texture offset, so a screen the
     * capture scrolls past still lands in the set.  Decode with
     * tools/bc_decode.py. */
    if (getenv("TEX_RAW_DUMP") && (bc || argb) && w >= 256) {
        static u32 raw_dumped_off[128];
        static u32 raw_dumped_n;
        int seen = 0;
        for (u32 i = 0; i < raw_dumped_n; i++)
            if (raw_dumped_off[i] == off) { seen = 1; break; }
        if (!seen && raw_dumped_n < 128) {
            char path[96];
            snprintf(path, sizeof(path), "tex_%08X_%ux%u_%s_p%u.bin",
                     off, w, h,
                     bc1 ? "bc1" : bc2 ? "bc2" : bc3 ? "bc3" : "argb",
                     guest_row_pitch);
            FILE* f = fopen(path, "wb");
            if (f) {
                const u8* src = vm_base + off;
                for (u32 y = 0; y < copy_rows; y++)
                    fwrite(src + (u64)y * guest_row_pitch, 1, src_row_bytes, f);
                fclose(f);
                fprintf(stderr, "[TEXRAW] wrote %s (%u x %u-byte rows)\n",
                        path, copy_rows, src_row_bytes);
            }
            raw_dumped_off[raw_dumped_n++] = off;
        }
    }

    if (t->res && (t->w != w || t->h != h || t->fmt != fmt)) {
        srv_cache_invalidate_resource(t->res);
        t->res->lpVtbl->Release(t->res); t->res = NULL;
        if (t->up) { t->up->lpVtbl->Release(t->up); t->up = NULL; }
    }
    if (!t->res) {
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = dxfmt; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                &IID_ID3D12Resource, (void**)&t->res)))
            return -1;
        D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = (u64)pitch * copy_rows; bd.Height = 1; bd.DepthOrArraySize = 1;
        bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hu, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&t->up))) {
            t->res->lpVtbl->Release(t->res); t->res = NULL; return -1;
        }
        fresh = 1;
    }

    void* mapped = NULL; D3D12_RANGE nr = {0,0};
    if (FAILED(t->up->lpVtbl->Map(t->up, 0, &nr, &mapped)) || !mapped) return -1;
    { static int _tp=0; if ((getenv("RTT_DUMP") || s_rtt_hotkey_frames > 0) && _tp++ < 60) {
        const u8* sp = vm_base + off;
        fprintf(stderr, "[TEXUP] off=0x%X fmt=0x%X row0:", off, fmt);
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[_b]);
        fprintf(stderr, "  middle:");
        u64 mid = (u64)(copy_rows / 2) * guest_row_pitch + src_row_bytes / 2;
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[mid+_b]);
        fprintf(stderr, "%s", "\n");
    } }
    if (bc) {
        /* RSX 2D DXT blocks use the native BC byte layout. Compressed block
         * rows are linear even when the texture lacks CELL_GCM_TEXTURE_LN;
         * this matches RPCS3's 2D DXT upload path. */
        for (u32 y = 0; y < copy_rows; y++)
            memcpy((u8*)mapped + (u64)y * pitch,
                   vm_base + off + (u64)y * guest_row_pitch, src_row_bytes);
    } else if (argb) {
        /* Guest A8R8G8B8 bytes are A,R,G,B. LN textures use pitched rows;
         * without LN the texels are Morton-ordered (Taiko's 64x64 offline
         * phone/card icons were the visible failure case). */
        for (u32 y = 0; y < h; y++) {
            u8* drow = (u8*)mapped + (u64)y * pitch;
            for (u32 x = 0; x < w; x++) {
                u64 si = (fmt & 0x20u)
                    ? (u64)y * guest_row_pitch + x * 4u
                    : (u64)rsx_swizzled_index_2d(x, y, w, h) * 4u;
                const u8* s = vm_base + off + si;
                /* Store conventional D3D RGBA. D8 substitutes one for A;
                 * CONTROL1 is applied later by the SRV descriptor. */
                drow[x*4+0] = s[1];
                drow[x*4+1] = s[2];
                drow[x*4+2] = s[3];
                drow[x*4+3] = d8 ? 0xFFu : s[0];
            }
        }
    } else {
        for (u32 y = 0; y < h; y++) {
            u8* drow = (u8*)mapped + (u64)y * pitch;
            if (fmt & 0x20u) {
                memcpy(drow, vm_base + off + (u64)y * guest_row_pitch, w);
            } else {
                for (u32 x = 0; x < w; x++)
                    drow[x] = vm_base[off + rsx_swizzled_index_2d(x, y, w, h)];
            }
        }
    }
    t->up->lpVtbl->Unmap(t->up, 0, NULL);

    if (!fresh) {   /* reused resource: PSR -> COPY_DEST first */
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = t->res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = t->res; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = t->up;  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format   = dxfmt;
    src.PlacedFootprint.Footprint.Width    = w;
    src.PlacedFootprint.Footprint.Height   = h;
    src.PlacedFootprint.Footprint.Depth    = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
    {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = t->res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }
    /* Do not create a second, slot-indexed SRV here.  The caller writes this
     * resource directly into DRAW_SRV_BASE + draw*4 + unit.  The old 1+slot
     * descriptor path collided with DRAW_SRV_BASE once 32 unique textures
     * were uploaded: Taiko's optional Lumen passes then overwrote the timer's
     * plate and digit descriptors, producing its periodic black flash. */

    t->off = off; t->w = w; t->h = h; t->fmt = fmt;
    t->pitch = src_pitch; t->used = 1;
    t->source_hash = vp_source_hash(
        off, src_row_bytes, copy_rows, guest_row_pitch);
    t->hash_valid = 1;
    t->uploaded = 1;
    return slot;
}

/* ---------------------------------------------------------------------------
 * Render-to-texture: offscreen RT pool.
 *
 * Draws/clears targeting a non-display surface (SET_SURFACE_COLOR_xOFFSET
 * not registered via cellGcmSetDisplayBuffer) render into a pooled RGBA8
 * texture keyed by the surface's resolved vm offset; a later draw binding a
 * texture at that offset samples the RT directly instead of guest memory.
 * RGBA8 for every RT (even half-float guest surfaces) keeps the existing
 * PSO/RTV formats -- values clamp to [0,1], which demosaic's RGB data fits.
 * -----------------------------------------------------------------------*/

/* Snapshot the VP constant bank + viewport epilogue for one draw into its
 * vp_cb slot. Runs at record time so every draw keeps the constants that
 * were live when the guest issued it. */
static void vp_record_cb(u32 slot, int vs_idx, D3D12DrawRecord* dr)
{
    static int no_alphatest = -1, no_fixproj = -1, force_fixproj = -1;
    if (no_alphatest < 0) {
        no_alphatest = getenv("NO_ALPHATEST") ? 1 : 0;
        no_fixproj = getenv("VP_NOFIXPROJ") ? 1 : 0;
        force_fixproj = getenv("VP_FIXPROJ") ? 1 : 0;
    }
    const rsx_state* st = s_d3d.current_rsx_state;
    if (dr) dr->cb_slot = slot;
    if (!s_d3d.vp_cb_mapped || !st || slot >= MAX_DRAWS) return;
    if (dr) {
        dr->tie_c258z = st->vertex_constants[258][2];
        dr->tie_c259z = st->vertex_constants[259][2];
    }
    /* FP texcoord scale (b1): 1/size for UNnormalized textures (fmt bit
     * 0x40 -- wave samples everything in texel space), 1.0 otherwise. */
    if (s_d3d.vp_fpcb_mapped) {
        float* ts = (float*)((char*)s_d3d.vp_fpcb_mapped
            + ((u64)s_d3d.vp_parity * MAX_DRAWS + slot) * 256);
        for (int _u = 0; _u < 4; _u++) {
            float sx = 1.0f, sy = 1.0f;
            if (dr && dr->tex[_u].set && (dr->tex[_u].fmt & 0x40) &&
                dr->tex[_u].w && dr->tex[_u].h) {
                sx = 1.0f / (float)dr->tex[_u].w;
                sy = 1.0f / (float)dr->tex[_u].h;
            }
            ts[_u*4+0] = sx;
            ts[_u*4+1] = sy;
            /* z selects the neutral white texel for a unit with no texture:
             * a D3D12 null SRV samples as transparent black, which erases the
             * draw entirely.  This is a fallback, not a fix -- a draw landing
             * here usually means the backend dropped a binding it should have
             * kept (see TEXDROP).  FP_NOWHITE=1 disables it, so an untextured
             * draw goes back to transparent and you can see what an opaque
             * overlay is covering. */
            { static int nowhite = -1;
              if (nowhite < 0) nowhite = getenv("FP_NOWHITE") ? 1 : 0;
              ts[_u*4+2] = (nowhite || (dr && dr->tex[_u].set)) ? 1.0f : 0.0f; }
            ts[_u*4+3] = 0.0f;
        }
        /* rsx_alphatest (b1[4]): x=enable, y=ref/255, z=func-0x200. D3D12
         * has no fixed alpha test; guest FPs discard on it dynamically
         * (wave's colour wheel is a disc via alpha test -- without it the
         * palette drew as an opaque square). */
        if (dr && !no_alphatest) {
            { static int _at = 0;
              if (((dr->alpha_ctl >> 16) & 1u) && _at++ < 4)
                  printf("[ALPHATEST] enable func=%u ref=%u\n",
                         (dr->alpha_ctl >> 8) & 0xFFu, dr->alpha_ctl & 0xFFu); }
            ts[16] = (float)((dr->alpha_ctl >> 16) & 1u);
            ts[17] = (float)(dr->alpha_ctl & 0xFFu) / 255.0f;
            ts[18] = (float)((dr->alpha_ctl >> 8) & 0x07u);
            ts[19] = 0.0f;
        } else {
            ts[16] = 0.0f; ts[17] = 0.0f; ts[18] = 7.0f; ts[19] = 0.0f;
        }
    }
    char* dst = (char*)s_d3d.vp_cb_mapped
        + ((u64)s_d3d.vp_parity * MAX_DRAWS + slot) * VP_CB_STRIDE;
    memcpy(dst, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16);
    /* Viewport epilogue (see the render_frame notes this logic came from):
     * x/y identity, z lane remaps GL clip z when the guest programs one. */
    float* vpx = (float*)(dst + RSX_MAX_VERTEX_CONSTANTS * 16);
    const float* vs_ = st->viewport_scale;
    const float* vo_ = st->viewport_offset;
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    if (vs_[2] != 0.0f) { vpx[2] = vs_[2]; vpx[6] = vo_[2]; }
    else                { vpx[2] = 1.0f;   vpx[6] = 0.0f;   }
    /* Garbage-projection fallback (vkcube; see the original comment). */
    int uses_c03 = (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n)
                       ? s_d3d.vp_vs[vs_idx].uses_c03 : s_d3d.vp_uses_c03;
    float* c = (float*)dst;
    float c00 = st->vertex_constants[0][0];
    int garbage = !(c00 > 0.0f && c00 < 8.0f);
    if (no_fixproj) garbage = 0;
    if ((garbage && uses_c03) || force_fixproj) {
        { static int _fb = 0; if (_fb++ < 6)
            printf("[VPFB] fallback proj on draw slot %u (c00=%g vs_idx=%d)\n",
                   slot, c00, vs_idx); }
        for (int _i = 0; _i < 16; _i++) c[_i] = 0.0f;
        c[0]=1.358f; c[5]=2.414f; c[11]=1.0f;
        c[10]=1.0f/99.0f; c[14]=-1.0f/99.0f;
        vpx[2] = 1.0f; vpx[6] = 0.0f;
    }
}

/* Which offscreen surface (if any) does the current RSX state render to?
 * Returns 0 for a display buffer (backbuffer), else the surface's RAW RSX
 * offset. RTs are keyed by raw offset -- a texture bound at the same raw
 * offset is the same buffer (surface and texture registers share the offset
 * space; location bits differ but one title doesn't alias local vs main at
 * one offset), which sidesteps guessing the surface's context DMA location. */
/* Raw SET_SURFACE_COLOR_OFFSET for the active target, with no display-buffer
 * collapse. Trace probe: tells buffer A from buffer B. */
static u32 current_surface_raw(void)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!st) return 0;
    return st->surface_color_offset[(st->color_target == 2) ? 1 : 0];
}

static u32 current_rt_off(u32* out_w, u32* out_h, u32* out_off2)
{
    extern int cellGcmOffsetIsDisplay(u32 offset);
    const rsx_state* st = s_d3d.current_rsx_state;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_off2) *out_off2 = 0;
    if (!st) return 0;
    /* SET_SURFACE_COLOR_TARGET: 1 = A, 2 = B, 0x13 = MRT1 (A+B),
     * 0x17/0x1F = MRT2/3 (A+B+C[+D], C/D not wired yet -- log). */
    int sel = (st->color_target == 2) ? 1 : 0;
    u32 raw = st->surface_color_offset[sel];
    if (out_off2) {
        *out_off2 = 0;
        if (st->color_target >= 0x13) {
            *out_off2 = st->surface_color_offset[1];
            if (st->color_target > 0x13) {
                static int _w = 0;
                if (_w++ < 4) printf("[D3D12] MRT2/3 target 0x%X: only A+B wired\n",
                                     st->color_target);
            }
        }
    }
    if (cellGcmOffsetIsDisplay(raw)) return 0;
    /* Surface clip dims when sane; else the window size. Any size works --
     * passes draw normalized full-surface quads -- this only picks resolution. */
    u32 w = st->surface_clip_w, h = st->surface_clip_h;
    if (w < 16 || w > 2048 || h < 16 || h > 2048) { w = 0; h = 0; }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return raw;
}

/* RSX surface colour format (SET_SURFACE_FORMAT bits [4:0]) -> DXGI. Float
 * targets matter: wave's water height maps store SIGNED values (F_W16..),
 * demosaic's differential planes likewise -- RGBA8 clamps them to zero. */
static DXGI_FORMAT rsx_surface_dxgi(u32 fmt)
{
    switch (fmt & 0x1F) {
    case 0x0B: return DXGI_FORMAT_R16G16B16A16_FLOAT; /* F_W16Z16Y16X16 */
    case 0x0C: return DXGI_FORMAT_R32G32B32A32_FLOAT; /* F_W32Z32Y32X32 */
    case 0x0D: return DXGI_FORMAT_R32_FLOAT;          /* F_X32          */
    default:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

static int off_rt_find(u32 off)
{
    if (!off) return -1;
    for (int i = 0; i < MAX_OFF_RTS; i++)
        if (s_d3d.off_rt[i].res && s_d3d.off_rt[i].off == off) return i;
    return -1;
}

/* Ensure an RT resource exists for this surface; (re)creates the RTV at
 * rt_rtv_heap[i] and the SRV at srv_heap[RT_SRV_BASE+i]. */
static int off_rt_get(u32 off, u32 w, u32 h, u32 rsx_fmt)
{
    if (!off || !s_d3d.rt_rtv_heap || !s_d3d.srv_heap) return -1;
    if (!w) w = s_d3d.width;
    if (!h) h = s_d3d.height;
    DXGI_FORMAT want_fmt = rsx_surface_dxgi(rsx_fmt);
    int slot = off_rt_find(off);
    if (slot >= 0) {
        OffRT* r = &s_d3d.off_rt[slot];
        if (r->w == w && r->h == h && r->dxgi == (u32)want_fmt) { r->used = 1; return slot; }
        srv_cache_invalidate_resource(r->res);
        r->res->lpVtbl->Release(r->res); r->res = NULL;   /* dims/format changed */
        if (r->up) { r->up->lpVtbl->Release(r->up); r->up = NULL; }
    } else {
        for (int i = 0; i < MAX_OFF_RTS; i++)
            if (!s_d3d.off_rt[i].res) { slot = i; break; }
        if (slot < 0) {   /* evict an entry not used this frame */
            for (int i = 0; i < MAX_OFF_RTS; i++)
                if (!s_d3d.off_rt[i].used) { slot = i; break; }
            if (slot < 0) return -1;
            srv_cache_invalidate_resource(s_d3d.off_rt[slot].res);
            s_d3d.off_rt[slot].res->lpVtbl->Release(s_d3d.off_rt[slot].res);
            s_d3d.off_rt[slot].res = NULL;
            if (s_d3d.off_rt[slot].up) {
                s_d3d.off_rt[slot].up->lpVtbl->Release(s_d3d.off_rt[slot].up);
                s_d3d.off_rt[slot].up = NULL;
            }
        }
    }
    OffRT* r = &s_d3d.off_rt[slot];
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = want_fmt; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {0};
    cv.Format = td.Format;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, &cv,
            &IID_ID3D12Resource, (void**)&r->res)))
        return -1;
    r->off = off; r->w = w; r->h = h; r->dxgi = (u32)want_fmt;
    r->st = D3D12_RESOURCE_STATE_COPY_DEST;
    r->used = 1;

    /* Populate the new RT from guest memory: titles CPU-initialise their
     * render-target buffers (wave fills the height fields with texels of
     * (0,0,0, 1.0f) -- the .w is 'inverse of mass'; from an all-zero GPU
     * resource the water simulation can never boot). Guest data is
     * big-endian and tightly packed. */
    {
        extern uint8_t* vm_base;
        extern u32 cellGcmResolveOffset(u32);
        u32 bpp = (want_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8u :
                  (want_fmt == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16u :
                  (want_fmt == DXGI_FORMAT_R32_FLOAT) ? 4u : 4u;
        u32 pitch = (w * bpp + 255) & ~255u;
        u32 src = cellGcmResolveOffset(off);
        if (vm_base && src != 0xFFFFFFFFu) {
            D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd = {0};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = (u64)pitch * h; bd.Height = 1; bd.DepthOrArraySize = 1;
            bd.MipLevels = 1; bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hu, D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                    &IID_ID3D12Resource, (void**)&r->up))) {
                void* mp = NULL; D3D12_RANGE nr = {0,0};
                if (SUCCEEDED(r->up->lpVtbl->Map(r->up, 0, &nr, &mp)) && mp) {
                    const u8* sp = vm_base + src;
                    for (u32 y = 0; y < h; y++) {
                        const u8* srow = sp + (u64)y * w * bpp;
                        u8* drow = (u8*)mp + (u64)y * pitch;
                        if (want_fmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
                            /* guest A8R8G8B8 (bytes A,R,G,B) -> R,G,B,A */
                            for (u32 x = 0; x < w; x++) {
                                drow[x*4+0] = srow[x*4+1];
                                drow[x*4+1] = srow[x*4+2];
                                drow[x*4+2] = srow[x*4+3];
                                drow[x*4+3] = srow[x*4+0];
                            }
                        } else if (want_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                            for (u32 x = 0; x < w * 4; x++) {   /* u16 halves, BE */
                                drow[x*2+0] = srow[x*2+1];
                                drow[x*2+1] = srow[x*2+0];
                            }
                        } else {                                 /* u32 floats, BE */
                            u32 nw = (w * bpp) / 4;
                            for (u32 x = 0; x < nw; x++) {
                                drow[x*4+0] = srow[x*4+3];
                                drow[x*4+1] = srow[x*4+2];
                                drow[x*4+2] = srow[x*4+1];
                                drow[x*4+3] = srow[x*4+0];
                            }
                        }
                    }
                    r->up->lpVtbl->Unmap(r->up, 0, NULL);
                    D3D12_TEXTURE_COPY_LOCATION cdst = {0}, csrc = {0};
                    cdst.pResource = r->res;
                    cdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    csrc.pResource = r->up;
                    csrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    csrc.PlacedFootprint.Footprint.Format   = want_fmt;
                    csrc.PlacedFootprint.Footprint.Width    = w;
                    csrc.PlacedFootprint.Footprint.Height   = h;
                    csrc.PlacedFootprint.Footprint.Depth    = 1;
                    csrc.PlacedFootprint.Footprint.RowPitch = pitch;
                    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cdst, 0, 0, 0, &csrc, NULL);
                }
            }
        }
        {
            D3D12_RESOURCE_BARRIER b = {0};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource   = r->res;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
            r->st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rh;
    s_d3d.rt_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rt_rtv_heap, &rh);
    rh.ptr += (u64)slot * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, r->res, NULL, rh);

    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = td.Format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sh;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &sh);
    sh.ptr += (u64)(RT_SRV_BASE + slot) * s_d3d.srv_inc;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, r->res, &sv, sh);

    static int _log = 0;
    if (_log++ < 8)
        printf("[D3D12] offscreen RT %d: off=0x%X %ux%u (render-to-texture)\n",
               slot, off, w, h);
    return slot;
}

static D3D12_CPU_DESCRIPTOR_HANDLE off_rt_rtv(int slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rh;
    s_d3d.rt_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rt_rtv_heap, &rh);
    rh.ptr += (u64)slot * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return rh;
}

/* Write a texture SRV (or a null SRV when res == NULL) at an absolute SRV
 * heap slot. Used to fill per-draw t0-t3 descriptor windows. */
static void srv_cache_invalidate_resource(ID3D12Resource* res)
{
    if (!res) return;
    for (u32 i = 0; i < SRV_HEAP_SIZE; ++i)
        if (s_srv_cache[i].valid && s_srv_cache[i].res == res)
            s_srv_cache[i].valid = 0;
}

static void srv_write(u32 heap_slot, ID3D12Resource* res, DXGI_FORMAT fmt, UINT mapping)
{
    if (heap_slot >= SRV_HEAP_SIZE) return;
    SRVCacheEntry* cached = &s_srv_cache[heap_slot];
    if (cached->valid && cached->res == res &&
        cached->fmt == (u32)fmt && cached->mapping == mapping) {
        if (rsx_profile_enabled()) s_prof.srv_skips++;
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = mapping;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &h);
    h.ptr += (u64)heap_slot * s_d3d.srv_inc;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, res, &sv, h);
    cached->res = res;
    cached->fmt = (u32)fmt;
    cached->mapping = mapping;
    cached->valid = 1;
    if (rsx_profile_enabled()) s_prof.srv_writes++;
}

static void off_rt_transition(int slot, D3D12_RESOURCE_STATES to)
{
    OffRT* r = &s_d3d.off_rt[slot];
    if (r->st == to) return;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = r->res;
    b.Transition.StateBefore = r->st;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    r->st = to;
}

static void render_frame(void)
{
    u32 fi = s_d3d.frame_index;
    const int submitted_upload_parity = s_d3d.vp_parity;
    const u32 vp_gpu_vb_bytes = s_d3d.vp_vb_offset;
    const u32 vp_gpu_draws = s_d3d.draw_count < MAX_DRAWS
        ? s_d3d.draw_count : MAX_DRAWS;
    int vp_gpu_snapshot = 0;
    const int profiling = rsx_profile_enabled();
    LARGE_INTEGER prof_t[8];
    memset(prof_t, 0, sizeof(prof_t));
    const u32 prof_draws = s_d3d.draw_count;
    s_prof_frame_tex_uploads = 0;
    s_prof_frame_tex_bytes = 0;
    if (profiling) QueryPerformanceCounter(&prof_t[0]);

    if (getenv("RTT_DUMP") && s_d3d.draw_count > 0)
        fprintf(stderr, "[FRAMEIDX] fi=%u parity=%d draws=%u\n",
                fi, s_d3d.vp_parity, s_d3d.draw_count);

    /* Drain the GPU before touching shared upload resources (vp_vb vertices,
     * vp_cb constants, per-frame texture staging): the previous frame's draws
     * may still be reading them, and overwriting mid-flight tears geometry
     * (gcm/cube: triangles mixing stale and new vertices -> missing/sliver
     * polygons). These workloads are a few draws/frame, so full serialisation
     * costs little. */
    protect_upload_heaps_for_batch();
    if (profiling) QueryPerformanceCounter(&prof_t[1]);

    /* F9's full 120-frame BMP run outlives the deliberately short, verbose
     * RTT trace.  Keep one compact line per captured frame so periodic Lumen
     * animation failures can be correlated with the matching frame_NNN.bmp.
     * The first five draws are the player-entry timer's nested sprite layers;
     * the m* records fingerprint each Don-chan material draw at the point the
     * immutable host vertices exist but before replay/upload. */
    if (s_d3d.dump_frames_left > 0 && s_d3d.draw_count > 0) {
        char capline[16384];
        u32 caplen = 0;
#define CAP_APPEND(...) do {                                                     \
            if (caplen < sizeof(capline)) {                                      \
                int _n = snprintf(capline + caplen, sizeof(capline) - caplen,     \
                                  __VA_ARGS__);                                   \
                if (_n > 0) {                                                    \
                    u32 _u = (u32)_n;                                            \
                    caplen += _u < sizeof(capline) - caplen                      \
                        ? _u : (u32)(sizeof(capline) - caplen - 1);               \
                }                                                                \
            }                                                                    \
        } while (0)
        const u8* vb = (const u8*)s_d3d.vp_vb_mapped
            + (u64)s_d3d.vp_parity * MAX_VERTICES * 256;
        const u8* cb = (const u8*)s_d3d.vp_cb_mapped
            + (u64)s_d3d.vp_parity * MAX_DRAWS * VP_CB_STRIDE;
        CAP_APPEND("[CAP] left=%d draws=%u fi=%u parity=%d seq=%u..%u",
                   s_d3d.dump_frames_left, s_d3d.draw_count, fi, s_d3d.vp_parity,
                   s_d3d.draws[0].seq,
                   s_d3d.draws[(s_d3d.draw_count < MAX_DRAWS
                       ? s_d3d.draw_count : MAX_DRAWS) - 1].seq);
        for (u32 d = 0; d < s_d3d.draw_count && d < 5; d++) {
            const D3D12DrawRecord* dr = &s_d3d.draws[d];
            const float* dv = (const float*)(vb + dr->vb_byte_offset);
            const float* dc = (const float*)(cb + (u64)dr->cb_slot * VP_CB_STRIDE);
            CAP_APPEND(" d%u=%X/a%.3g/at%05X/xy%.4g,%.4g/c467%.4g",
                       d, dr->tex[0].raw, dv[15], dr->alpha_ctl,
                       dc[259 * 4], dc[259 * 4 + 1], dc[467 * 4]);
        }
        u32 mesh = 0;
        for (u32 d = 0; d < s_d3d.draw_count && d < MAX_DRAWS; d++) {
            const D3D12DrawRecord* dr = &s_d3d.draws[d];
            const u32 fpa = dr->fp_addr & ~3u;
            const int is_fill = fpa == 0x01AA8E40u;
            const int is_outline = fpa == 0x01AA8F00u;
            if (!dr->is_vp || dr->is_clear || (!is_fill && !is_outline))
                continue;
            const u8* dv = vb + dr->vb_byte_offset;
            const u32 vh = vp_hash_ucode(dv, dr->vertex_count * 256u);
            const u32 ph = vp_vertex_attr_hash(dv, dr->vertex_count, 0);
            const u32 nh = vp_vertex_attr_hash(dv, dr->vertex_count, 3);
            const u32 uh = vp_vertex_attr_hash(dv, dr->vertex_count, 8);
            const u32 th = vp_texture_source_hash(dr, 0);
            const u8* dc = cb + (u64)dr->cb_slot * VP_CB_STRIDE;
            const u32 ch = vp_hash_ucode(dc,
                RSX_MAX_VERTEX_CONSTANTS * 16u + 8u * sizeof(float));
            const u8* fc = s_d3d.vp_fpcb_mapped
                ? (const u8*)s_d3d.vp_fpcb_mapped
                    + ((u64)s_d3d.vp_parity * MAX_DRAWS + dr->cb_slot) * 256
                : NULL;
            const u32 fh = fc ? vp_hash_ucode(fc, 20u * sizeof(float)) : 0;
            const u32 vsh = (dr->vs_idx >= 0 && dr->vs_idx < s_d3d.vp_vs_n)
                ? s_d3d.vp_vs[dr->vs_idx].hash : 0;
            CAP_APPEND(
                " m%u=%c/o%u/n%u/v%08X/p%08X/n%08X/u%08X/c%08X/q%08X/t%08X/x%X/%ux%u/f%X/h%08X/z%08X/s%u",
                mesh++, is_fill ? 'F' : 'O', d, dr->vertex_count,
                vh, ph, nh, uh, ch, fh, th,
                dr->tex[0].raw, dr->tex[0].w, dr->tex[0].h,
                dr->tex[0].fmt, dr->fp_hash, vsh, dr->seq);
        }
        CAP_APPEND("\n");
        fwrite(capline, 1, caplen, stderr);
#undef CAP_APPEND
    }

    {
        HRESULT removed = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
        if (FAILED(removed)) {
            printf("[D3D12] device lost before command reset: 0x%08lX\n", (long)removed);
            s_d3d.window_closed = 1;
            return;
        }
    }

    /* Compile the real vertex program once its microcode is captured, and keep
     * the constant bank uploaded for the VS. */
    if (s_d3d.current_rsx_state && !s_d3d.vp_ready &&
        s_d3d.current_rsx_state->vp_ucode_bytes >= 16 &&
        s_d3d.vp_compiled_bytes != s_d3d.current_rsx_state->vp_ucode_bytes)
        compile_vp();
    /* Per-draw VP constants are snapshotted at record time (vp_record_cb). */

    /* Lazily create the readback buffer the first time a dump is requested. */
    if (s_d3d.dump_frames_left > 0 && !s_d3d.readback_buf) {
        s_d3d.readback_pitch = (s_d3d.width * 4 + 255) & ~255u;
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (u64)s_d3d.readback_pitch * s_d3d.height;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.readback_buf);
    }

    /* Reset command allocator and list */
    HRESULT alloc_reset_hr = s_d3d.cmd_allocators[fi]->lpVtbl->Reset(s_d3d.cmd_allocators[fi]);
    if (FAILED(alloc_reset_hr)) {
        HRESULT removed = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
        printf("[D3D12] allocator reset failed: 0x%08lX removed=0x%08lX\n",
               (long)alloc_reset_hr, (long)removed);
        s_d3d.window_closed = 1;
        return;
    }
    HRESULT list_reset_hr = s_d3d.cmd_list->lpVtbl->Reset(
        s_d3d.cmd_list, s_d3d.cmd_allocators[fi], NULL);
    if (FAILED(list_reset_hr)) {
        HRESULT removed = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
        printf("[D3D12] command-list reset failed: 0x%08lX removed=0x%08lX\n",
               (long)list_reset_hr, (long)removed);
        s_d3d.window_closed = 1;
        return;
    }

    if (vp_gpu_vb_bytes && vp_gpu_draws &&
        s_d3d.vp_vb && s_d3d.vp_cb && s_d3d.vp_fpcb &&
        s_d3d.vp_vb_gpu && s_d3d.vp_cb_gpu && s_d3d.vp_fpcb_gpu) {
        const u64 vb_base = (u64)s_d3d.vp_parity * MAX_VERTICES * 256;
        const u64 cb_base = (u64)s_d3d.vp_parity * MAX_DRAWS * VP_CB_STRIDE;
        const u64 fp_base = (u64)s_d3d.vp_parity * MAX_DRAWS * 256;
        s_d3d.cmd_list->lpVtbl->CopyBufferRegion(
            s_d3d.cmd_list, s_d3d.vp_vb_gpu, vb_base,
            s_d3d.vp_vb, vb_base, vp_gpu_vb_bytes);
        s_d3d.cmd_list->lpVtbl->CopyBufferRegion(
            s_d3d.cmd_list, s_d3d.vp_cb_gpu, cb_base,
            s_d3d.vp_cb, cb_base, (u64)vp_gpu_draws * VP_CB_STRIDE);
        s_d3d.cmd_list->lpVtbl->CopyBufferRegion(
            s_d3d.cmd_list, s_d3d.vp_fpcb_gpu, fp_base,
            s_d3d.vp_fpcb, fp_base, (u64)vp_gpu_draws * 256);

        D3D12_RESOURCE_BARRIER bb[3] = {0};
        ID3D12Resource* rr[3] = {
            s_d3d.vp_vb_gpu, s_d3d.vp_cb_gpu, s_d3d.vp_fpcb_gpu
        };
        for (int i = 0; i < 3; i++) {
            bb[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bb[i].Transition.pResource = rr[i];
            bb[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            bb[i].Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            bb[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 3, bb);
        vp_gpu_snapshot = 1;
    }

    /* Upload the bound font atlas into an R8_UNORM texture and create its SRV.
     * The atlas is a linear 8-bit coverage map, so a straight row copy (no
     * deswizzle) suffices. It is a DYNAMIC glyph cache -- the game rasterizes
     * new glyphs into it over time -- so re-upload whenever it is dirty (set on
     * every bind), not just once. Uploading only on first bind left every glyph
     * added after frame 1 sampling stale texels -> torn text. The backend is
     * synchronous (wait_for_gpu each frame), so recreating the resource per
     * dirty frame is safe. */
    if (s_d3d.tex_bound && (!s_d3d.tex_ready || s_d3d.tex_dirty) && s_d3d.srv_heap && s_d3d.pipeline_state_tex) {
        extern uint8_t* vm_base;
        u32 w = s_d3d.tex_w, h = s_d3d.tex_h;
        u32 pitch = (w + 255) & ~255u;   /* D3D12 requires 256-byte row pitch */

        if (s_d3d.tex_resource) { s_d3d.tex_resource->lpVtbl->Release(s_d3d.tex_resource); s_d3d.tex_resource = NULL; }
        if (s_d3d.tex_upload)   { s_d3d.tex_upload->lpVtbl->Release(s_d3d.tex_upload);     s_d3d.tex_upload = NULL; }

        D3D12_HEAP_PROPERTIES hp_def = {0}; hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        HRESULT thr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp_def, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.tex_resource);

        D3D12_HEAP_PROPERTIES hp_up = {0}; hp_up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC ud = {0};
        ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ud.Width = (u64)pitch * h; ud.Height = 1; ud.DepthOrArraySize = 1; ud.MipLevels = 1;
        ud.SampleDesc.Count = 1; ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(thr))
            thr = s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hp_up, D3D12_HEAP_FLAG_NONE, &ud,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.tex_upload);

        if (SUCCEEDED(thr) && vm_base) {
            void* mapped = NULL;
            D3D12_RANGE nr = {0, 0};
            if (SUCCEEDED(s_d3d.tex_upload->lpVtbl->Map(s_d3d.tex_upload, 0, &nr, &mapped)) && mapped) {
                const u8* srcbase = vm_base + s_d3d.tex_src_offset;
                for (u32 y = 0; y < h; y++)
                    memcpy((u8*)mapped + (u64)y * pitch, srcbase + (u64)y * w, w);
                s_d3d.tex_upload->lpVtbl->Unmap(s_d3d.tex_upload, 0, NULL);

                D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
                dst.pResource = s_d3d.tex_resource;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = 0;
                src.pResource = s_d3d.tex_upload;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint.Offset = 0;
                src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8_UNORM;
                src.PlacedFootprint.Footprint.Width    = w;
                src.PlacedFootprint.Footprint.Height   = h;
                src.PlacedFootprint.Footprint.Depth    = 1;
                src.PlacedFootprint.Footprint.RowPitch = pitch;
                s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);

                D3D12_RESOURCE_BARRIER tb = {0};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource   = s_d3d.tex_resource;
                tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &tb);

                D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
                sv.Format = DXGI_FORMAT_R8_UNORM;
                sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sv.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE sh;
                s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &sh);
                s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, s_d3d.tex_resource, &sv, sh);

                s_d3d.tex_ready = 1;
                s_d3d.tex_dirty = 0;   /* content now in sync with guest atlas */
                { static int _au = 0; if (_au++ < 3)
                    printf("[D3D12] atlas uploaded (%ux%u R8) -> textured\n", w, h); }
            }
        }
    }

    int trace_match = 1;
    if (getenv("RTT_NONATLAS")) {
        trace_match = 0;
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            u32 raw = s_d3d.draws[_d].tex[0].raw;
            if (raw && raw != 0x00CC0300u) { trace_match = 1; break; }
        }
    }
    { const char* trace_tex = getenv("RTT_TEX");
      if (trace_tex) {
          u32 want_tex = (u32)strtoul(trace_tex, NULL, 16);
          trace_match = 0;
          for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++)
              if (s_d3d.draws[_d].tex[0].raw == want_tex) {
                  trace_match = 1;
                  break;
              }
      } }
    { const char* _rde = getenv("RTT_DUMP"); int _hot = s_rtt_hotkey_frames > 0;
    if ((_rde || _hot) && trace_match && s_d3d.draw_count > 0) { static int _f=0; int _cap = _rde ? atoi(_rde) : 0; if (_cap < 2) _cap = 14; if (_hot) s_rtt_hotkey_frames--; if (_f++ < _cap || _hot) {
        fprintf(stderr,
                "[RTT] frame %d: %u ops fi=%u parity=%d reset=0x%08lX/0x%08lX\n",
                _f, s_d3d.draw_count, fi, s_d3d.vp_parity,
                (long)alloc_reset_hr, (long)list_reset_hr);
        {
            extern uint8_t* vm_base;
            const u8* vb = (const u8*)s_d3d.vp_vb_mapped
                + (u64)s_d3d.vp_parity * MAX_VERTICES * 256;
            const u8* cb = (const u8*)s_d3d.vp_cb_mapped
                + (u64)s_d3d.vp_parity * MAX_DRAWS * VP_CB_STRIDE;
            u32 vh = vp_hash_ucode(vb, s_d3d.vp_vb_offset);
            u32 ch = vp_hash_ucode(cb, s_d3d.draw_count * VP_CB_STRIDE);
            u32 th = 0, tn = 0;
            for (u32 d = 0; d < s_d3d.draw_count && d < MAX_DRAWS; d++) {
                D3D12DrawRecord* r = &s_d3d.draws[d];
                if (!r->tex[0].off || !r->tex[0].w || !r->tex[0].h ||
                    r->tex[0].w > 4096 || r->tex[0].h > 4096 || !vm_base)
                    continue;
                u32 fmt = r->tex[0].fmt & 0x9F;
                tn = (fmt == 0x87)
                    ? ((r->tex[0].w + 3) / 4) * 16 * ((r->tex[0].h + 3) / 4)
                    : r->tex[0].w * r->tex[0].h * (fmt == 0x85 || fmt == 0x9E ? 4 : 1);
                th = vp_hash_ucode(vm_base + r->tex[0].off, tn);
                break;
            }
            fprintf(stderr,
                    "[RTT]  hashes vb=%08X/%u cb=%08X tex=%08X/%u\n",
                    vh, s_d3d.vp_vb_offset, ch, th, tn);
            if (s_d3d.current_rsx_state) {
                const rsx_vertex_attrib* a0 = &s_d3d.current_rsx_state->vertex_attribs[0];
                const rsx_vertex_attrib* a1 = &s_d3d.current_rsx_state->vertex_attribs[1];
                const rsx_vertex_attrib* a2 = &s_d3d.current_rsx_state->vertex_attribs[2];
                fprintf(stderr,
                        "[RTT]  attrs a0=%08X/%u a1=%08X/%u a2=%08X/%u\n",
                        a0->offset, a0->stride, a1->offset, a1->stride,
                        a2->offset, a2->stride);
            }
            const float* vv = (const float*)vb;
            for (u32 vi = 0; vi < 3 && vi * 256 < s_d3d.vp_vb_offset; vi++)
                fprintf(stderr,
                        "[RTT]  v%u a0=(%g,%g,%g,%g) a1=(%g,%g,%g,%g) a2=(%g,%g,%g,%g)\n",
                        vi,
                        vv[vi*64+0], vv[vi*64+1], vv[vi*64+2], vv[vi*64+3],
                        vv[vi*64+4], vv[vi*64+5], vv[vi*64+6], vv[vi*64+7],
                        vv[vi*64+8], vv[vi*64+9], vv[vi*64+10], vv[vi*64+11]);
        }
        const u8* trace_cb = (const u8*)s_d3d.vp_cb_mapped
            + (u64)s_d3d.vp_parity * MAX_DRAWS * VP_CB_STRIDE;
        const u8* trace_vb = (const u8*)s_d3d.vp_vb_mapped
            + (u64)s_d3d.vp_parity * MAX_VERTICES * 256;
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            D3D12DrawRecord* r = &s_d3d.draws[_d];
            const float* dc = (const float*)(trace_cb + (u64)r->cb_slot * VP_CB_STRIDE);
            const float* dv = (const float*)(trace_vb + r->vb_byte_offset);
            /* Taiko uses two Lumen sprite VPs. Slot 0 adds c467.x to every
             * position lane; slot 1 uses c467.xy as texture dimensions and
             * transforms a0.z through c258 instead. */
            float raw_z, raw_w;
            if (r->vs_idx == 1) {
                raw_z = dv[2] * dc[258 * 4 + 2] + dc[259 * 4 + 2];
                raw_w = dv[2] * dc[258 * 4 + 3] + dc[259 * 4 + 3];
            } else {
                raw_z = dc[259 * 4 + 2] + dc[467 * 4 + 0];
                raw_w = dc[259 * 4 + 3] + dc[467 * 4 + 0];
            }
            float out_z = raw_z * dc[RSX_MAX_VERTEX_CONSTANTS * 4 + 2]
                        + raw_w * dc[RSX_MAX_VERTEX_CONSTANTS * 4 + 6];
            float ndc_z = raw_w != 0.0f ? out_z / raw_w : out_z;
            /* Per-op source-texture hash: distinguishes "texture never filled"
             * (hash of all-zero bytes) from a layering/ordering problem. */
            u32 t0h = 0;
            { extern uint8_t* vm_base;
              u32 f = r->tex[0].fmt & 0x9F, w = r->tex[0].w, h = r->tex[0].h;
              if (vm_base && r->tex[0].off && w && h && w <= 4096 && h <= 4096) {
                  u32 n = (f == 0x86) ? ((w + 3) / 4) * 8 * ((h + 3) / 4)
                        : (f == 0x87 || f == 0x88) ? ((w + 3) / 4) * 16 * ((h + 3) / 4)
                        : w * h * (f == 0x85 ? 4 : 1);
                  t0h = vp_hash_ucode(vm_base + r->tex[0].off, n);
              } }
            const float* tfp = (const float*)((const u8*)s_d3d.vp_fpcb_mapped
                + ((u64)s_d3d.vp_parity * MAX_DRAWS + r->cb_slot) * 256);
            fprintf(stderr, "[RTT]  op%02u %s fifo=0x%06X seq=%u t0h=%08X fph=%08X/%08X rt=0x%X raw=0x%X rt2=0x%X t0=0x%X t0fmt=0x%X t0dim=%ux%u t0addr=%08X ts=(%.9g,%.9g) fp=0x%X n=%u vb=%u vs=%d cmask=%X alpha=%05X vp=%u,%u %ux%u sc=%u,%u %ux%u blend=%d/%08X/%08X/%08X depth=%d/%d/%03X cull=%d/%03X/%03X stencil=%d/%03X/%u/%02X/%04X,%04X,%04X a0.z=%.9a a3=(%.4g,%.4g,%.4g,%.4g) c258.z=%.9a c259.zw=(%.9a,%.9a) vpz=(%.9a,%.9a) c467.xy=(%.9a,%.9a) z=%.9a\n",
                _d, r->is_clear?"CLR ":(r->is_vp?"draw":"leg "), r->fifo_off, r->seq, t0h, r->fp_hash, fp_ucode_hash(r->fp_addr), r->rt_off, r->rt_raw, r->rt_off2,
                r->tex[0].raw, r->tex[0].fmt, r->tex[0].w, r->tex[0].h,
                r->tex[0].address, tfp[0], tfp[1],
                r->fp_addr, r->vertex_count, r->vb_byte_offset, r->vs_idx,
                r->cmask, r->alpha_ctl, r->vp_x, r->vp_y, r->vp_w, r->vp_h,
                r->sc_x, r->sc_y, r->sc_w, r->sc_h, r->blend,
                r->blend_sf, r->blend_df, r->blend_eq,
                r->depth_test, r->depth_mask, r->depth_func & 0xFFF,
                r->cull_enable, r->cull_face & 0xFFF, r->front_face & 0xFFF,
                r->stencil_test, r->stencil_func & 0xFFF, r->stencil_ref,
                r->stencil_mask & 0xFF, r->stencil_fail & 0xFFFF,
                r->stencil_zfail & 0xFFFF, r->stencil_zpass & 0xFFFF,
                dv[2], dv[12], dv[13], dv[14], dv[15],
                dc[258 * 4 + 2], dc[259 * 4 + 2], dc[259 * 4 + 3],
                dc[RSX_MAX_VERTEX_CONSTANTS * 4 + 2],
                dc[RSX_MAX_VERTEX_CONSTANTS * 4 + 6],
                dc[467 * 4 + 0], dc[467 * 4 + 1], ndc_z);
            if ((getenv("RTT_VERTS") || _hot) && r->is_vp && !r->is_clear) {
                fprintf(stderr,
                        "[RTT]    xform c256=(%.9g,%.9g,%.9g,%.9g) c257=(%.9g,%.9g,%.9g,%.9g) c259=(%.9g,%.9g,%.9g,%.9g)\n",
                        dc[1024], dc[1025], dc[1026], dc[1027],
                        dc[1028], dc[1029], dc[1030], dc[1031],
                        dc[1036], dc[1037], dc[1038], dc[1039]);
                u32 nv = r->vertex_count < 4 ? r->vertex_count : 4;
                for (u32 vi = 0; vi < nv; vi++) {
                    const float* v = (const float*)((const u8*)dv + (u64)vi * 256);
                    fprintf(stderr,
                            "[RTT]    v%u a0=(%.7g,%.7g,%.7g,%.7g) a3=(%.5g,%.5g,%.5g,%.5g) a8=(%.7g,%.7g,%.7g,%.7g)\n",
                            vi, v[0], v[1], v[2], v[3],
                            v[12], v[13], v[14], v[15],
                            v[32], v[33], v[34], v[35]);
                }
            }
        }
    } } }

    /* Debug: RTT_PASS=N shows pass N's output directly on screen (drops later
     * ops, retargets pass N to the backbuffer). */
    { const char* rp = getenv("RTT_PASS");
      if (rp) {
        int keep = atoi(rp), seen = 0; u32 cut = s_d3d.draw_count;
        int viewrt = getenv("RTT_VIEWRT") != NULL;
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            D3D12DrawRecord* r = &s_d3d.draws[_d];
            if (r->is_clear || !r->is_vp) continue;
            if (seen == keep) {
                /* Alone: retarget op N to the backbuffer. With RTT_VIEWRT:
                 * keep op N intact and pull the display composite forward so
                 * the chosen RT is shown as of this point in the chain. */
                if (!viewrt) { r->rt_off = 0; r->rt_off2 = 0; }
                cut = _d + 1;
                break;
            }
            seen++;
        }
        if (viewrt) {
            for (u32 _j = cut; _j < s_d3d.draw_count && _j < MAX_DRAWS; _j++) {
                D3D12DrawRecord* r = &s_d3d.draws[_j];
                if (r->is_vp && !r->is_clear && r->rt_off == 0) {
                    if (cut < MAX_DRAWS) s_d3d.draws[cut++] = *r;
                    break;
                }
            }
        }
        s_d3d.draw_count = (cut < s_d3d.draw_count) ? cut : s_d3d.draw_count;
      } }

    /* RTT_UNREVERSE=1 -- DIAGNOSTIC, not a fix.
     *
     * Comparison against an RPCS3 capture shows our lifted guest code emits each
     * run of equal-depth quads in exactly REVERSED order (logo screen: hardware
     * draws white then logo, we draw logo then white; both z=1000). Runs with
     * distinct depth are ordered correctly. The real bug is CPU-side, in the
     * Lumen display-list insert -- this just flips tied runs back so the
     * diagnosis can be confirmed on screen.
     *
     * Tie key is the depth inputs the VP actually consumes: vertex z plus the
     * c258.z / c259.z constants. Restricted to consecutive display draws so
    * offscreen RT chains keep their ordering. */
    if (getenv("RTT_UNREVERSE")) {
        u32 total = s_d3d.draw_count < MAX_DRAWS ? s_d3d.draw_count : MAX_DRAWS;
        u32 i = 0;
        while (i < total) {
            D3D12DrawRecord* a = &s_d3d.draws[i];
            if (!a->is_vp || a->is_clear || a->rt_off || a->rt_off2) { i++; continue; }
            u32 j = i + 1;
            while (j < total) {
                D3D12DrawRecord* b = &s_d3d.draws[j];
                if (!b->is_vp || b->is_clear || b->rt_off || b->rt_off2) break;
                if (a->tie_vz != b->tie_vz ||
                    a->tie_c258z != b->tie_c258z ||
                    a->tie_c259z != b->tie_c259z) break;
                j++;
            }
            if (j - i >= 2) {
                /* cb_slot follows each record, so upload heaps stay write-only. */
                for (u32 lo = i, hi = j - 1; lo < hi; lo++, hi--) {
                    D3D12DrawRecord t = s_d3d.draws[lo];
                    s_d3d.draws[lo] = s_d3d.draws[hi];
                    s_d3d.draws[hi] = t;
                }
                { static int _u = 0; if (_u++ < 12)
                    fprintf(stderr, "[UNREV] reversed tied run [%u..%u]\n", i, j - 1); }
            }
            i = j;
        }
    }

    /* A nested Lumen movie carries its own local Z values.  Taiko flattens
     * those movies into the display list in whole groups; the player-entry
     * timer is consequently submitted before its parent backdrop even though
     * its group Z (-6) places it between the backdrop (-1) and foreground UI
     * (-950 and below).  With depth writes enabled, the timer's translucent
     * shadow then blends against the black clear and prevents the backdrop
     * from filling those pixels.
     *
     * Sort only WHOLE equal-Z groups, far-to-near by Lumen's source vertex Z.
     * Never sort individual quads: c467 contains texture dimensions rather
     * than a global depth, and using the post-VP value globally crosses
     * unrelated layers.  Equal groups remain stable and RTT_UNREVERSE keeps
     * responsibility for their known CPU-side reversal. */
    if (getenv("RTT_SORT_LUMEN_GROUPS")) {
        typedef struct {
            u32 begin, end;
            float z;
        } LumenGroup;
        static D3D12DrawRecord ordered[MAX_DRAWS];
        LumenGroup groups[MAX_DRAWS];
        u32 total = s_d3d.draw_count < MAX_DRAWS ? s_d3d.draw_count : MAX_DRAWS;
        u32 seg = 0;

        while (seg < total) {
            D3D12DrawRecord* first = &s_d3d.draws[seg];
            if (!first->is_vp || first->is_clear || first->rt_off || first->rt_off2) {
                seg++;
                continue;
            }

            u32 seg_end = seg;
            while (seg_end < total) {
                D3D12DrawRecord* r = &s_d3d.draws[seg_end];
                if (!r->is_vp || r->is_clear || r->rt_off || r->rt_off2) break;
                seg_end++;
            }

            u32 ng = 0;
            for (u32 p = seg; p < seg_end;) {
                D3D12DrawRecord* a = &s_d3d.draws[p];
                u32 q = p + 1;
                while (q < seg_end) {
                    D3D12DrawRecord* b = &s_d3d.draws[q];
                    if (a->tie_vz != b->tie_vz ||
                        a->tie_c258z != b->tie_c258z ||
                        a->tie_c259z != b->tie_c259z) break;
                    q++;
                }
                groups[ng].begin = p;
                groups[ng].end = q;
                groups[ng].z = a->tie_vz;
                ng++;
                p = q;
            }

            /* Stable insertion sort: larger (less-negative) source Z first. */
            for (u32 g = 1; g < ng; g++) {
                LumenGroup key = groups[g];
                u32 k = g;
                while (k > 0 && groups[k - 1].z < key.z) {
                    groups[k] = groups[k - 1];
                    k--;
                }
                groups[k] = key;
            }

            u32 out = seg;
            int changed = 0;
            for (u32 g = 0; g < ng; g++) {
                if (groups[g].begin != out) changed = 1;
                for (u32 p = groups[g].begin; p < groups[g].end; p++)
                    ordered[out++] = s_d3d.draws[p];
            }
            if (changed) {
                memcpy(&s_d3d.draws[seg], &ordered[seg],
                       (seg_end - seg) * sizeof(D3D12DrawRecord));
                { static int logged = 0; if (logged++ < 12)
                    fprintf(stderr, "[LUMENSORT] sorted %u groups in display run [%u..%u]\n",
                            ng, seg, seg_end - 1); }
            }
            seg = seg_end;
        }
    }

    if (profiling) QueryPerformanceCounter(&prof_t[2]);

    /* Render-to-texture pre-pass: make sure an offscreen RT resource exists for
     * every non-display surface targeted this frame (so draws binding it as a
     * texture can resolve to it below, whatever the op order). */
    for (int _i = 0; _i < MAX_OFF_RTS; _i++) s_d3d.off_rt[_i].used = 0;
    for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
        D3D12DrawRecord* dr = &s_d3d.draws[_d];
        if (dr->is_vp && dr->rt_off)
            off_rt_get(dr->rt_off, dr->rt_w, dr->rt_h, dr->rt_fmt);
        if (dr->is_vp && dr->rt_off2)
            off_rt_get(dr->rt_off2, dr->rt_w, dr->rt_h, dr->rt_fmt);
    }

    /* Per-frame VP textures + guest-FP pipelines: for each VP draw, upload the
     * texture it had bound at submit time into a slot (SRV heap 1+slot; plasma
     * animates so contents re-upload every frame) and pre-build its FP PSO.
     * A texture whose offset matches an offscreen RT samples the RT directly
     * (tex_slot 1000+idx) -- no guest-memory upload. */
    for (int _i = 0; _i < VP_TEX_SLOTS; _i++) {
        s_d3d.vp_tex[_i].used = 0;
        s_d3d.vp_tex[_i].uploaded = 0;
    }
    for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
        D3D12DrawRecord* dr = &s_d3d.draws[_d];
        if (!dr->is_vp || dr->is_clear) continue;
        /* Debug: RTT_VIEWRT=<hex raw offset> makes display draws sample that
         * offscreen RT at t0 (the composite blit then shows it fullscreen). */
        { const char* vr = getenv("RTT_VIEWRT");
          if (vr && dr->rt_off == 0) {
              dr->tex[0].raw = (u32)strtoul(vr, NULL, 16);
              dr->tex[0].off = 0;
              dr->tex[0].set = 1;
          } }
        /* Fill this draw's parity-owned t0-t3 SRV window: each unit resolves
         * to an offscreen RT (sampled directly), an uploaded guest texture, or
         * a null SRV.  The other parity can still be referenced by the GPU. */
        for (int _u = 0; _u < 4; _u++) {
            u32 wslot = DRAW_SRV_BASE
                      + (u32)s_d3d.vp_parity * DRAW_SRV_PARITY_STRIDE
                      + _d * 4 + (u32)_u;
            dr->tex_rt[_u] = -1;
            if (dr->tex[_u].set) {
                int rt = off_rt_find(dr->tex[_u].raw);
                if (rt >= 0) {
                    dr->tex_rt[_u] = rt;
                    srv_write(wslot, s_d3d.off_rt[rt].res, (DXGI_FORMAT)s_d3d.off_rt[rt].dxgi,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    continue;
                }
                if (dr->tex[_u].off &&
                    ((dr->tex[_u].fmt & 0x9F) == 0x81 ||
                     (dr->tex[_u].fmt & 0x9F) == 0x85 ||
                     (dr->tex[_u].fmt & 0x9F) == 0x86 ||
                     (dr->tex[_u].fmt & 0x9F) == 0x87 ||
                     (dr->tex[_u].fmt & 0x9F) == 0x88 ||
                     (dr->tex[_u].fmt & 0x9F) == 0x9E)) {
                    int ts = vp_upload_tex_slot(dr->tex[_u].off, dr->tex[_u].w,
                                                dr->tex[_u].h, dr->tex[_u].fmt,
                                                dr->tex[_u].pitch);
                    if (ts >= 0) {
                        u32 tf = s_d3d.vp_tex[ts].fmt & 0x9F;
                        int argb = (tf == 0x85 || tf == 0x9E);
                        int bc1  = (tf == 0x86);
                        int bc2  = (tf == 0x87);
                        int bc3  = (tf == 0x88);
                        u32 component_mapping =
                            (tf == 0x85 || tf == 0x9E)
                                ? vp_texture_component_mapping(dr->tex[_u].control1)
                                : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        srv_write(wslot, s_d3d.vp_tex[ts].res,
                                  bc1 ? DXGI_FORMAT_BC1_UNORM :
                                  (bc2 ? DXGI_FORMAT_BC2_UNORM :
                                  (bc3 ? DXGI_FORMAT_BC3_UNORM :
                                   (argb ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8_UNORM))),
                                  (argb || bc1 || bc2 || bc3) ? component_mapping : 0x1000);
                        continue;
                    }
                }
            }
            srv_write(wslot, NULL, DXGI_FORMAT_R8G8B8A8_UNORM,
                      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
        }
        if (dr->fp_addr) vp_get_fp_pso(dr->vs_idx, dr->fp_addr,
                                       fp_snapshot_ucode(dr), dr->fp_size,
                                       dr->fp_hash, dr->blend,
                                       dr->blend_sf, dr->blend_df, dr->blend_eq,
                                       dr->depth_test, dr->depth_mask, dr->depth_func,
                                       dr->cull_enable, dr->cull_face, dr->front_face,
                                       dr->rt_off2 ? 2 : 1,
                                       dr->rt_off ? rsx_surface_dxgi(dr->rt_fmt)
                                                  : DXGI_FORMAT_R8G8B8A8_UNORM,
                                       dr->fp_exp32, dr->cmask);
    }
    if (profiling) QueryPerformanceCounter(&prof_t[3]);

    /* Transition render target to RENDER_TARGET state */
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = s_d3d.render_targets[fi];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);

    /* Get RTV handle for current frame */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &rtv_handle);
    rtv_handle.ptr += fi * s_d3d.rtv_descriptor_size;

    /* Get DSV handle (single depth buffer shared across frames) */
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    s_d3d.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.dsv_heap, &dsv_handle);

    /* Set render target + depth */
    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);

    /* Clear color and depth */
    /* CLEAR_DBG paints the backbuffer magenta, which separates "the guest
     * cleared to this colour" from "a draw painted this colour". */
    { static const float dbg[4] = {1.0f, 0.0f, 1.0f, 1.0f};
      static int clear_dbg = -1;
      if (clear_dbg < 0) clear_dbg = getenv("CLEAR_DBG") ? 1 : 0;
      s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(
          s_d3d.cmd_list, rtv_handle, clear_dbg ? dbg : s_d3d.clear_color, 0, NULL); }
    s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(
        s_d3d.cmd_list, dsv_handle,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f, 0, 0, NULL);

    /* Set viewport and scissor */
    D3D12_VIEWPORT viewport = {0, 0, (float)s_d3d.width, (float)s_d3d.height, 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, (LONG)s_d3d.width, (LONG)s_d3d.height};
    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &viewport);
    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &scissor);

    /* Bind pipeline state and push MVP if anything to draw */
    if (s_d3d.pipeline_ready && s_d3d.draw_count > 0) {
        s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list, s_d3d.root_signature);
        s_d3d.cmd_list->lpVtbl->IASetVertexBuffers(s_d3d.cmd_list, 0, 1, &s_d3d.vb_view);

        /* Make the atlas SRV heap current and bind its table (t0) for textured
         * draws. Safe to set even if no draw is textured. */
        if (s_d3d.tex_ready && s_d3d.srv_heap) {
            ID3D12DescriptorHeap* heaps[] = { s_d3d.srv_heap };
            s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list, 1, heaps);
            D3D12_GPU_DESCRIPTOR_HANDLE gh;
            s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
        }

        /* Push the MVP matrix from RSX vertex constants slots 0..3.
         * If the game hasn't written any constants (e.g. placeholder data
         * already in clip space), fall back to identity. */
        float mvp[16];
        const rsx_state* st = s_d3d.current_rsx_state;
        int have_mvp = 0;
        if (st) {
            for (u32 r = 0; r < 4; r++) {
                for (u32 c = 0; c < 4; c++) {
                    float v = st->vertex_constants[r][c];
                    mvp[r * 4 + c] = v;
                    if (v != 0.0f) have_mvp = 1;
                }
            }
        }
        if (!have_mvp) {
            memset(mvp, 0, sizeof(mvp));
            mvp[0] = mvp[5] = mvp[10] = mvp[15] = 1.0f; /* identity */
        }
        s_d3d.cmd_list->lpVtbl->SetGraphicsRoot32BitConstants(
            s_d3d.cmd_list, 0 /*root param 0*/, 16, mvp, 0);

        /* D3D12_IQ=1: dump exactly what the GPU is about to see -- the MVP root
         * constants and the first uploaded host vertices. The legacy path draws
         * with no validation errors yet produces zero pixels, so the geometry
         * must be degenerate/off-screen post-VS. */
        { static int _vd = -1;
          if (_vd < 0) { const char* e = getenv("D3D12_IQ"); _vd = e ? 1 : 0; }
          static int _n = 0;
          if (_vd && _n < 3) {
            _n++;
            fprintf(stderr, "[D3D12-DBG] have_mvp=%d draw_count=%u vb_offset=%u\n",
                    have_mvp, s_d3d.draw_count, s_d3d.vb_offset);
            fprintf(stderr, "[D3D12-DBG] mvp rows: [%.2f %.2f %.2f %.2f][%.2f %.2f %.2f %.2f]"
                            "[%.2f %.2f %.2f %.2f][%.2f %.2f %.2f %.2f]\n",
                    mvp[0],mvp[1],mvp[2],mvp[3], mvp[4],mvp[5],mvp[6],mvp[7],
                    mvp[8],mvp[9],mvp[10],mvp[11], mvp[12],mvp[13],mvp[14],mvp[15]);
            if (s_d3d.vb_mapped) {
                /* host vertex = 9 floats (pos3 + col4 + uv2), VERTEX_STRIDE=36 */
                const float* bv = (const float*)s_d3d.vb_mapped;
                for (int k = 0; k < 3; k++) {
                    const float* v = bv + k * 9;
                    fprintf(stderr, "[D3D12-DBG]  hostvert[%d] pos=(%.3f,%.3f,%.3f) col=(%.2f,%.2f,%.2f,%.2f)\n",
                            k, v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
                }
            }
            for (u32 d = 0; d < s_d3d.draw_count && d < 3; d++) {
                const D3D12DrawRecord* dr = &s_d3d.draws[d];
                fprintf(stderr, "[D3D12-DBG]  draw[%u] is_vp=%d is_clear=%d topo=%u cnt=%u vbofs=%u startv=%u rt=%u\n",
                        d, dr->is_vp, dr->is_clear, dr->topology, dr->vertex_count,
                        dr->vb_byte_offset, dr->vb_byte_offset / VERTEX_STRIDE, dr->rt_off);
            }
            fflush(stderr);
          } }

        /* Replay each recorded draw with its own primitive topology and
         * the matching PSO class (triangle / line / point). The PSO class
         * must match the topology or D3D12 rejects the draw. */
        u32 last_topo = 0xFFFFFFFFu;
        ID3D12PipelineState* last_pso = NULL;
        u32 draws = s_d3d.draw_count;
        if (draws > MAX_DRAWS) draws = MAX_DRAWS;
        for (u32 d = 0; d < draws; d++) {
            const D3D12DrawRecord* dr = &s_d3d.draws[d];
            if (dr->is_vp) continue; /* drawn by the VP pass below */

            /* Select PSO: textured triangles (dbgfont) use the atlas PSO;
             * otherwise pick by topology class. */
            ID3D12PipelineState* target_pso = s_d3d.pipeline_state; /* default triangle */
            if (dr->textured && s_d3d.tex_ready && s_d3d.pipeline_state_tex) {
                target_pso = s_d3d.pipeline_state_tex;
            } else if (dr->topology == D3D_TOPOLOGY_POINTLIST) {
                target_pso = s_d3d.pipeline_state_points
                             ? s_d3d.pipeline_state_points : s_d3d.pipeline_state;
            } else if (dr->topology == D3D_TOPOLOGY_LINELIST ||
                       dr->topology == D3D_TOPOLOGY_LINESTRIP) {
                target_pso = s_d3d.pipeline_state_lines
                             ? s_d3d.pipeline_state_lines : s_d3d.pipeline_state;
            }
            if (target_pso != last_pso) {
                s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list, target_pso);
                last_pso = target_pso;
            }
            if (dr->topology != last_topo) {
                s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list, dr->topology);
                last_topo = dr->topology;
            }
            u32 start_vert = dr->vb_byte_offset / VERTEX_STRIDE;
            s_d3d.cmd_list->lpVtbl->DrawInstanced(
                s_d3d.cmd_list, dr->vertex_count, 1, start_vert, 0);
        }
    }

    /* VP pass: real decompiled vertex program + atlas alpha-test PS. Feeds raw
     * float4 attrib0 from vp_vb and the vp_c[] constant bank. */
    if (s_d3d.vp_ready && s_d3d.draw_count > 0) {
        int any = 0;
        for (u32 d = 0; d < s_d3d.draw_count && d < MAX_DRAWS; d++)
            if (s_d3d.draws[d].is_vp) { any = 1; break; }
        /* Textured geometry (dbgfont atlas) uses the sampling PS; untextured 3D
         * (vkcube) uses the colour-only PS. Fall back to whichever exists. */
        ID3D12PipelineState* vpso =
            (s_d3d.tex_ready && s_d3d.pipeline_state_vp) ? s_d3d.pipeline_state_vp
                                                         : s_d3d.pipeline_state_vp_color;
        if (!vpso) vpso = s_d3d.pipeline_state_vp;
        if (any && vpso && s_d3d.sampler_heap) {
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list, s_d3d.vp_root_sig);
            s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list, vpso);
            s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list, D3D_TOPOLOGY_TRIANGLELIST);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 0,
                (vp_gpu_snapshot ? s_d3d.vp_cb_gpu : s_d3d.vp_cb)->lpVtbl->GetGPUVirtualAddress(
                    vp_gpu_snapshot ? s_d3d.vp_cb_gpu : s_d3d.vp_cb));
            ID3D12DescriptorHeap* heaps[] = { s_d3d.srv_heap, s_d3d.sampler_heap };
            s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list, 2, heaps);
            D3D12_GPU_DESCRIPTOR_HANDLE gh;
            s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
            D3D12_VERTEX_BUFFER_VIEW vbv;
            ID3D12Resource* draw_vb = vp_gpu_snapshot ? s_d3d.vp_vb_gpu : s_d3d.vp_vb;
            vbv.BufferLocation = draw_vb->lpVtbl->GetGPUVirtualAddress(draw_vb)
                               + (u64)s_d3d.vp_parity * MAX_VERTICES * 256;
            vbv.SizeInBytes    = MAX_VERTICES * 256;
            vbv.StrideInBytes  = 256;   /* 16 float4 attrib slots */
            s_d3d.cmd_list->lpVtbl->IASetVertexBuffers(s_d3d.cmd_list, 0, 1, &vbv);
            D3D12_GPU_DESCRIPTOR_HANDLE gh_base;
            s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh_base);
            D3D12_GPU_DESCRIPTOR_HANDLE sampler_base;
            s_d3d.sampler_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
                s_d3d.sampler_heap, &sampler_base);
            int cur_rt = -1, cur_rt2 = -1;   /* colour targets: -1 = backbuffer/none */
            int dbg_body_seen = 0;
            for (u32 d = 0; d < s_d3d.draw_count && d < MAX_DRAWS; d++) {
                const D3D12DrawRecord* dr = &s_d3d.draws[d];
                if (!dr->is_vp) continue;
                /* Render-to-texture: retarget when this op's surfaces differ.
                 * Depth is a single shared buffer, so clear it per switch. */
                const int dbg_body_draw =
                    ((dr->fp_addr & ~3u) == 0x01AA8E40u) && s_dbg_body_direct_rt;
                int want  = (!dbg_body_draw && dr->rt_off)
                    ? off_rt_find(dr->rt_off) : -1;
                int want2 = dr->rt_off2 ? off_rt_find(dr->rt_off2) : -1;
                if (want != cur_rt || want2 != cur_rt2) {
                    D3D12_CPU_DESCRIPTOR_HANDLE rh[2];
                    UINT nrt = 1;
                    rh[0] = rtv_handle;
                    D3D12_VIEWPORT vp = {0, 0, (float)s_d3d.width, (float)s_d3d.height, 0.0f, 1.0f};
                    if (want >= 0) {
                        off_rt_transition(want, D3D12_RESOURCE_STATE_RENDER_TARGET);
                        rh[0] = off_rt_rtv(want);
                        vp.Width  = (float)s_d3d.off_rt[want].w;
                        vp.Height = (float)s_d3d.off_rt[want].h;
                    }
                    if (want2 >= 0) {
                        off_rt_transition(want2, D3D12_RESOURCE_STATE_RENDER_TARGET);
                        rh[1] = off_rt_rtv(want2);
                        nrt = 2;
                    }
                    D3D12_RECT sc = {0, 0, (LONG)vp.Width, (LONG)vp.Height};
                    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, nrt, rh, FALSE, &dsv_handle);
                    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &vp);
                    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &sc);
                    s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, dsv_handle,
                        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
                    cur_rt = want; cur_rt2 = want2;
                    dbg_body_seen = 0;
                }
                if (dr->is_clear) {
                    D3D12_CPU_DESCRIPTOR_HANDLE rh =
                        (cur_rt >= 0) ? off_rt_rtv(cur_rt) : rtv_handle;
                    s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rh, dr->cc, 0, NULL);
                    if (cur_rt2 >= 0) {
                        D3D12_CPU_DESCRIPTOR_HANDLE rh2 = off_rt_rtv(cur_rt2);
                        s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rh2, dr->cc, 0, NULL);
                    }
                    continue;
                }
                /* F3 checkpoint: preserve the normal offscreen body writes and
                 * the normal downstream composite, but stop modifying this RT
                 * once Don-chan's two body batches have completed. */
                if (s_dbg_body_isolate_offrt && cur_rt >= 0) {
                    const int is_body =
                        ((dr->fp_addr & ~3u) == 0x01AA8E40u);
                    if (dbg_body_seen && !is_body)
                        continue;
                    if (is_body)
                        dbg_body_seen = 1;
                }
                /* Per-draw pipeline: prefer the guest's own compiled FP; fall
                 * back to the hardcoded atlas/colour PS pair. */
                ID3D12PipelineState* dpso =
                    dr->fp_addr ? vp_get_fp_pso(dr->vs_idx, dr->fp_addr,
                                                fp_snapshot_ucode(dr), dr->fp_size,
                                                dr->fp_hash, dr->blend,
                                                dr->blend_sf, dr->blend_df, dr->blend_eq,
                                                dr->depth_test, dr->depth_mask, dr->depth_func,
                                                dr->cull_enable, dr->cull_face, dr->front_face,
                                                dr->rt_off2 ? 2 : 1,
                                                dr->rt_off ? rsx_surface_dxgi(dr->rt_fmt)
                                                           : DXGI_FORMAT_R8G8B8A8_UNORM,
                                                dr->fp_exp32, dr->cmask) : NULL;
                s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list,
                                                         dpso ? dpso : vpso);
                {
                    float bf[4] = {
                        (float)((dr->blend_color >> 16) & 0xFFu) / 255.0f,
                        (float)((dr->blend_color >> 8)  & 0xFFu) / 255.0f,
                        (float)( dr->blend_color        & 0xFFu) / 255.0f,
                        (float)((dr->blend_color >> 24) & 0xFFu) / 255.0f
                    };
                    s_d3d.cmd_list->lpVtbl->OMSetBlendFactor(s_d3d.cmd_list, bf);
                }
                /* Per-draw viewport: the guest rect when sane, else the
                 * full target. Scissor tracks the same rect. */
                {
                    float tw = (cur_rt >= 0) ? (float)s_d3d.off_rt[cur_rt].w : (float)s_d3d.width;
                    float th = (cur_rt >= 0) ? (float)s_d3d.off_rt[cur_rt].h : (float)s_d3d.height;
                    D3D12_VIEWPORT dvp = {0, 0, tw, th, 0.0f, 1.0f};
                    if (dr->vp_w >= 2 && dr->vp_h >= 2 &&
                        (float)(dr->vp_x + dr->vp_w) <= tw + 0.5f &&
                        (float)(dr->vp_y + dr->vp_h) <= th + 0.5f) {
                        dvp.TopLeftX = (float)dr->vp_x;
                        dvp.TopLeftY = (float)dr->vp_y;
                        dvp.Width    = (float)dr->vp_w;
                        dvp.Height   = (float)dr->vp_h;
                    }
                    /* Scissor is surface-relative RSX state, not implicitly
                     * the viewport. Lumen uses it for nested 2D layers. */
                    LONG sl = 0, st = 0, sr = (LONG)tw, sb = (LONG)th;
                    if (dr->sc_w && dr->sc_h) {
                        u64 rr = (u64)dr->sc_x + dr->sc_w;
                        u64 bb = (u64)dr->sc_y + dr->sc_h;
                        sl = (LONG)(dr->sc_x < (u32)tw ? dr->sc_x : (u32)tw);
                        st = (LONG)(dr->sc_y < (u32)th ? dr->sc_y : (u32)th);
                        sr = (LONG)(rr < (u64)tw ? rr : (u64)tw);
                        sb = (LONG)(bb < (u64)th ? bb : (u64)th);
                        if (sr < sl) sr = sl;
                        if (sb < st) sb = st;
                    }
                    D3D12_RECT dsc = {sl, st, sr, sb};
                    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &dvp);
                    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &dsc);
                }
                /* Per-draw textures: bind this draw's t0-t3 SRV window.
                 * Any sampled offscreen RT transitions to PSR first (never
                 * one of the currently-bound colour targets). */
                for (int _u = 0; _u < 4; _u++) {
                    int rt = dr->tex_rt[_u];
                    if (rt >= 0 && rt != cur_rt && rt != cur_rt2)
                        off_rt_transition(rt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                }
                D3D12_GPU_DESCRIPTOR_HANDLE gh = gh_base;
                gh.ptr += (u64)(DRAW_SRV_BASE
                              + (u32)s_d3d.vp_parity * DRAW_SRV_PARITY_STRIDE
                              + d * 4) * s_d3d.srv_inc;
                s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
                int sampler_set = vp_sampler_set_for_draw(dr);
                if (sampler_set >= 0) {
                    D3D12_GPU_DESCRIPTOR_HANDLE sh = sampler_base;
                    sh.ptr += (u64)sampler_set * 4 * s_d3d.sampler_inc;
                    s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(
                        s_d3d.cmd_list, 3, sh);
                }
                /* Per-draw constants: this draw's vp_cb + FP texscale slots. */
                s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 0,
                    (vp_gpu_snapshot ? s_d3d.vp_cb_gpu : s_d3d.vp_cb)->lpVtbl->GetGPUVirtualAddress(
                        vp_gpu_snapshot ? s_d3d.vp_cb_gpu : s_d3d.vp_cb)
                    + ((u64)s_d3d.vp_parity * MAX_DRAWS + dr->cb_slot) * VP_CB_STRIDE);
                if (s_d3d.vp_fpcb)
                    s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 2,
                        (vp_gpu_snapshot ? s_d3d.vp_fpcb_gpu : s_d3d.vp_fpcb)->lpVtbl->GetGPUVirtualAddress(
                            vp_gpu_snapshot ? s_d3d.vp_fpcb_gpu : s_d3d.vp_fpcb)
                        + ((u64)s_d3d.vp_parity * MAX_DRAWS + dr->cb_slot) * 256);
                /* Draw records may be triangle lists or strips.  Leaving the
                 * IA hardcoded to TRIANGLELIST made every four-vertex Lumen
                 * sprite consume only its first three vertices, producing the
                 * characteristic diagonal half-quad. */
                s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(
                    s_d3d.cmd_list, (D3D12_PRIMITIVE_TOPOLOGY)dr->topology);
                s_d3d.cmd_list->lpVtbl->DrawInstanced(s_d3d.cmd_list,
                    dr->vertex_count, 1, dr->vb_byte_offset / 256, 0);
            }
            /* Leave the backbuffer bound for the dump/present epilogue. */
            if (cur_rt >= 0) {
                s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);
                D3D12_VIEWPORT vp = {0, 0, (float)s_d3d.width, (float)s_d3d.height, 0.0f, 1.0f};
                D3D12_RECT sc = {0, 0, (LONG)s_d3d.width, (LONG)s_d3d.height};
                s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &vp);
                s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &sc);
            }
        }
    }

    s_dbg_last_draws = s_d3d.draw_count;
    s_d3d.vb_offset  = 0; /* reset for next frame */
    s_d3d.vp_vb_offset = 0;
    s_d3d.draw_count = 0;
    s_d3d.vp_parity = (s_d3d.vp_parity + 1) % UPLOAD_FRAME_COUNT;
    s_batch_has_display_clear = 0;  /* batch consumed */
    s_spurs_drained_for_batch = 0;
    s_upload_heaps_safe_for_batch = 0;

    /* Debug: RTT_SAVERT=<hex raw offset>[:frame] copies that offscreen RT
     * into a readback buffer this frame and writes rt_save.bmp (half-float
     * RTs are tonemapped |v| -> byte). */
    static ID3D12Resource* s_rtsave_buf = NULL;
    static u32 s_rtsave_state = 0;   /* 1 = copy queued this frame */
    static u32 s_rtsave_w, s_rtsave_h, s_rtsave_pitch, s_rtsave_dxgi;
    { const char* sv = getenv("RTT_SAVERT");
      static int _done = 0;
      /* When F9 tracing is active, capture the requested RT at the broken
       * screen rather than consuming the one-shot during an earlier boot
       * scene which happens to reuse the same local-memory offset. */
      if (sv && !_done && s_rtt_hotkey_frames > 0) {
        int rt = off_rt_find((u32)strtoul(sv, NULL, 16));
        if (rt >= 0 && s_d3d.off_rt[rt].res) {
            OffRT* r = &s_d3d.off_rt[rt];
            u32 bpp = (r->dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 :
                      (r->dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16 : 4;
            u32 pitch = (r->w * bpp + 255) & ~255u;
            if (!s_rtsave_buf) {
                D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC rd = {0};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Width = (u64)pitch * r->h; rd.Height = 1; rd.DepthOrArraySize = 1;
                rd.MipLevels = 1; rd.SampleDesc.Count = 1;
                rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                    &IID_ID3D12Resource, (void**)&s_rtsave_buf);
            }
            if (s_rtsave_buf) {
                off_rt_transition(rt, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION cdst = {0}, csrc = {0};
                cdst.pResource = s_rtsave_buf;
                cdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                cdst.PlacedFootprint.Footprint.Format   = (DXGI_FORMAT)r->dxgi;
                cdst.PlacedFootprint.Footprint.Width    = r->w;
                cdst.PlacedFootprint.Footprint.Height   = r->h;
                cdst.PlacedFootprint.Footprint.Depth    = 1;
                cdst.PlacedFootprint.Footprint.RowPitch = pitch;
                csrc.pResource = r->res;
                csrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cdst, 0, 0, 0, &csrc, NULL);
                off_rt_transition(rt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                s_rtsave_state = 1;
                s_rtsave_w = r->w; s_rtsave_h = r->h;
                s_rtsave_pitch = pitch; s_rtsave_dxgi = r->dxgi;
                _done = 1;
            }
        }
      } }

    /* CELLMARK_DUMP_CONTENT=1 keeps the finite capture budget from being
     * consumed by the boot-time empty presents.  s_dbg_last_draws was saved
     * immediately above, before the per-frame draw list was reset. */
    int dump_has_content = !getenv("CELLMARK_DUMP_CONTENT") || s_dbg_last_draws > 0;
    int dumping = (s_d3d.dump_frames_left > 0 && s_d3d.readback_buf &&
                   dump_has_content);
    if (dumping) {
        /* RT -> COPY_SOURCE, copy into the readback buffer, then -> PRESENT. */
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = s_d3d.readback_buf;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width    = s_d3d.width;
        dst.PlacedFootprint.Footprint.Height   = s_d3d.height;
        dst.PlacedFootprint.Footprint.Depth    = 1;
        dst.PlacedFootprint.Footprint.RowPitch = s_d3d.readback_pitch;
        src.pResource = s_d3d.render_targets[fi];
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);
    } else {
        /* Transition render target to PRESENT state */
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);
    }

    if (vp_gpu_snapshot) {
        D3D12_RESOURCE_BARRIER bb[3] = {0};
        ID3D12Resource* rr[3] = {
            s_d3d.vp_vb_gpu, s_d3d.vp_cb_gpu, s_d3d.vp_fpcb_gpu
        };
        for (int i = 0; i < 3; i++) {
            bb[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bb[i].Transition.pResource = rr[i];
            bb[i].Transition.StateBefore = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            bb[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            bb[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 3, bb);
    }

    if (profiling) QueryPerformanceCounter(&prof_t[4]);

    /* Close and execute */
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);
    ID3D12CommandList* cmd_lists[] = {(ID3D12CommandList*)s_d3d.cmd_list};
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, cmd_lists);

    /* D3D12_IQ=1: drain the debug layer's message queue after submitting the
     * frame. The legacy (is_vp=0) DrawInstanced path silently produced ZERO
     * pixels in flOw's render injection while ClearRenderTargetView worked, and
     * every input (verts, offsets, layout, MVP, PSO, root sig, depth, cull) was
     * verified correct -- so the only remaining explanation is a validation
     * error the debug layer is swallowing. Off unless the env var is set. */
    { static int _iq = -1;
      if (_iq < 0) { const char* e = getenv("D3D12_IQ"); _iq = e ? 1 : 0; }
      if (_iq && s_d3d.device) {
        ID3D12InfoQueue* iq = NULL;
        if (SUCCEEDED(s_d3d.device->lpVtbl->QueryInterface(
                s_d3d.device, &IID_ID3D12InfoQueue, (void**)&iq)) && iq) {
            UINT64 n = iq->lpVtbl->GetNumStoredMessages(iq);
            static int _printed = 0;
            for (UINT64 mi = 0; mi < n && _printed < 80; mi++) {
                SIZE_T len = 0;
                iq->lpVtbl->GetMessage(iq, mi, NULL, &len);
                D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
                if (m && SUCCEEDED(iq->lpVtbl->GetMessage(iq, mi, m, &len))) {
                    fprintf(stderr, "[D3D12-IQ][sev=%d id=%d] %s\n",
                            (int)m->Severity, (int)m->ID, m->pDescription);
                    _printed++;
                }
                free(m);
            }
            if (n) iq->lpVtbl->ClearStoredMessages(iq);
            iq->lpVtbl->Release(iq);
            fflush(stderr);
        }
      } }

    if (dumping) {
        wait_for_gpu();            /* ensure the copy finished before mapping */
        dump_backbuffer_bmp();
        s_d3d.dump_frames_left--;
    }

    if (s_rtsave_state) {
        if (!dumping) wait_for_gpu();
        void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)s_rtsave_pitch * s_rtsave_h};
        if (SUCCEEDED(s_rtsave_buf->lpVtbl->Map(s_rtsave_buf, 0, &rr, &mp)) && mp) {
            FILE* f = fopen("rt_save.bmp", "wb");
            if (f) {
                u32 w = s_rtsave_w, h = s_rtsave_h;
                u32 rowb = (w * 3 + 3) & ~3u;
                u32 datasz = rowb * h;
                u8 hdr[54] = {0};
                hdr[0]='B'; hdr[1]='M';
                *(u32*)(hdr+2) = 54 + datasz; *(u32*)(hdr+10) = 54;
                *(u32*)(hdr+14) = 40; *(int*)(hdr+18) = (int)w; *(int*)(hdr+22) = (int)h;
                *(u16*)(hdr+26) = 1; *(u16*)(hdr+28) = 24; *(u32*)(hdr+34) = datasz;
                fwrite(hdr, 1, 54, f);
                u8* line = (u8*)malloc(rowb);
                for (int y = (int)h - 1; y >= 0; y--) {
                    const u8* srow = (const u8*)mp + (u64)y * s_rtsave_pitch;
                    memset(line, 0, rowb);
                    for (u32 x = 0; x < w; x++) {
                        float rv, gv, bv;
                        if (s_rtsave_dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                            const u16* hp16 = (const u16*)(srow + (u64)x * 8);
                            /* crude half->float: sign|exp|mant */
                            float v[3];
                            for (int c2 = 0; c2 < 3; c2++) {
                                u16 hv = hp16[c2];
                                u32 sign = (hv >> 15) & 1, exp = (hv >> 10) & 0x1F, man = hv & 0x3FF;
                                float fv;
                                if (exp == 0) fv = (float)man / 16777216.0f;
                                else { u32 fb = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
                                       memcpy(&fv, &fb, 4); }
                                v[c2] = fv;
                            }
                            rv = v[0]; gv = v[1]; bv = v[2];
                        } else if (s_rtsave_dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                            const float* fp32 = (const float*)(srow + (u64)x * 16);
                            rv = fp32[0]; gv = fp32[1]; bv = fp32[2];
                        } else {
                            const u8* p8 = srow + (u64)x * 4;
                            rv = p8[0] / 255.0f; gv = p8[1] / 255.0f; bv = p8[2] / 255.0f;
                        }
                        /* RTT_SAVEA=1: show the alpha lane in RED (gates
                         * like wave's mask.w live there). */
                        if (getenv("RTT_SAVEA")) {
                            float av;
                            if (s_rtsave_dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                                const u16* hp16 = (const u16*)(srow + (u64)x * 8);
                                u16 hv = hp16[3];
                                u32 sign = (hv >> 15) & 1, exp = (hv >> 10) & 0x1F, man = hv & 0x3FF;
                                if (exp == 0) av = (float)man / 16777216.0f;
                                else { u32 fb = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
                                       memcpy(&av, &fb, 4); }
                            } else if (s_rtsave_dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                                av = ((const float*)(srow + (u64)x * 16))[3];
                            } else {
                                av = (srow + (u64)x * 4)[3] / 255.0f;
                            }
                            rv = av;
                        }
                        /* |v| tonemap so signed heights are visible */
                        float ar = rv < 0 ? -rv : rv, ag = gv < 0 ? -gv : gv, ab = bv < 0 ? -bv : bv;
                        if (ar > 1) ar = 1; if (ag > 1) ag = 1; if (ab > 1) ab = 1;
                        line[x*3+0] = (u8)(ab * 255.0f);
                        line[x*3+1] = (u8)(ag * 255.0f);
                        line[x*3+2] = (u8)(ar * 255.0f);
                    }
                    fwrite(line, 1, rowb, f);
                }
                free(line);
                fclose(f);
                printf("[D3D12] wrote rt_save.bmp (%ux%u dxgi=%u)\n", w, h, s_rtsave_dxgi);
            }
            s_rtsave_buf->lpVtbl->Unmap(s_rtsave_buf, 0, NULL);
        }
        s_rtsave_state = 0;
    }

    if (profiling) QueryPerformanceCounter(&prof_t[5]);

    /* The synthetic vblank thread already clocks the guest.  SyncInterval=1
     * made vkd3d/Wine block this same thread for 40-60 ms in bursts, slowing
     * animation and beatmaps even though GPU work took only a few ms.  Present
     * immediately and let the desktop compositor pace scanout. */
    static int host_vsync = -1;
    if (host_vsync < 0) {
        const char* e = getenv("RSX_PRESENT_VSYNC");
        host_vsync = (e && e[0] != '0') ? 1 : 0;
    }
    const UINT present_flags = host_vsync ? 0 : DXGI_PRESENT_DO_NOT_WAIT;
    const HRESULT present_hr = s_d3d.swap_chain->lpVtbl->Present(
        s_d3d.swap_chain, host_vsync ? 1 : 0, present_flags);
    if (FAILED(present_hr) && present_hr != DXGI_ERROR_WAS_STILL_DRAWING) {
        static int present_errors;
        if (present_errors++ < 8)
            fprintf(stderr, "[D3D12] Present failed (0x%08lX)\n", present_hr);
    }
    if (profiling) QueryPerformanceCounter(&prof_t[6]);

    const u64 submitted_fence = move_to_next_frame();
    s_upload_parity_fence[submitted_upload_parity] = submitted_fence;
    for (int i = 0; i < VP_TEX_SLOTS; ++i)
        if (s_d3d.vp_tex[i].uploaded)
            s_d3d.vp_tex[i].upload_fence = submitted_fence;
    s_dbg_present_serial++;
    debug_frame_step_wait();
    if (profiling) {
        QueryPerformanceCounter(&prof_t[7]);
        rsx_profile_frame(prof_draws, prof_t);
    }

    s_d3d.frame_count++;

    /* RPCS3-style titlebar stats, refreshed once a second: presented FPS,
     * draw count of the last frame, and the backbuffer size. */
    {
        static ULONGLONG s_tt0 = 0;
        static u64 s_tframes = 0;
        s_tframes++;
        ULONGLONG tnow = GetTickCount64();
        if (s_tt0 == 0) s_tt0 = tnow;
        if (tnow - s_tt0 >= 1000 && s_d3d.hwnd) {
            extern char g_rsx_title_base[128];
            const double title_fps =
                s_tframes * 1000.0 / (double)(tnow - s_tt0);
            char* title_update = (char*)malloc(256);
            if (title_update) {
                snprintf(title_update, 256,
                         "%s | FPS: %.2f | draws: %u | %ux%u",
                         g_rsx_title_base, title_fps,
                         s_dbg_last_draws, s_d3d.width, s_d3d.height);
                if (!PostMessageA(s_d3d.hwnd, WM_RSX_UPDATE_TITLE, 0,
                                  (LPARAM)title_update))
                    free(title_update);
            }
            {
                static int fps_log = -1;
                if (fps_log < 0) {
                    const char* e = getenv("RSX_FPS_LOG");
                    fps_log = (e && e[0] != '0') ? 1 : 0;
                }
                if (fps_log)
                    fprintf(stderr, "[RSXFPS] fps=%.2f draws=%u\n",
                            title_fps, s_dbg_last_draws);
            }
            s_tframes = 0;
            s_tt0 = tnow;
        }
    }
}

/* ---------------------------------------------------------------------------
 * RSX backend callbacks
 * -----------------------------------------------------------------------*/

static int d3d12_init(void* ud, u32 width, u32 height)
{
    (void)ud;
    printf("[D3D12] Backend init(%ux%u)\n", width, height);
    return 0;
}

static void d3d12_shutdown(void* ud)
{
    (void)ud;
    printf("[D3D12] Backend shutdown\n");
}

static void d3d12_begin_frame(void* ud)
{
    (void)ud;
}

static void d3d12_end_frame(void* ud)
{
    (void)ud;
}

static u32 s_dbg_clears_since_present = 0;   /* CELLMARK_BLINKDBG */
static u32 s_clear_presents = 0;   /* presents issued at clear (frame boundary) */

static int blink_dbg(void)
{
    static int v = -1;
    if (v < 0) v = getenv("CELLMARK_BLINKDBG") ? 1 : 0;
    return v;
}

static void d3d12_present(void* ud, u32 buffer_id)
{
    (void)ud;
    (void)buffer_id;

    /* Ring-layout probe: the flip is RenderDoc's frame boundary. Compare with
     * the [RING] display-clear line to see whether our batch is cut elsewhere. */
    if (getenv("RTT_DUMP") || s_rtt_hotkey_frames > 0) {
        extern u32 cellGcm_current_fifo_getoff(void);
        fprintf(stderr, "[RING] FLIP buf=%u fifo=0x%06X seq=%u draws_pending=%u\n",
                buffer_id, cellGcm_current_fifo_getoff(), s_draw_seq, s_d3d.draw_count);
    }

    if (blink_dbg())
        printf("[PRESENT] draws=%u clears_since_last=%u\n",
               s_d3d.draw_count, s_dbg_clears_since_present);
    s_dbg_clears_since_present = 0;

    /* If this batch opened with a display clear, that clear is the frame
     * boundary and the NEXT one presents the completed frame; a flip now is
     * mid-batch and would show a partial frame (Taiko credits flickered
     * text/gradient/icon as alternating one-layer presents). Escape hatch:
     * several flips with no new clear means the title stopped clearing the
     * display (mode change) -- hand the boundary back to the flip. */
    if (s_batch_has_display_clear) {
        if (++s_gated_flips <= 3)
            return;
        s_batch_has_display_clear = 0;
        s_gated_flips = 0;
    }

    /* An accumulated batch with draws but NONE targeting a display buffer is
     * offscreen pass work only (demosaic flips once per effect pass): showing
     * it would strobe the bare backbuffer clear. Keep accumulating -- the
     * composite draw that targets the display presents the whole chain, in
     * order, in one command list. Empty batches still present (boot/idle). */
    int flip_has_display = (s_d3d.draw_count == 0);
    for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++)
        if (!s_d3d.draws[_i].is_clear && s_d3d.draws[_i].rt_off == 0) {
            flip_has_display = 1;
            break;
        }

    if (s_d3d.initialized && flip_has_display)
        render_frame();

    /* FPS tracking */
    ULONGLONG now = GetTickCount64();
    if (now - s_d3d.last_fps_time >= 1000) {
        s_d3d.fps = (u32)s_d3d.frame_count; /* rough estimate */
        s_d3d.last_fps_time = now;
        s_d3d.frame_count = 0;
    }
}

static void d3d12_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud;
    (void)flags;
    (void)depth;
    (void)stencil;

    /* Convert RSX ARGB u32 to float[4] RGBA */
    float cc[4];
    cc[0] = ((color >> 16) & 0xFF) / 255.0f; /* R */
    cc[1] = ((color >> 8) & 0xFF) / 255.0f;  /* G */
    cc[2] = (color & 0xFF) / 255.0f;          /* B */
    cc[3] = ((color >> 24) & 0xFF) / 255.0f;  /* A */

    u32 rt_w = 0, rt_h = 0, rt2 = 0;
    u32 rt = current_rt_off(&rt_w, &rt_h, &rt2);

    /* An OFFSCREEN clear is just an ordered op in the current frame's pass
     * chain (demosaic clears each effect pass's surface) -- record it, don't
     * touch the frame boundary. */
    if (rt != 0) {
        if (s_d3d.draw_count < MAX_DRAWS) {
            D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count++];
            memset(dr, 0, sizeof(*dr));
            dr->is_vp = 1; dr->is_clear = 1; dr->tex_slot = -1;
            dr->rt_off = rt; dr->rt_off2 = rt2; dr->rt_w = rt_w; dr->rt_h = rt_h;
            dr->rt_raw = current_surface_raw();
            { extern u32 cellGcm_current_fifo_getoff(void);
              dr->fifo_off = cellGcm_current_fifo_getoff(); }
        { extern u32 cellGcm_current_fifo_getoff(void);
          dr->fifo_off = cellGcm_current_fifo_getoff();
          dr->seq = s_draw_seq++; }
            dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
            memcpy(dr->cc, cc, sizeof(cc));
        }
        return;
    }

    memcpy(s_d3d.clear_color, cc, sizeof(cc));

    /* Ring-layout probe: RenderDoc frames are flip-to-flip but our batch is
     * clear-to-clear. If those boundaries differ the batch straddles a frame
     * and picks up a neighbouring quad. Log where the clear sits in the ring. */
    if (getenv("RTT_DUMP") || s_rtt_hotkey_frames > 0) {
        extern u32 cellGcm_current_fifo_getoff(void);
        fprintf(stderr,
                "[RING] display clear fifo=0x%06X seq=%u draws_pending=%u "
                "cc=(%.3f,%.3f,%.3f,%.3f)\n",
                cellGcm_current_fifo_getoff(), s_draw_seq, s_d3d.draw_count,
                cc[0], cc[1], cc[2], cc[3]);
    }

    /* A DISPLAY clear marks the start of a new visible frame. If a completed
     * frame is still accumulated (the drain gulped across a frame boundary --
     * guaranteed at the FIFO ring wrap, where rest-of-frame-N + clear-N+1
     * arrive in one batch), PRESENT it now instead of discarding it. Only a
     * batch that actually contains DISPLAY draws is a completed frame: a
     * batch of offscreen pass work (render-to-texture) must keep accumulating
     * until its composite draw arrives, or the screen strobes intermediates. */
    int have_display_draws = 0;
    for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS; i++)
        if (!s_d3d.draws[i].is_clear && s_d3d.draws[i].rt_off == 0) {
            have_display_draws = 1;
            break;
        }
    if (!have_display_draws) {
        /* Keep accumulating the in-progress frame, but this clear still owns
         * the frame boundary: flips before the batch executes are mid-frame. */
        s_batch_has_display_clear = 1;
        s_gated_flips = 0;
        return;
    }

    if (s_d3d.initialized) {
        if (blink_dbg())
            printf("[CLEAR] presenting %u accumulated draws at frame boundary\n",
                   s_d3d.draw_count);
        render_frame();
        s_clear_presents++;
    }
    s_dbg_clears_since_present++;
    s_d3d.draw_count   = 0;
    s_d3d.vb_offset    = 0;
    s_d3d.vp_vb_offset = 0;
    /* The batch now accumulating (frame N+1, opened by this clear) must not
     * be flip-presented partially; the next display clear presents it. */
    s_batch_has_display_clear = 1;
    s_gated_flips = 0;
}

static void d3d12_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;
    /* Log only the first few; set_render_target is called every frame and
     * floods the log otherwise. */
    static int s_count = 0;
    if (s_count < 5) {
        printf("[D3D12] set_render_target(%ux%u)\n",
               state->surface_clip_w, state->surface_clip_h);
        s_count++;
    }
}

static void d3d12_set_viewport(void* ud, const rsx_state* state)
{
    (void)ud;
    /* TODO: update D3D12 viewport from RSX state */
    (void)state;
}

/* Our host vertex layout for the fallback path: position (xyz) + color (rgba)
 * + texcoord (uv), 36 bytes. (The real VP path feeds raw float4 attrib0.) */
typedef struct { float x, y, z; float r, g, b, a; float u, v; } BasicVertex;

/* Read a big-endian 32-bit float from guest memory. */
static float rd_bef(const u8* src)
{
    u32 w;
    memcpy(&w, src, 4);
    w = ((w>>24)&0xFF)|((w>>8)&0xFF00)|((w<<8)&0xFF0000)|((w<<24)&0xFF000000);
    float f; memcpy(&f, &w, 4); return f;
}

/* NV4097_SET_VERTEX_DATA_ARRAY_OFFSET encodes the memory location in bit 31:
 * 0 = RSX local memory, 1 = IO-mapped main memory. The remaining 31 bits are
 * the offset in that address space. Passing the encoded word directly to the
 * generic offset resolver made a main-memory array such as Taiko's
 * 0x804215C0 look like a huge local-memory offset, so every fetched position
 * and UV was zero. Keep the location bit separate, matching RSX hardware. */
static u32 resolve_vertex_ea(const rsx_state* state, u32 encoded_offset, u32 byte_offset)
{
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    int local = (encoded_offset >> 31) == 0;
    /* RSX adds SET_VERTEX_DATA_BASE_OFFSET to the attribute's relative
     * offset and masks the result to its 28-bit address space before applying
     * the per-vertex stride. Taiko advances this base through a large dynamic
     * UI arena; ignoring it reads command/object data as float positions. */
    u32 base = state ? state->vertex_data_base_offset : 0;
    u32 offset = ((base + (encoded_offset & 0x7FFFFFFFu)) & 0x0FFFFFFFu)
               + byte_offset;
    return cellGcmResolveLocated(local, offset);
}

/* Read one RSX vertex (by absolute vertex index) from guest memory into our
 * host layout. Position is attrib 0 (float3+), color is attrib 3 (ubyte4 or
 * float4); missing attribs default to opaque white. RSX stores each 32-bit
 * component big-endian, so every lane is byte-swapped. */
static void read_rsx_vertex(const rsx_state* state, u32 vindex, BasicVertex* out)
{
    extern uint8_t* vm_base;

    out->x = out->y = out->z = 0.0f;
    out->r = out->g = out->b = out->a = 1.0f;
    out->u = out->v = 0.0f;
    if (!state || !vm_base) return;

    const rsx_vertex_attrib* pos = &state->vertex_attribs[0];
    if (pos->enabled && pos->type == 2 /* float */ && pos->size >= 2) {
        u8* src = vm_base + resolve_vertex_ea(state, pos->offset, vindex * pos->stride);
        out->x = rd_bef(src);
        out->y = rd_bef(src + 4);
        if (pos->size >= 3) out->z = rd_bef(src + 8);

        /* dbgfont (and similar 2D overlays) store screen positions biased far
         * outside clip space; their vertex program folds them back to clip
         * space using transform constants we don't execute. When a position is
         * well outside NDC, recover the fractional part as a normalized [0,1]
         * screen coord and map it to NDC (screen Y-down -> clip Y-up). In this
         * layout the attribute is [posX, posY, U, V] (size 4), so the last two
         * components are the atlas texcoords, not depth.
         * TODO: execute the real vertex-program / RSX viewport transform so
         * this is not needed. */
        if (out->x > 2.0f || out->x < -2.0f || out->y > 2.0f || out->y < -2.0f) {
            float sx = out->x - floorf(out->x);
            float sy = out->y - floorf(out->y);
            out->x = sx * 2.0f - 1.0f;
            out->y = 1.0f - sy * 2.0f;
            out->z = 0.0f;
            if (pos->size >= 4) {
                out->u = rd_bef(src + 8);   /* U */
                out->v = rd_bef(src + 12);  /* V */
            }
        }
    }

    const rsx_vertex_attrib* col = &state->vertex_attribs[3];
    if (col->enabled && col->size >= 3) {
        u8* src = vm_base + resolve_vertex_ea(state, col->offset, vindex * col->stride);
        if (col->type == 4 /* ubyte */) {
            out->r = src[0] / 255.0f;
            out->g = src[1] / 255.0f;
            out->b = src[2] / 255.0f;
            out->a = (col->size >= 4) ? src[3] / 255.0f : 1.0f;
        } else if (col->type == 2 /* float */) {
            u32 fr, fg, fb, fa;
            memcpy(&fr, src,     4); fr = ((fr>>24)&0xFF)|((fr>>8)&0xFF00)|((fr<<8)&0xFF0000)|((fr<<24)&0xFF000000);
            memcpy(&fg, src + 4, 4); fg = ((fg>>24)&0xFF)|((fg>>8)&0xFF00)|((fg<<8)&0xFF0000)|((fg<<24)&0xFF000000);
            memcpy(&fb, src + 8, 4); fb = ((fb>>24)&0xFF)|((fb>>8)&0xFF00)|((fb<<8)&0xFF0000)|((fb<<24)&0xFF000000);
            memcpy(&out->r, &fr, 4); memcpy(&out->g, &fg, 4); memcpy(&out->b, &fb, 4);
            if (col->size >= 4) {
                memcpy(&fa, src + 12, 4); fa = ((fa>>24)&0xFF)|((fa>>8)&0xFF00)|((fa<<8)&0xFF0000)|((fa<<24)&0xFF000000);
                memcpy(&out->a, &fa, 4);
            }
        }
    }
}

/* Upload `count` sequential vertices [first, first+count). Returns the count
 * actually written (clamped to the remaining per-frame buffer). */
static u32 upload_vertices_from_rsx(u32 first, u32 count)
{
    BasicVertex* verts = (BasicVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 max_verts = (MAX_VERTICES * VERTEX_STRIDE - s_d3d.vb_offset) / sizeof(BasicVertex);
    if (count > max_verts) count = max_verts;
    const rsx_state* state = s_d3d.current_rsx_state;
    for (u32 i = 0; i < count; i++)
        read_rsx_vertex(state, first + i, &verts[i]);
    s_d3d.vb_offset += count * sizeof(BasicVertex);
    return count;
}

/* Upload RSX QUADS (prim 8) as a triangle list: each 4-vertex quad v0..v3
 * (perimeter winding) splits into triangles (v0,v1,v2) and (v0,v2,v3).
 * D3D12 has no quad topology, so this expansion is how quads render at all.
 * Returns the number of triangle-list vertices emitted (6 per quad). */
static u32 upload_quads_from_rsx(u32 first, u32 count)
{
    const rsx_state* state = s_d3d.current_rsx_state;
    u32 quads = count / 4;
    u32 max_verts = (MAX_VERTICES * VERTEX_STRIDE - s_d3d.vb_offset) / sizeof(BasicVertex);
    if (quads * 6 > max_verts) quads = max_verts / 6;
    BasicVertex* verts = (BasicVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        BasicVertex c[4];
        for (u32 k = 0; k < 4; k++)
            read_rsx_vertex(state, first + q * 4 + k, &c[k]);
        verts[o++] = c[0]; verts[o++] = c[1]; verts[o++] = c[2];
        verts[o++] = c[0]; verts[o++] = c[2]; verts[o++] = c[3];
    }
    s_d3d.vb_offset += o * sizeof(BasicVertex);
    return o;
}

/* Upload RSX QUADS as raw float4 attrib0 (byte-swapped) into vp_vb, expanded
 * to a triangle list (6 verts/quad). The decompiled vertex shader does the
 * transform. Returns emitted vertex count. */
/* Generic VP vertex: all 16 RSX vertex attributes, each converted to a float4
 * slot -- 256 bytes/vertex, input layout ATTRi @ i*16. Apps place attributes at
 * arbitrary indices (tiny3d: pos=a0 colour=a3 tex=a8; SDK gcm samples: pos=a0
 * colour=a1 tex=a2; dbgfont: a0..a2), so a hardcoded pos+colour pair can't
 * cover them: every enabled attrib is fetched from guest memory and converted
 * by its RSX type; disabled slots read (0,0,0,1). */
typedef struct { float v[4]; } VPSlot;
#define VP_VERT_STRIDE (16 * sizeof(VPSlot))   /* 256 */
static float s_last_vp_first_z;

static float rd_half_be(const u8* p)
{
    u16 h = (u16)((p[0] << 8) | p[1]);
    u32 sgn = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    u32 f;
    if (exp == 0)       f = (sgn << 31);                                    /* +-0 / denorm->0 */
    else if (exp == 31) f = (sgn << 31) | 0x7F800000u | (man << 13);        /* inf/nan */
    else                f = (sgn << 31) | ((exp - 15 + 127) << 23) | (man << 13);
    float out; memcpy(&out, &f, 4); return out;
}

static void read_vp_vertex(const rsx_state* state, u32 vi, VPSlot* out16)
{
    extern uint8_t* vm_base;
    for (int i = 0; i < 16; i++) {
        VPSlot* o = &out16[i];
        o->v[0] = o->v[1] = o->v[2] = 0.0f; o->v[3] = 1.0f;
        const rsx_vertex_attrib* a = &state->vertex_attribs[i];
        if (!a->enabled || a->stride == 0) continue;
        const u8* p = vm_base + resolve_vertex_ea(state, a->offset, vi * a->stride);
        u32 n = a->size ? a->size : 4; if (n > 4) n = 4;
        switch (a->type) {
        case 2: /* CELL_GCM_VERTEX_F: float32 BE */
            for (u32 k = 0; k < n; k++) o->v[k] = rd_bef(p + k * 4);
            break;
        case 3: /* SF: half float BE */
            for (u32 k = 0; k < n; k++) o->v[k] = rd_half_be(p + k * 2);
            break;
        case 4: /* UB: u8 normalized [0,1] */
            for (u32 k = 0; k < n; k++) o->v[k] = p[k] / 255.0f;
            break;
        case 1: /* S1: s16 normalized [-1,1] */
            for (u32 k = 0; k < n; k++) {
                s16 s = (s16)((p[k*2] << 8) | p[k*2+1]);
                o->v[k] = (float)s / 32767.0f;
            }
            break;
        case 5: /* S32K: s16 integer */
            for (u32 k = 0; k < n; k++)
                o->v[k] = (float)(s16)((p[k*2] << 8) | p[k*2+1]);
            break;
        case 7: /* UB256: u8 unnormalized */
            for (u32 k = 0; k < n; k++) o->v[k] = (float)p[k];
            break;
        default:
            for (u32 k = 0; k < n; k++) o->v[k] = rd_bef(p + k * 4);
            break;
        }
    }
}

/* D3D12 upload heaps are write-combined on common drivers. Never read a tie
 * key back from the mapped heap: keep the first vertex on the CPU stack, save
 * its z, then write the completed 256-byte vertex once. */
static void upload_one_vp_vertex(const rsx_state* state, u32 vi,
                                 VPSlot* out16, int first_output)
{
    VPSlot tmp[16];
    read_vp_vertex(state, vi, tmp);
    if (first_output) s_last_vp_first_z = tmp[0].v[2];
    memcpy(out16, tmp, sizeof(tmp));
}

static void vp_attrs_dbg(const rsx_state* state)
{
    if (!getenv("VP_ATTRS")) return;
    static int _a = 0; if (_a++ >= 4) return;
    for (int i = 0; i < 16; i++) {
        const rsx_vertex_attrib* a = &state->vertex_attribs[i];
        if (a->enabled) fprintf(stderr, "[VPATTR] a%d off=0x%X stride=%u size=%u type=%u\n",
                                i, a->offset, a->stride, a->size, a->type);
    }
}

static u32 upload_quads_vp(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    u32 quads = count / 4;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (quads * 6 > maxv) quads = maxv / 6;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        VPSlot c[4][16];
        for (u32 k = 0; k < 4; k++)
            read_vp_vertex(state, first + q*4 + k, c[k]);
        if (q == 0) s_last_vp_first_z = c[0][0].v[2];
        /* quad -> two triangles (perimeter winding) */
        static const int idx[6] = {0,1,2, 0,2,3};
        for (int t = 0; t < 6; t++) { memcpy(&out[o*16], c[idx[t]], sizeof(c[0])); o++; }
        if (getenv("VTX_DUMP")) { static int _n=0; if (_n++ < 4) {
            FILE* f = fopen("vtx_dump.txt", _n==1 ? "w" : "a");
            if (f) {
                const float* vs_ = state->viewport_scale;
                const float* vo_ = state->viewport_offset;
                fprintf(f, "q%02d vp_scale=(%.1f,%.1f,%.3f) vp_off=(%.1f,%.1f,%.3f)\n", _n,
                        vs_[0], vs_[1], vs_[2], vo_[0], vo_[1], vo_[2]);
                static const int dbg_constants[] = {256, 257, 258, 259, 464, 465, 466, 467};
                for (u32 _ci = 0; _ci < sizeof(dbg_constants)/sizeof(dbg_constants[0]); _ci++) {
                    int _c = dbg_constants[_ci];
                    fprintf(f, "  c[%d]=(%.6g,%.6g,%.6g,%.6g)\n", _c,
                            state->vertex_constants[_c][0], state->vertex_constants[_c][1],
                            state->vertex_constants[_c][2], state->vertex_constants[_c][3]);
                }
                for (u32 k = 0; k < 4; k++)
                    fprintf(f, "  v%u a0=(%.3f,%.3f,%.3f,%.3f) a8=(%.3f,%.3f)\n", k,
                        c[k][0].v[0],c[k][0].v[1],c[k][0].v[2],c[k][0].v[3],
                        c[k][8].v[0],c[k][8].v[1]);
                fclose(f);
            } } }
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* Straight triangle-list upload through the VP path (gcm/cube's cube draws
 * TRIANGLES, prim 5 -- no expansion needed). */
static u32 upload_tris_vp(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (count > maxv) count = maxv - (maxv % 3);
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    for (u32 k = 0; k < count; k++)
        upload_one_vp_vertex(state, first + k, &out[k*16], k == 0);
    if (getenv("VTX_DUMP")) { static int _n=0; if (_n++ < 1) {
        FILE* f = fopen("vtx_dump.txt", "w");
        if (f) { for (u32 k = 0; k < count; k++)
            fprintf(f, "v%02u pos=(%.3f,%.3f,%.3f,%.3f) uv=(%.3f,%.3f)\n", k,
                out[k*16].v[0],out[k*16].v[1],out[k*16].v[2],out[k*16].v[3],
                out[k*16+2].v[0],out[k*16+2].v[1]);
          fclose(f); } } }
    s_d3d.vp_vb_offset += count * VP_VERT_STRIDE;
    return count;
}

/* Straight triangle-strip upload through the VP path.  Lumen uses four-vertex
 * strips for most of its 2D sprites.  Sending these through the legacy
 * fallback loses both the guest vertex transform and all texture bindings,
 * leaving only non-Lumen/debug text visible. */
static u32 upload_strip_vp(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (count > maxv) count = maxv;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    for (u32 k = 0; k < count; k++)
        upload_one_vp_vertex(state, first + k, &out[k*16], k == 0);
    if (getenv("VTX_DUMP")) { static int dumped = 0; if (!dumped++) {
        FILE* f = fopen("strip_vtx_dump.txt", "w");
        if (f) {
            fprintf(f, "first=%u count=%u viewport=%u,%u %ux%u scale=(%.6g,%.6g,%.6g) offset=(%.6g,%.6g,%.6g)\n",
                    first, count, state->viewport_x, state->viewport_y,
                    state->viewport_w, state->viewport_h,
                    state->viewport_scale[0], state->viewport_scale[1], state->viewport_scale[2],
                    state->viewport_offset[0], state->viewport_offset[1], state->viewport_offset[2]);
            for (u32 k = 0; k < count && k < 8; k++) {
                fprintf(f, "v%u", k);
                for (int a = 0; a < 16; a++) if (state->vertex_attribs[a].enabled)
                    fprintf(f, " a%d=(%.6g,%.6g,%.6g,%.6g)", a,
                            out[k*16+a].v[0], out[k*16+a].v[1],
                            out[k*16+a].v[2], out[k*16+a].v[3]);
                fputc('\n', f);
            }
            fclose(f);
        }
    } }
    s_d3d.vp_vb_offset += count * VP_VERT_STRIDE;
    return count;
}

/* Fetch index k from the guest index array (SET_INDEX_ARRAY_ADDRESS/_DMA:
 * dma [3:0] = location (0 local, 1 main), [7:4] = type (0 u32, 1 u16)).
 * Indices are big-endian in guest memory. */
static u32 read_guest_index_raw(const rsx_state* st, u32 k)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    int local = ((st->index_array_dma & 0xF) == 0);
    u32 base = cellGcmResolveLocated(local, st->index_array_offset);
    const u8* p = vm_base + base;
    if (((st->index_array_dma >> 4) & 0xF) == 1) {
        p += (u64)k * 2;
        return ((u32)p[0] << 8) | p[1];
    }
    p += (u64)k * 4;
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | p[3];
}

static u32 resolve_guest_index(const rsx_state* st, u32 index)
{
    return (index + st->vertex_data_base_index) & 0x000FFFFFu;
}

static u32 read_guest_index(const rsx_state* st, u32 k)
{
    return resolve_guest_index(st, read_guest_index_raw(st, k));
}

/* TAIKO_VERTEX_RACE_TRACE=1: fingerprint the exact indexed position source
 * consumed by Don-chan's fill/outline draws.  RenderDoc proved that corrupt
 * frames reach the GPU with a coherent static UV stream but isolated skinned
 * positions such as (0.5,-0.5,359), so compare the guest position arena before
 * and after CPU de-indexing to catch a concurrent writer at the source. */
static int vp_vertex_race_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* setting = getenv("TAIKO_VERTEX_RACE_TRACE");
        enabled = (setting && setting[0] != '0') ? 1 : 0;
    }
    return enabled;
}

static u32 vp_hash_indexed_position_source(const rsx_state* st,
                                           u32 first, u32 count)
{
    extern uint8_t* vm_base;
    const rsx_vertex_attrib* a = &st->vertex_attribs[0];
    u32 hash = 2166136261u;
    for (u32 k = 0; k < count; k++) {
        const u32 raw = read_guest_index_raw(st, first + k);
        const u32 vi = resolve_guest_index(st, raw);
        const u32 ea = resolve_vertex_ea(st, a->offset, vi * a->stride);
        for (u32 b = 0; b < 12; b++) {
            hash ^= vm_base[ea + b];
            hash *= 16777619u;
        }
        for (u32 b = 0; b < 4; b++) {
            hash ^= (raw >> (b * 8)) & 0xFFu;
            hash *= 16777619u;
        }
    }
    return hash;
}

static void vp_report_bad_indexed_tri(const rsx_state* st, u32 first,
                                      const VPSlot* out, u32 base,
                                      float max_edge2, u32 source_before,
                                      u32 source_after)
{
    extern uint8_t* vm_base;
    extern void (*g_spurs_job_output_probe)(u32 ea);
    const rsx_vertex_attrib* a0 = &st->vertex_attribs[0];
    const rsx_vertex_attrib* a8 = &st->vertex_attribs[8];
    fprintf(stderr,
            "[VTXBAD] seq=%u fp=%08X tri=%u edge2=%.7g srcHash=%08X->%08X "
            "ib=%08X/dma%X a0=%08X/s%u baseOff=%08X baseIdx=%08X\n",
            s_draw_seq, st->shader_program & ~3u, base / 3, max_edge2,
            source_before, source_after, st->index_array_offset,
            st->index_array_dma, a0->offset, a0->stride,
            st->vertex_data_base_offset, st->vertex_data_base_index);
    for (u32 local = 0; local < 3; local++) {
        const u32 k = base + local;
        const u32 raw = read_guest_index_raw(st, first + k);
        const u32 vi = resolve_guest_index(st, raw);
        const u32 ea0 = resolve_vertex_ea(st, a0->offset, vi * a0->stride);
        const u32 ea8 = a8->enabled
            ? resolve_vertex_ea(st, a8->offset, vi * a8->stride) : 0;
        const VPSlot* v = &out[k * 16];
        fprintf(stderr,
                "[VTXBAD]   k=%u raw=%u vi=%u ea0=%08X "
                "copied=(%.7g,%.7g,%.7g) now=(%.7g,%.7g,%.7g) ea8=%08X\n",
                k, raw, vi, ea0, v[0].v[0], v[0].v[1], v[0].v[2],
                rd_bef(vm_base + ea0), rd_bef(vm_base + ea0 + 4),
                rd_bef(vm_base + ea0 + 8), ea8);
        if (g_spurs_job_output_probe)
            g_spurs_job_output_probe(ea0);
    }
    fflush(stderr);
}

/* Indexed variants of the VP-path uploads. */
static u32 upload_quads_vp_indexed(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    u32 quads = count / 4;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (quads * 6 > maxv) quads = maxv / 6;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        VPSlot c[4][16];
        for (u32 k = 0; k < 4; k++)
            read_vp_vertex(state, read_guest_index(state, first + q*4 + k), c[k]);
        if (q == 0) s_last_vp_first_z = c[0][0].v[2];
        static const int idx[6] = {0,1,2, 0,2,3};
        for (int t = 0; t < 6; t++) { memcpy(&out[o*16], c[idx[t]], sizeof(c[0])); o++; }
        if (getenv("VTX_DUMP")) { static int _n=0; if (_n++ < 6) {
            FILE* f = fopen("vtx_dump.txt", _n==1 ? "w" : "a");
            if (f) {
                const float* vs_ = state->viewport_scale;
                const float* vo_ = state->viewport_offset;
                fprintf(f, "iq%02d vp_scale=(%.1f,%.1f,%.3f) vp_off=(%.1f,%.1f,%.3f)\n", _n,
                        vs_[0], vs_[1], vs_[2], vo_[0], vo_[1], vo_[2]);
                for (int _c = 464; _c <= 467; _c++)
                    fprintf(f, "  c[%d]=(%.3f,%.3f,%.3f,%.3f)\n", _c,
                            state->vertex_constants[_c][0], state->vertex_constants[_c][1],
                            state->vertex_constants[_c][2], state->vertex_constants[_c][3]);
                for (u32 k = 0; k < 4; k++)
                    fprintf(f, "  v%u i%u a0=(%.3f,%.3f,%.3f,%.3f) a8=(%.3f,%.3f)\n", k,
                        read_guest_index(state, first + q*4 + k),
                        c[k][0].v[0],c[k][0].v[1],c[k][0].v[2],c[k][0].v[3],
                        c[k][8].v[0],c[k][8].v[1]);
                fclose(f);
            } } }
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

static u32 upload_tris_vp_indexed(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (count > maxv) count = maxv - (maxv % 3);
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    const u32 body_fp = state->shader_program & ~3u;
    const int race_trace = vp_vertex_race_trace_enabled() &&
        (body_fp == 0x01AA8E40u || body_fp == 0x01AA8F00u);
    const u32 source_before = race_trace
        ? vp_hash_indexed_position_source(state, first, count) : 0;
    for (u32 k = 0; k < count; k++)
        upload_one_vp_vertex(state, read_guest_index(state, first + k),
                             &out[k*16], k == 0);
    if (race_trace) {
        const u32 source_after =
            vp_hash_indexed_position_source(state, first, count);
        if (source_before != source_after) {
            fprintf(stderr,
                    "[VTXRACE] seq=%u fp=%08X n=%u source changed %08X->%08X\n",
                    s_draw_seq, body_fp, count, source_before, source_after);
        }
        for (u32 base = 0; base + 2 < count; base += 3) {
            float max_edge2 = 0.0f;
            for (u32 edge = 0; edge < 3; edge++) {
                const VPSlot* a = &out[(base + edge) * 16];
                const VPSlot* b = &out[(base + (edge + 1) % 3) * 16];
                const float dx = a[0].v[0] - b[0].v[0];
                const float dy = a[0].v[1] - b[0].v[1];
                const float dz = a[0].v[2] - b[0].v[2];
                const float edge2 = dx * dx + dy * dy + dz * dz;
                if (edge2 > max_edge2) max_edge2 = edge2;
            }
            if (max_edge2 > 400.0f) {
                vp_report_bad_indexed_tri(state, first, out, base,
                                          max_edge2, source_before, source_after);
                break;
            }
        }
    }
    s_d3d.vp_vb_offset += count * VP_VERT_STRIDE;
    return count;
}

static u32 upload_strip_vp_indexed(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    const u32 body_fp = state->shader_program & ~3u;
    const int race_trace = vp_vertex_race_trace_enabled() &&
        (body_fp == 0x01AA8E40u || body_fp == 0x01AA8F00u);
    const u32 source_before = race_trace
        ? vp_hash_indexed_position_source(state, first, count) : 0;
    rsx_triangle_strip_expander strip = {{0, 0}, 0};
    u32 emitted = 0;
    u32 bad_base = 0xFFFFFFFFu;
    u32 bad_raw[3] = {0, 0, 0};
    float bad_edge2 = 0.0f;
    for (u32 k = 0; k < count && emitted + 3 <= maxv; k++) {
        u32 a, b, c;
        u32 raw = read_guest_index_raw(state, first + k);
        if (!rsx_triangle_strip_push(&strip, raw,
                                     state->restart_index_enable,
                                     state->restart_index,
                                     &a, &b, &c))
            continue;
        upload_one_vp_vertex(state, resolve_guest_index(state, a),
                             &out[(emitted + 0)*16], emitted == 0);
        upload_one_vp_vertex(state, resolve_guest_index(state, b),
                             &out[(emitted + 1)*16], 0);
        upload_one_vp_vertex(state, resolve_guest_index(state, c),
                             &out[(emitted + 2)*16], 0);
        if (race_trace && bad_base == 0xFFFFFFFFu) {
            float max_edge2 = 0.0f;
            for (u32 edge = 0; edge < 3; edge++) {
                const VPSlot* va = &out[(emitted + edge) * 16];
                const VPSlot* vb = &out[(emitted + (edge + 1) % 3) * 16];
                const float dx = va[0].v[0] - vb[0].v[0];
                const float dy = va[0].v[1] - vb[0].v[1];
                const float dz = va[0].v[2] - vb[0].v[2];
                const float edge2 = dx * dx + dy * dy + dz * dz;
                if (edge2 > max_edge2) max_edge2 = edge2;
            }
            if (max_edge2 > 100.0f) {
                bad_base = emitted;
                bad_raw[0] = a; bad_raw[1] = b; bad_raw[2] = c;
                bad_edge2 = max_edge2;
            }
        }
        emitted += 3;
    }
    if (race_trace) {
        extern uint8_t* vm_base;
        extern void (*g_spurs_job_output_probe)(u32 ea);
        const rsx_vertex_attrib* a0 = &state->vertex_attribs[0];
        const u32 source_after =
            vp_hash_indexed_position_source(state, first, count);
        if (source_before != source_after) {
            fprintf(stderr,
                    "[VTXRACE] seq=%u fp=%08X stripN=%u source changed %08X->%08X\n",
                    s_draw_seq, body_fp, count, source_before, source_after);
        }
        if (bad_base != 0xFFFFFFFFu) {
            fprintf(stderr,
                    "[VTXBAD] seq=%u fp=%08X stripTri=%u edge2=%.7g "
                    "srcHash=%08X->%08X ib=%08X/dma%X a0=%08X/s%u\n",
                    s_draw_seq, body_fp, bad_base / 3, bad_edge2,
                    source_before, source_after, state->index_array_offset,
                    state->index_array_dma, a0->offset, a0->stride);
            for (u32 local = 0; local < 3; local++) {
                const u32 vi = resolve_guest_index(state, bad_raw[local]);
                const u32 ea0 = resolve_vertex_ea(
                    state, a0->offset, vi * a0->stride);
                const VPSlot* v = &out[(bad_base + local) * 16];
                fprintf(stderr,
                        "[VTXBAD]   out=%u raw=%u vi=%u ea0=%08X "
                        "copied=(%.7g,%.7g,%.7g) now=(%.7g,%.7g,%.7g)\n",
                        bad_base + local, bad_raw[local], vi, ea0,
                        v[0].v[0], v[0].v[1], v[0].v[2],
                        rd_bef(vm_base + ea0), rd_bef(vm_base + ea0 + 4),
                        rd_bef(vm_base + ea0 + 8));
                if (g_spurs_job_output_probe)
                    g_spurs_job_output_probe(ea0);
            }
            fflush(stderr);
        }
    }
    s_d3d.vp_vb_offset += emitted * VP_VERT_STRIDE;
    return emitted;
}

static void d3d12_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    if (s_rtt_hotkey_frames > 0) {
        u32 rw = 0, rh = 0, rt2 = 0;
        u32 rt = current_rt_off(&rw, &rh, &rt2);
        const rsx_state* st = s_d3d.current_rsx_state;
        fprintf(stderr,
                "[PRIMCAP] arrays prim=%u first=%u count=%u rec=%u "
                "rt=0x%X/%ux%u rt2=0x%X vpbytes=%u a0=%u tex=%d\n",
                primitive, first, count, s_d3d.draw_count,
                rt, rw, rh, rt2,
                st ? st->vp_ucode_bytes : 0,
                st ? st->vertex_attribs[0].enabled : 0,
                s_d3d.tex_bound);
    }
    /* Log the first 20 calls in detail, then every 1000th to show liveness
     * without flooding. */
    static u64 s_total = 0;
    if (s_total < 20 || (s_total % 1000) == 0) {
        printf("[D3D12] draw_arrays #%llu prim=%u first=%u count=%u\n",
               (unsigned long long)s_total, primitive, first, count);
    }
    s_total++;

    if (!s_d3d.pipeline_ready || !s_d3d.vb_mapped) return;
    if (count == 0 || count > MAX_VERTICES) return;

    /* Vertex/constant/FP snapshots below write persistently mapped upload
     * heaps.  Fence before the first write, not later in render_frame(). */
    protect_upload_heaps_for_batch();

    /* QUADS (prim 8) have no D3D12 topology; expand to a triangle list.
     * dbgfont draws all text as quads. Prefer the real vertex-program path
     * (exact position + texcoord): upload raw float4 attrib0 into vp_vb and
     * mark the draw is_vp; render_frame compiles the VP and draws it. Fall
     * back to the frac-approximation textured path if VP resources are absent. */
    if (primitive == 8 /* CELL_GCM_PRIMITIVE_QUADS */) {
        /* Prefer the real vertex-program path whenever VP resources exist --
         * NOT only when a texture is bound. Requiring tex_bound routed vkcube's
         * UNtextured cube to the fixed-function fallback, which never applies the
         * VP's MVP transform, so the cube was drawn in object space and clipped
         * off-screen. Untextured VP draws now render via the colour PSO. */
        if (s_d3d.vp_vb_mapped && s_d3d.vp_root_sig) {
            u32 rec = s_d3d.vp_vb_offset;
            u32 emitted = upload_quads_vp(s_d3d.current_rsx_state, first, count);
            if (emitted && s_d3d.draw_count < MAX_DRAWS) {
                D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count];
                dr->vb_byte_offset = rec;
                dr->vertex_count   = emitted;
                dr->topology       = D3D_TOPOLOGY_TRIANGLELIST;
                dr->textured       = s_d3d.tex_bound;
                dr->is_vp          = 1;
                dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
                fp_snapshot_draw(dr, s_d3d.draw_count);
                dr->fp_exp32 = s_d3d.current_rsx_state ?
                    ((s_d3d.current_rsx_state->shader_control & 0x40) != 0) : 1;
                dr->cmask = 0xF;
                if (s_d3d.current_rsx_state) {
                    u32 _cm = s_d3d.current_rsx_state->color_mask;
                    dr->cmask = ((_cm & 0x00010000) ? 1u : 0u)   /* R */
                              | ((_cm & 0x00000100) ? 2u : 0u)   /* G */
                              | ((_cm & 0x00000001) ? 4u : 0u)   /* B */
                              | ((_cm & 0x01000000) ? 8u : 0u);  /* A */
                }
                dr->alpha_ctl = 0;
                if (s_d3d.current_rsx_state)
                    dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                                  | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                                  | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
                for (int _u = 0; _u < 4; _u++) {
                    dr->tex[_u].off = s_d3d.cur_texs[_u].off;
                    dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
                    dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
                    dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
                    dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                    dr->tex[_u].pitch = s_d3d.cur_texs[_u].pitch;
                    dr->tex[_u].address = s_d3d.cur_texs[_u].address;
                    dr->tex[_u].control1 = s_d3d.cur_texs[_u].control1;
                    dr->tex[_u].border_color = s_d3d.cur_texs[_u].border_color;
                    dr->tex[_u].set = s_d3d.cur_texs[_u].set;
                    dr->tex_rt[_u]  = -1;
                }
                dr->tex_slot = -1;
                dr->vs_idx = vp_get_vs(s_d3d.current_rsx_state);
                dr->is_clear = 0;
                dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
                dr->blend_sf = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_sfactor : 0x00010302u;
                dr->blend_df = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_dfactor : 0x00000303u;
                dr->blend_eq = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_equation : 0x80068006u;
                dr->blend_color = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_color : 0;
                dr->depth_test = s_d3d.current_rsx_state ?
                    s_d3d.current_rsx_state->depth_test_enable : 0;
                dr->depth_mask = s_d3d.current_rsx_state ?
                    s_d3d.current_rsx_state->depth_mask : 0;
                dr->depth_func = s_d3d.current_rsx_state ?
                    s_d3d.current_rsx_state->depth_func : 0x207;
                dr->cull_enable = s_d3d.current_rsx_state ?
                    s_d3d.current_rsx_state->cull_face_enable : 0;
                dr->cull_face = s_d3d.current_rsx_state ?
                    s_d3d.current_rsx_state->cull_face : 0x405;
                dr->front_face = s_d3d.current_rsx_state ?
                    s_d3d.current_rsx_state->front_face : 0x900;
                dr->stencil_test = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_test_enable : 0;
                dr->stencil_func = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_func : 0x207;
                dr->stencil_ref = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_ref : 0;
                dr->stencil_mask = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_mask : 0xFF;
                dr->stencil_fail = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_op_fail : 0x1E00;
                dr->stencil_zfail = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_op_zfail : 0x1E00;
                dr->stencil_zpass = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_op_zpass : 0x1E00;
                dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, &dr->rt_off2);
        dr->rt_raw = current_surface_raw();
        { extern u32 cellGcm_current_fifo_getoff(void);
          dr->fifo_off = cellGcm_current_fifo_getoff();
          dr->seq = s_draw_seq++; }
                dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
                if (s_d3d.current_rsx_state) {
                    dr->vp_x = s_d3d.current_rsx_state->viewport_x;
                    dr->vp_y = s_d3d.current_rsx_state->viewport_y;
                    dr->vp_w = s_d3d.current_rsx_state->viewport_w;
                    dr->vp_h = s_d3d.current_rsx_state->viewport_h;
                    dr->sc_x = s_d3d.current_rsx_state->scissor_x;
                    dr->sc_y = s_d3d.current_rsx_state->scissor_y;
                    dr->sc_w = s_d3d.current_rsx_state->scissor_w;
                    dr->sc_h = s_d3d.current_rsx_state->scissor_h;
                } else {
                    dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0;
                    dr->sc_x = dr->sc_y = dr->sc_w = dr->sc_h = 0;
                }
                dr->tie_vz = s_last_vp_first_z;
                vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
                s_d3d.draw_count++;
            }
            return;
        }
        u32 record_offset = s_d3d.vb_offset;
        u32 emitted = upload_quads_from_rsx(first, count);
        if (emitted == 0) return;
        if (s_d3d.draw_count < MAX_DRAWS) {
            s_d3d.draws[s_d3d.draw_count].vb_byte_offset = record_offset;
            s_d3d.draws[s_d3d.draw_count].vertex_count   = emitted;
            s_d3d.draws[s_d3d.draw_count].topology       = D3D_TOPOLOGY_TRIANGLELIST;
            s_d3d.draws[s_d3d.draw_count].textured       = s_d3d.tex_bound;
            s_d3d.draws[s_d3d.draw_count].is_vp          = 0;
            s_d3d.draws[s_d3d.draw_count].is_clear       = 0;
            s_d3d.draws[s_d3d.draw_count].rt_off         = 0;
            s_d3d.draw_count++;
        }
        return;
    }

    /* TRIANGLES (prim 5): same VP-path preference as quads -- the guest's
     * vertex program does the MVP transform (gcm/cube draws its cube this way);
     * the fixed-function fallback below applies no transform, so 3D geometry
     * ends up in object space (invisible/garbage). */
    /* ...but only when the guest ACTUALLY HAS a vertex program. The VP path
     * transforms via the guest's VP microcode; with no microcode loaded it can
     * transform nothing and the draw silently produces zero pixels (it also
     * uploads to vp_vb, leaving the fixed-function vb empty). Geometry that is
     * already in clip space with no VP -- e.g. flOw's injected scene -- must go
     * down the fixed-function passthrough below instead. */
    if ((primitive == 5 /* CELL_GCM_PRIMITIVE_TRIANGLES */ ||
         primitive == 6 /* CELL_GCM_PRIMITIVE_TRIANGLE_STRIP */) &&
        s_d3d.vp_vb_mapped && s_d3d.vp_root_sig &&
        s_d3d.current_rsx_state && s_d3d.current_rsx_state->vp_ucode_bytes >= 16) {
        u32 rec = s_d3d.vp_vb_offset;
        u32 emitted = primitive == 6
            ? upload_strip_vp(s_d3d.current_rsx_state, first, count)
            : upload_tris_vp(s_d3d.current_rsx_state, first, count);
        if (emitted && s_d3d.draw_count < MAX_DRAWS) {
            D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count];
            dr->vb_byte_offset = rec;
            dr->vertex_count   = emitted;
            dr->topology       = primitive == 6
                ? D3D_TOPOLOGY_TRIANGLESTRIP : D3D_TOPOLOGY_TRIANGLELIST;
            dr->textured       = s_d3d.tex_bound;
            dr->is_vp          = 1;
            dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
            fp_snapshot_draw(dr, s_d3d.draw_count);
            dr->fp_exp32 = s_d3d.current_rsx_state ?
                ((s_d3d.current_rsx_state->shader_control & 0x40) != 0) : 1;
            dr->cmask = 0xF;
            if (s_d3d.current_rsx_state) {
                u32 _cm = s_d3d.current_rsx_state->color_mask;
                dr->cmask = ((_cm & 0x00010000) ? 1u : 0u)   /* R */
                          | ((_cm & 0x00000100) ? 2u : 0u)   /* G */
                          | ((_cm & 0x00000001) ? 4u : 0u)   /* B */
                          | ((_cm & 0x01000000) ? 8u : 0u);  /* A */
            }
            dr->alpha_ctl = 0;
            if (s_d3d.current_rsx_state)
                dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                              | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                              | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
            for (int _u = 0; _u < 4; _u++) {
                dr->tex[_u].off = s_d3d.cur_texs[_u].off;
                dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
                dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
                dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
                dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                dr->tex[_u].pitch = s_d3d.cur_texs[_u].pitch;
                dr->tex[_u].address = s_d3d.cur_texs[_u].address;
                dr->tex[_u].control1 = s_d3d.cur_texs[_u].control1;
                dr->tex[_u].border_color = s_d3d.cur_texs[_u].border_color;
                dr->tex[_u].set = s_d3d.cur_texs[_u].set;
                dr->tex_rt[_u]  = -1;
            }
            dr->tex_slot = -1;
            dr->vs_idx = vp_get_vs(s_d3d.current_rsx_state);
            dr->is_clear = 0;
            dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
            dr->blend_sf = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_sfactor : 0x00010302u;
            dr->blend_df = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_dfactor : 0x00000303u;
            dr->blend_eq = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_equation : 0x80068006u;
            dr->blend_color = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_color : 0;
            dr->depth_test = s_d3d.current_rsx_state ?
                s_d3d.current_rsx_state->depth_test_enable : 0;
            dr->depth_mask = s_d3d.current_rsx_state ?
                s_d3d.current_rsx_state->depth_mask : 0;
            dr->depth_func = s_d3d.current_rsx_state ?
                s_d3d.current_rsx_state->depth_func : 0x207;
            dr->cull_enable = s_d3d.current_rsx_state ?
                s_d3d.current_rsx_state->cull_face_enable : 0;
            dr->cull_face = s_d3d.current_rsx_state ?
                s_d3d.current_rsx_state->cull_face : 0x405;
            dr->front_face = s_d3d.current_rsx_state ?
                s_d3d.current_rsx_state->front_face : 0x900;
            dr->stencil_test = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_test_enable : 0;
            dr->stencil_func = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_func : 0x207;
            dr->stencil_ref = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_ref : 0;
            dr->stencil_mask = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_mask : 0xFF;
            dr->stencil_fail = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_op_fail : 0x1E00;
            dr->stencil_zfail = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_op_zfail : 0x1E00;
            dr->stencil_zpass = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_op_zpass : 0x1E00;
            dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, &dr->rt_off2);
        dr->rt_raw = current_surface_raw();
        { extern u32 cellGcm_current_fifo_getoff(void);
          dr->fifo_off = cellGcm_current_fifo_getoff();
          dr->seq = s_draw_seq++; }
            dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
            if (s_d3d.current_rsx_state) {
                dr->vp_x = s_d3d.current_rsx_state->viewport_x;
                dr->vp_y = s_d3d.current_rsx_state->viewport_y;
                dr->vp_w = s_d3d.current_rsx_state->viewport_w;
                dr->vp_h = s_d3d.current_rsx_state->viewport_h;
                dr->sc_x = s_d3d.current_rsx_state->scissor_x;
                dr->sc_y = s_d3d.current_rsx_state->scissor_y;
                dr->sc_w = s_d3d.current_rsx_state->scissor_w;
                dr->sc_h = s_d3d.current_rsx_state->scissor_h;
            } else {
                dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0;
                dr->sc_x = dr->sc_y = dr->sc_w = dr->sc_h = 0;
            }
            dr->tie_vz = s_last_vp_first_z;
            vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
            s_d3d.draw_count++;
        }
        return;
    }

    u32 topo = rsx_to_d3d12_topology(primitive);
    if (topo == D3D_TOPOLOGY_UNDEFINED) {
        /* Other primitives still needing index-buffer conversion
         * (line loops, triangle fans) — skip rather than draw wrong. */
        static int s_skipped_nontri = 0;
        if (s_skipped_nontri < 3) {
            printf("[D3D12] draw_arrays: skipping prim=%u (needs index conversion)\n",
                   primitive);
            s_skipped_nontri++;
        }
        return;
    }

    u32 record_offset = s_d3d.vb_offset;
    u32 actual_count  = upload_vertices_from_rsx(first, count);
    if (actual_count == 0) return;

    if (s_d3d.draw_count < MAX_DRAWS) {
        s_d3d.draws[s_d3d.draw_count].vb_byte_offset = record_offset;
        s_d3d.draws[s_d3d.draw_count].vertex_count   = actual_count;
        s_d3d.draws[s_d3d.draw_count].topology       = topo;
        s_d3d.draws[s_d3d.draw_count].textured       = 0;
        s_d3d.draws[s_d3d.draw_count].is_vp          = 0;
        s_d3d.draws[s_d3d.draw_count].is_clear       = 0;
        s_d3d.draws[s_d3d.draw_count].rt_off         = 0;
        s_d3d.draw_count++;
    }
}

static void d3d12_draw_indexed(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    if (!s_spurs_drained_for_batch) {
        extern void (*g_spurs_job_chain_drain)(void);
        if (g_spurs_job_chain_drain)
            g_spurs_job_chain_drain();
        s_spurs_drained_for_batch = 1;
    }
    if (s_rtt_hotkey_frames > 0) {
        u32 rw = 0, rh = 0, rt2 = 0;
        u32 rt = current_rt_off(&rw, &rh, &rt2);
        const rsx_state* st = s_d3d.current_rsx_state;
        fprintf(stderr,
                "[PRIMCAP] indexed prim=%u first=%u count=%u rec=%u "
                "rt=0x%X/%ux%u rt2=0x%X vpbytes=%u a0=%u tex=%d "
                "idxoff=0x%X dma=0x%X\n",
                primitive, first, count, s_d3d.draw_count,
                rt, rw, rh, rt2,
                st ? st->vp_ucode_bytes : 0,
                st ? st->vertex_attribs[0].enabled : 0,
                s_d3d.tex_bound,
                st ? st->index_array_offset : 0,
                st ? st->index_array_dma : 0);
        /* One compact memory-side snapshot for the first indexed pass.  Its
         * position currently decodes as 0xCDCDCDCD while other attributes
         * remain plausible; show the exact indices and resolved attribute
         * addresses so an address-state bug can be separated from bad guest
         * data without logging every vertex. */
        static int idxcap = 0;
        if (idxcap < 2 && st && first == 0 && count >= 8) {
            extern uint8_t* vm_base;
            extern u32 cellGcmResolveLocated(int local, u32 offset);
            const rsx_vertex_attrib* a0 = &st->vertex_attribs[0];
            const rsx_vertex_attrib* a8 = &st->vertex_attribs[8];
            int ilocal = ((st->index_array_dma & 0xF) == 0);
            u32 iea = cellGcmResolveLocated(ilocal, st->index_array_offset);
            fprintf(stderr,
                    "[IDXCAP] rt=0x%X ib=0x%X->0x%X dma=0x%X "
                    "vbaseoff=0x%X vbaseidx=0x%X "
                    "a0=0x%X/f%X/s%u/n%u/t%u "
                    "a8=0x%X/f%X/s%u/n%u/t%u\n",
                    rt, st->index_array_offset, iea, st->index_array_dma,
                    st->vertex_data_base_offset, st->vertex_data_base_index,
                    a0->offset, a0->format, a0->stride, a0->size, a0->type,
                    a8->offset, a8->format, a8->stride, a8->size, a8->type);
            for (u32 k = 0; k < 8; k++) {
                u32 vi = read_guest_index(st, k);
                u32 ea0 = resolve_vertex_ea(st, a0->offset, vi * a0->stride);
                u32 ea8 = a8->enabled
                    ? resolve_vertex_ea(st, a8->offset, vi * a8->stride) : 0;
                fprintf(stderr, "[IDXCAP] k%u i=%u ea0=0x%X bytes=", k, vi, ea0);
                for (u32 b = 0; b < 16; b++) fprintf(stderr, "%02X", vm_base[ea0 + b]);
                if (ea8) {
                    fprintf(stderr, " ea8=0x%X bytes=", ea8);
                    for (u32 b = 0; b < 8; b++) fprintf(stderr, "%02X", vm_base[ea8 + b]);
                }
                fputc('\n', stderr);
            }
            idxcap++;
        }
    }
    static int log_count = 0;
    if (log_count < 8) {
        printf("[D3D12] draw_indexed(prim=%u, first=%u, count=%u)\n",
               primitive, first, count);
        log_count++;
    }
    if (!s_d3d.pipeline_ready) return;
    if (count == 0 || count > MAX_VERTICES) return;
    if (!s_d3d.vp_vb_mapped || !s_d3d.vp_root_sig) return;

    protect_upload_heaps_for_batch();

    /* Expand through the VP path (indices resolved CPU-side): QUADS -> two
     * triangles per quad, TRIANGLES straight through. Other primitives are
     * skipped rather than drawn wrong. */
    u32 emitted = 0;
    u32 rec = s_d3d.vp_vb_offset;
    if (primitive == 8)      emitted = upload_quads_vp_indexed(s_d3d.current_rsx_state, first, count);
    else if (primitive == 5) emitted = upload_tris_vp_indexed(s_d3d.current_rsx_state, first, count);
    else if (primitive == 6) emitted = upload_strip_vp_indexed(s_d3d.current_rsx_state, first, count);
    else {
        static int _skip = 0;
        if (_skip++ < 3)
            printf("[D3D12] draw_indexed: skipping prim=%u (not wired)\n", primitive);
        return;
    }
    if (emitted && s_d3d.draw_count < MAX_DRAWS) {
        D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count];
        dr->vb_byte_offset = rec;
        dr->vertex_count   = emitted;
        dr->topology       = D3D_TOPOLOGY_TRIANGLELIST;
        dr->textured       = s_d3d.tex_bound;
        dr->is_vp          = 1;
        dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
        fp_snapshot_draw(dr, s_d3d.draw_count);
        dr->fp_exp32 = s_d3d.current_rsx_state ?
            ((s_d3d.current_rsx_state->shader_control & 0x40) != 0) : 1;
        dr->cmask = 0xF;
        if (s_d3d.current_rsx_state) {
            u32 _cm = s_d3d.current_rsx_state->color_mask;
            dr->cmask = ((_cm & 0x00010000) ? 1u : 0u)   /* R */
                      | ((_cm & 0x00000100) ? 2u : 0u)   /* G */
                      | ((_cm & 0x00000001) ? 4u : 0u)   /* B */
                      | ((_cm & 0x01000000) ? 8u : 0u);  /* A */
        }
        dr->alpha_ctl = 0;
        if (s_d3d.current_rsx_state)
            dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                          | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                          | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
        for (int _u = 0; _u < 4; _u++) {
            dr->tex[_u].off = s_d3d.cur_texs[_u].off;
            dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
            dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
            dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
            dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
            dr->tex[_u].pitch = s_d3d.cur_texs[_u].pitch;
            dr->tex[_u].address = s_d3d.cur_texs[_u].address;
            dr->tex[_u].control1 = s_d3d.cur_texs[_u].control1;
            dr->tex[_u].border_color = s_d3d.cur_texs[_u].border_color;
            dr->tex[_u].set = s_d3d.cur_texs[_u].set;
            dr->tex_rt[_u]  = -1;
        }
        /* A VP draw with nothing on texture unit 0: report what the guest
         * last said about that unit.  That is the only way to tell "never
         * bound" from "bound, then dropped by us". */
        if (!dr->tex[0].set && getenv("TEXDROP")) {
            extern u32 s_lastbind_off, s_lastbind_fmt, s_lastbind_w,
                       s_lastbind_h, s_lastbind_ctl0, s_lastbind_n;
            static int logged = 0;
            if (logged++ < 40)
                fprintf(stderr,
                        "[NOTEX] draw=%u lastbind#%u off=0x%X fmt=0x%02X "
                        "%ux%u ctl0=0x%08X\n",
                        s_d3d.draw_count, s_lastbind_n, s_lastbind_off,
                        s_lastbind_fmt, s_lastbind_w, s_lastbind_h,
                        s_lastbind_ctl0);
        }
        dr->tex_slot = -1;
        dr->vs_idx = vp_get_vs(s_d3d.current_rsx_state);
        dr->is_clear = 0;
        dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
        dr->blend_sf = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_sfactor : 0x00010302u;
        dr->blend_df = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_dfactor : 0x00000303u;
        dr->blend_eq = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_equation : 0x80068006u;
        dr->blend_color = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_color : 0;
        dr->depth_test = s_d3d.current_rsx_state ?
            s_d3d.current_rsx_state->depth_test_enable : 0;
        dr->depth_mask = s_d3d.current_rsx_state ?
            s_d3d.current_rsx_state->depth_mask : 0;
        dr->depth_func = s_d3d.current_rsx_state ?
            s_d3d.current_rsx_state->depth_func : 0x207;
        dr->cull_enable = s_d3d.current_rsx_state ?
            s_d3d.current_rsx_state->cull_face_enable : 0;
        dr->cull_face = s_d3d.current_rsx_state ?
            s_d3d.current_rsx_state->cull_face : 0x405;
        dr->front_face = s_d3d.current_rsx_state ?
            s_d3d.current_rsx_state->front_face : 0x900;
        dr->stencil_test = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_test_enable : 0;
        dr->stencil_func = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_func : 0x207;
        dr->stencil_ref = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_ref : 0;
        dr->stencil_mask = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_mask : 0xFF;
        dr->stencil_fail = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_op_fail : 0x1E00;
        dr->stencil_zfail = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_op_zfail : 0x1E00;
        dr->stencil_zpass = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_op_zpass : 0x1E00;
        dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, &dr->rt_off2);
        dr->rt_raw = current_surface_raw();
        { extern u32 cellGcm_current_fifo_getoff(void);
          dr->fifo_off = cellGcm_current_fifo_getoff();
          dr->seq = s_draw_seq++; }
        dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
        if (s_d3d.current_rsx_state) {
            dr->vp_x = s_d3d.current_rsx_state->viewport_x;
            dr->vp_y = s_d3d.current_rsx_state->viewport_y;
            dr->vp_w = s_d3d.current_rsx_state->viewport_w;
            dr->vp_h = s_d3d.current_rsx_state->viewport_h;
            dr->sc_x = s_d3d.current_rsx_state->scissor_x;
            dr->sc_y = s_d3d.current_rsx_state->scissor_y;
            dr->sc_w = s_d3d.current_rsx_state->scissor_w;
            dr->sc_h = s_d3d.current_rsx_state->scissor_h;
        } else {
            dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0;
            dr->sc_x = dr->sc_y = dr->sc_w = dr->sc_h = 0;
        }
        dr->tie_vz = s_last_vp_first_z;
        vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
        s_d3d.draw_count++;
    }
}

u32 s_lastbind_off, s_lastbind_fmt, s_lastbind_w, s_lastbind_h,
    s_lastbind_ctl0, s_lastbind_n;

static void d3d12_bind_texture(void* ud, u32 unit, const rsx_texture_state* tex)
{
    (void)ud;
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);

    u32 width  = (tex->image_rect >> 16) & 0xFFFF;
    u32 height = tex->image_rect & 0xFFFF;
    u32 format = (tex->format >> 8) & 0xFF;
    u32 offset = tex->offset;
    u32 pitch  = tex->control3 & 0x000FFFFFu;

    /* Log state changes rather than merely the first few font draws. */
    static u32 last_off[4] = {~0u,~0u,~0u,~0u};
    static u32 last_fmt[4] = {~0u,~0u,~0u,~0u};
    static int change_logs = 0;
    if (unit < 4 && change_logs < 96 &&
        (last_off[unit] != offset || last_fmt[unit] != format)) {
        printf("[D3D12] bind_texture(unit=%u, offset=0x%X, fmt=0x%02X, %ux%u pitch=%u addr=0x%08X ctl0=0x%08X ctl1=0x%08X filter=0x%08X)\n",
               unit, offset, format, width, height, pitch, tex->address,
               tex->control0, tex->control1, tex->filter);
        last_off[unit] = offset; last_fmt[unit] = format; change_logs++;
    }

    if (unit == 0) {
        s_lastbind_off = offset; s_lastbind_fmt = format;
        s_lastbind_w = width;    s_lastbind_h = height;
        s_lastbind_ctl0 = tex->control0; s_lastbind_n++;
    }

    /* TEXDROP: the two paths that silently discard a binding, one line per
     * distinct (format,w,h).  Without it a draw that reaches the FP with no
     * texture is indistinguishable from one the guest never textured -- which
     * is exactly how the D8R8G8B8 rainbow strip hid. */
    if (getenv("TEXDROP")) {
        static u32 seen[64]; static u32 seen_n;
        u32 key = (format << 24) ^ (width << 12) ^ height;
        int dup = 0;
        for (u32 i = 0; i < seen_n; i++) if (seen[i] == key) { dup = 1; break; }
        u32 bf = format & 0x9F;
        int drop = (width == 0 || height == 0) ||
                   !(bf == 0x81 || bf == 0x85 || bf == 0x86 || bf == 0x87 ||
                     bf == 0x88 || bf == 0x9A);
        if (drop && !dup && seen_n < 64) {
            seen[seen_n++] = key;
            fprintf(stderr,
                    "[TEXDROP] unit=%u off=0x%X fmt=0x%02X %ux%u pitch=%u "
                    "ctl0=0x%08X ctl1=0x%08X\n",
                    unit, offset, format, width, height, pitch,
                    tex->control0, tex->control1);
        }
    }

    if (!vm_base || width == 0 || height == 0) {
        if (unit < 4) memset(&s_d3d.cur_texs[unit], 0, sizeof(s_d3d.cur_texs[unit]));
        return;
    }

    /* Record the currently-bound atlas so subsequent quad draws sample it. The
     * actual GPU upload happens in render_frame (we have no open command list
     * here). Only the 8-bit single-channel font atlas (B8, RSX fmt base 0x81 /
     * as-seen 0xA1 with the LN flag) is wired up so far; other formats fall
     * back to untextured. */
    u32 base_fmt = format & 0x9F;   /* strip LN(0x20)/UN(0x40) flags */
    /* VP path: record the latest bound texture (any supported format) so draws
     * can carry it per-draw. Location bits (format[1:0]): 1 = LOCAL, 2 = MAIN. */
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    int supported = base_fmt == 0x81 /* B8 */ || base_fmt == 0x85 /* A8R8G8B8 */ ||
                    base_fmt == 0x9E /* D8R8G8B8 */ ||
                    base_fmt == 0x86 /* DXT1 */ || base_fmt == 0x87 /* DXT3 */ ||
                    base_fmt == 0x88 /* DXT5 */ ||
                    base_fmt == 0x9A /* W16Z16Y16X16 half-float: RTT */;
    if (unit < 4 && supported) {
        s_d3d.cur_texs[unit].off = cellGcmResolveLocated((tex->format & 3) == 1, offset);
        s_d3d.cur_texs[unit].raw = offset;
        s_d3d.cur_texs[unit].w = width; s_d3d.cur_texs[unit].h = height;
        s_d3d.cur_texs[unit].fmt = format;   /* full byte: LN(0x20)/UN(0x40) kept */
        s_d3d.cur_texs[unit].pitch = pitch;
        s_d3d.cur_texs[unit].address = tex->address;
        s_d3d.cur_texs[unit].control1 = tex->control1;
        s_d3d.cur_texs[unit].border_color = tex->border_color;
        s_d3d.cur_texs[unit].set = 1;
    } else if (unit < 4) {
        memset(&s_d3d.cur_texs[unit], 0, sizeof(s_d3d.cur_texs[unit]));
    }
    if (base_fmt == 0x81 /* B8 */) {
        s_d3d.tex_src_offset = cellGcmResolveLocated((tex->format & 3) == 1, offset);
        if (s_d3d.tex_w != width || s_d3d.tex_h != height) {
            /* dims changed -> resource must be (re)created in render_frame */
            s_d3d.tex_ready = 0;
        }
        s_d3d.tex_w     = width;
        s_d3d.tex_h     = height;
        s_d3d.tex_bound = 1;
        s_d3d.tex_dirty = 1;
    } else {
        s_d3d.tex_bound = 0;
    }
}

static void d3d12_set_vertex_attribs(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;

    /* Log enabled vertex attributes for debugging */
    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_vertex_attribs:\n");
        for (int i = 0; i < 16; i++) {
            const rsx_vertex_attrib* a = &state->vertex_attribs[i];
            if (a->enabled) {
                const char* type_name = "?";
                switch (a->type) {
                case 1: type_name = "snorm16"; break;
                case 2: type_name = "float32"; break;
                case 3: type_name = "float16"; break;
                case 4: type_name = "ubyte"; break;
                case 5: type_name = "s16"; break;
                case 7: type_name = "ubyte256"; break;
                }
                printf("  attrib[%d]: %s x%u, stride=%u, offset=0x%X\n",
                       i, type_name, a->size, a->stride, a->offset);
            }
        }
        log_count++;
    }
}

static void d3d12_set_shader(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;

    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_shader: fp_addr=0x%08X, vp_load=%u, output_mask=0x%08X\n",
               state->fragment_program_addr, state->transform_program_load,
               state->vertex_attrib_output_mask);
        log_count++;
    }

    /* TODO: Look up or compile a PSO matching this shader combination.
     * For now we use the basic vertex-colored PSO for everything. */
}

static void d3d12_set_blend(void* ud, const rsx_state* state)
{
    (void)ud;
    /* TODO: modify PSO blend state or use dynamic state.
     * D3D12 requires PSO recreation for blend state changes,
     * so we'd need a PSO cache keyed by blend configuration. */
    static int log_count = 0;
    if (log_count < 32) {
        printf("[D3D12] set_blend(enable=%d, sfactor=0x%08X, dfactor=0x%08X, eq=0x%08X color=0x%08X)\n",
               state->blend_enable, state->blend_sfactor, state->blend_dfactor,
               state->blend_equation, state->blend_color);
        log_count++;
    }
}

static void d3d12_set_depth_stencil(void* ud, const rsx_state* state)
{
    (void)ud;
    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_depth_stencil(depth=%d, stencil=%d, func=0x%X)\n",
               state->depth_test_enable, state->stencil_test_enable,
               state->depth_func);
        log_count++;
    }
}

/* ---------------------------------------------------------------------------
 * Backend registration
 * -----------------------------------------------------------------------*/

static rsx_backend s_d3d12_backend = {0};

/* Transitional portable consumer: D3D12 still encodes its already-recorded
 * legacy draw stream on this same thread. Receiving the immutable batch here
 * makes ordering/capture validation available without changing the oracle's
 * output while individual resource paths migrate to the portable payloads. */
static int d3d12_submit_portable_batch(void* userdata,
                                       const rsx_render_batch* batch)
{
    (void)userdata;
    if (!batch) return -1;
    return 0;
}

static const char* d3d12_driver_name(void* userdata)
{
    (void)userdata;
    return "d3d12";
}

static const rsx_render_backend_ops s_d3d12_consumer = {
    .submit_batch = d3d12_submit_portable_batch,
    .driver_name = d3d12_driver_name,
};

/* ---------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

int rsx_d3d12_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_d3d, 0, sizeof(s_d3d));
    s_d3d.width = width;
    s_d3d.height = height;
    s_d3d.clear_color[0] = 0.0f;
    s_d3d.clear_color[1] = 0.0f;
    s_d3d.clear_color[2] = 0.1f;
    s_d3d.clear_color[3] = 1.0f;

    /* Debug: dump the first N presented frames to BMP if CELLMARK_DUMP is set
     * (its numeric value when > 1, else 24). */
    {
        const char* dv = getenv("CELLMARK_DUMP");
        int n = dv ? atoi(dv) : 0;
        s_d3d.dump_frames_left = dv ? (n > 1 ? n : 24) : 0;
    }

    /* Create window */
    {
        extern char g_rsx_title_base[128];
        snprintf(g_rsx_title_base, sizeof(g_rsx_title_base), "%s",
                 title ? title : "ps3recomp");
    }
    /* Wine's PeekMessage path can block for 50-200 ms while synchronizing
     * with wineserver/X11, which must never happen on the RSX/vblank timing
     * thread.  Let a dedicated owner thread create and pump the HWND; D3D12
     * device/swap-chain work remains on this thread. */
    HANDLE window_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    D3D12WindowThreadArgs window_args = { width, height, title, window_ready };
    s_window_thread = CreateThread(NULL, 0, d3d12_window_thread,
                                   &window_args, 0, NULL);
    if (s_window_thread)
        WaitForSingleObject(window_ready, INFINITE);
    CloseHandle(window_ready);
    if (!s_d3d.hwnd) {
        printf("[D3D12] ERROR: Window creation failed\n");
        if (s_window_thread) {
            WaitForSingleObject(s_window_thread, INFINITE);
            CloseHandle(s_window_thread);
            s_window_thread = NULL;
        }
        return -1;
    }

    /* Initialize D3D12 */
    if (init_d3d12(width, height) != 0) {
        printf("[D3D12] ERROR: D3D12 initialization failed\n");
        return -1;
    }

    /* Set up backend callbacks */
    s_d3d12_backend.userdata          = &s_d3d;
    s_d3d12_backend.init              = d3d12_init;
    s_d3d12_backend.shutdown          = d3d12_shutdown;
    s_d3d12_backend.begin_frame       = d3d12_begin_frame;
    s_d3d12_backend.end_frame         = d3d12_end_frame;
    s_d3d12_backend.present           = d3d12_present;
    s_d3d12_backend.clear             = d3d12_clear;
    s_d3d12_backend.set_render_target = d3d12_set_render_target;
    s_d3d12_backend.set_viewport      = d3d12_set_viewport;
    s_d3d12_backend.set_blend         = d3d12_set_blend;
    s_d3d12_backend.set_depth_stencil = d3d12_set_depth_stencil;
    s_d3d12_backend.set_shader        = d3d12_set_shader;
    s_d3d12_backend.set_vertex_attribs = d3d12_set_vertex_attribs;
    s_d3d12_backend.draw_arrays       = d3d12_draw_arrays;
    s_d3d12_backend.draw_indexed      = d3d12_draw_indexed;
    s_d3d12_backend.bind_texture      = d3d12_bind_texture;

    if (rsx_recorder_install(&s_d3d12_backend, &s_d3d12_consumer, &s_d3d) != 0) {
        fprintf(stderr, "[D3D12] ERROR: portable recorder install failed\n");
        return -1;
    }

    s_d3d.initialized = 1;
    s_d3d.last_fps_time = GetTickCount64();

    printf("[D3D12] Backend ready: %ux%u\n", width, height);
    return 0;
}

void rsx_d3d12_backend_shutdown(void)
{
    if (!s_d3d.initialized) return;

    wait_for_gpu();

    /* Release D3D12 resources */
    if (s_d3d.vertex_buffer) {
        s_d3d.vertex_buffer->lpVtbl->Unmap(s_d3d.vertex_buffer, 0, NULL);
        s_d3d.vertex_buffer->lpVtbl->Release(s_d3d.vertex_buffer);
    }
    if (s_d3d.pipeline_state)        s_d3d.pipeline_state->lpVtbl->Release(s_d3d.pipeline_state);
    if (s_d3d.pipeline_state_lines)  s_d3d.pipeline_state_lines->lpVtbl->Release(s_d3d.pipeline_state_lines);
    if (s_d3d.pipeline_state_points) s_d3d.pipeline_state_points->lpVtbl->Release(s_d3d.pipeline_state_points);
    if (s_d3d.depth_buffer) s_d3d.depth_buffer->lpVtbl->Release(s_d3d.depth_buffer);
    if (s_d3d.dsv_heap)     s_d3d.dsv_heap->lpVtbl->Release(s_d3d.dsv_heap);
    if (s_d3d.root_signature) s_d3d.root_signature->lpVtbl->Release(s_d3d.root_signature);
    if (s_d3d.fence) s_d3d.fence->lpVtbl->Release(s_d3d.fence);
    if (s_d3d.fence_event) CloseHandle(s_d3d.fence_event);
    if (s_d3d.cmd_list) s_d3d.cmd_list->lpVtbl->Release(s_d3d.cmd_list);
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        if (s_d3d.cmd_allocators[i]) s_d3d.cmd_allocators[i]->lpVtbl->Release(s_d3d.cmd_allocators[i]);
        if (s_d3d.render_targets[i]) s_d3d.render_targets[i]->lpVtbl->Release(s_d3d.render_targets[i]);
    }
    if (s_d3d.rtv_heap) s_d3d.rtv_heap->lpVtbl->Release(s_d3d.rtv_heap);
    if (s_d3d.sampler_heap) s_d3d.sampler_heap->lpVtbl->Release(s_d3d.sampler_heap);
    if (s_d3d.swap_chain) s_d3d.swap_chain->lpVtbl->Release(s_d3d.swap_chain);
    if (s_d3d.cmd_queue) s_d3d.cmd_queue->lpVtbl->Release(s_d3d.cmd_queue);
    if (s_d3d.device) s_d3d.device->lpVtbl->Release(s_d3d.device);

    if (s_d3d.hwnd) PostMessageA(s_d3d.hwnd, WM_CLOSE, 0, 0);
    if (s_window_thread) {
        WaitForSingleObject(s_window_thread, INFINITE);
        CloseHandle(s_window_thread);
        s_window_thread = NULL;
    }

    rsx_recorder_uninstall();
    s_d3d.initialized = 0;

    printf("[D3D12] Backend shut down\n");
}

int rsx_d3d12_backend_pump_messages(void)
{
    /* The dedicated HWND owner blocks in GetMessage and dispatches all input,
     * resize, close and hotkey events.  Keep this API as a cheap liveness poll
     * for the vblank ticker. */
    return s_d3d.window_closed ? -1 : 0;
}

void rsx_d3d12_backend_present(void)
{
    /* The frame driver enters here for compatibility. Route the boundary
     * through the portable recorder, which then invokes d3d12_present as its
     * inline legacy consumer. */
    rsx_backend* active = rsx_get_backend();
    if (active && active != &s_d3d12_backend && active->present) {
        extern u32 cellGcmGetCurrentDisplayBufferId(void);
        active->present(active->userdata, cellGcmGetCurrentDisplayBufferId());
        return;
    }
    if (blink_dbg()) {
        u32 display = 0, offscreen = 0, clears = 0;
        for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS; i++) {
            const D3D12DrawRecord* dr = &s_d3d.draws[i];
            if (dr->is_clear) clears++;
            else if (dr->rt_off == 0) display++;
            else offscreen++;
        }
        printf("[PRESENT] draws=%u display=%u offscreen=%u clear=%u "
               "clears_since_last=%u clear_presents=%u\n",
               s_d3d.draw_count, display, offscreen, clears,
               s_dbg_clears_since_present, s_clear_presents);
    }
    s_dbg_clears_since_present = 0;

    /* When the accumulating batch opened with a display clear, d3d12_clear
     * presents it as the drain crosses into the next frame -- the ticker
     * present would only ever show the partially-accumulated NEXT frame
     * (that partial present right after the FIFO ring recycle was the
     * visible blink). Screens without display clears (Taiko's boot test
     * screen) leave the flag 0 and present here at flip time. */
    if (s_batch_has_display_clear)
        return;

    /* Same display gate as d3d12_present: a batch of offscreen pass work only
     * (render-to-texture) keeps accumulating until its composite arrives.
     * Empty batches present only until the first real frame -- after that an
     * empty present is a flip/drain race and wipes the screen for a frame
     * (wave: black flashes and layout flicker between frames). */
    static int s_seen_content = 0;
    int has_display = (s_d3d.draw_count == 0 && !s_seen_content);
    for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++)
        if (!s_d3d.draws[_i].is_clear && s_d3d.draws[_i].rt_off == 0) {
            has_display = 1;
            break;
        }
    if (has_display && s_d3d.draw_count > 0) s_seen_content = 1;

    if (s_d3d.initialized && has_display)
        render_frame();
}

#else /* !_WIN32 */

#include <ps3emu/ps3types.h>   /* u32 (header includes above are inside the _WIN32 guard) */
#include <stdio.h>

/* Stub for non-Windows — D3D12 is Windows-only */
int rsx_d3d12_backend_init(u32 w, u32 h, const char* t)
{
    (void)w; (void)h; (void)t;
    printf("[D3D12] Not available on this platform (use Vulkan backend)\n");
    return -1;
}
void rsx_d3d12_backend_shutdown(void) {}
int rsx_d3d12_backend_pump_messages(void) { return 0; }
void rsx_d3d12_backend_present(void) {}
u32 rsx_dbg_draw_seq(void) { return 0; }
int rsx_dbg_trace_armed(void) { return 0; }
int rsx_dbg_capture_left(void) { return 0; }

#endif /* _WIN32 */

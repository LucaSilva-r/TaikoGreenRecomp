/*
 * ps3recomp - RSX FIFO parser test
 *
 * Verifies rsx_process_command_buffer method dispatch. Guest-endian words are
 * converted by vm_read32 before reaching this API in cellGcmSys. This is the core of the
 * cellGcm -> RSX bridge (cellGcmSys's gcm_consume_fifo() just hands a
 * [get,put) slice of guest memory to this function).
 *
 * No D3D12: a tiny recording backend captures the dispatched calls.
 *
 * Build:
 *   gcc -std=c11 -O2 -I../../../include ../rsx_commands.c test_rsx_fifo.c -o t.exe
 */

#include "rsx_commands.h"
#include "rsx_primitives.h"
#include <stdio.h>
#include <string.h>

/* rsx_commands.c's semaphore path writes guest labels; this parser test does
 * not exercise it, but supplies the runtime symbol for standalone linking. */
void vm_write32(u32 address, u32 value) { (void)address; (void)value; }

/* --- recording backend ------------------------------------------------- */
static int      rec_clear_calls;
static u32      rec_clear_color;
static int      rec_blend_calls;
static int      rec_blend_enable;
static int      rec_draw_calls;
static u32      rec_draw_prim, rec_draw_first, rec_draw_count;

static void rb_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{ (void)ud;(void)flags;(void)depth;(void)stencil; rec_clear_calls++; rec_clear_color = color; }
static void rb_set_blend(void* ud, const rsx_state* s)
{ (void)ud; rec_blend_calls++; rec_blend_enable = s->blend_enable; }
static void rb_draw_arrays(void* ud, u32 prim, u32 first, u32 count)
{ (void)ud; rec_draw_calls++; rec_draw_prim = prim; rec_draw_first = first; rec_draw_count = count; }
static void rb_draw_indexed(void* ud, u32 prim, u32 first, u32 count)
{ (void)ud; rec_draw_calls++; rec_draw_prim = prim; rec_draw_first = first; rec_draw_count = count; }

static rsx_backend g_rec = {0};

/* --- host-endian FIFO writer (the form supplied by vm_read32) ----------- */
static u32 g_fifo[64];
static u32 g_fifo_len;

static void put_word(u32 v)
{
    g_fifo[g_fifo_len++] = v;
}

/* One increasing method + single data word (count = 1). */
static void emit_method(u32 method, u32 data)
{
    u32 header = (1u << 18) | (((method >> 2) & 0x7FF) << 2); /* type 0, count 1 */
    put_word(header);
    put_word(data);
}

#define CHECK(cond, msg) do { \
    if (cond) { printf("[PASS] %s\n", msg); } \
    else { printf("[FAIL] %s\n", msg); failures++; } } while (0)

int main(void)
{
    int failures = 0;

    g_rec.clear        = rb_clear;
    g_rec.set_blend    = rb_set_blend;
    g_rec.draw_arrays  = rb_draw_arrays;
    g_rec.draw_indexed = rb_draw_indexed;
    rsx_set_backend(&g_rec);

    rsx_state st;
    rsx_state_init(&st);

    /* Build a small frame: set clear color, clear, enable blend, draw a tri. */
    emit_method(NV4097_SET_COLOR_CLEAR_VALUE, 0xAABBCCDD);
    emit_method(NV4097_CLEAR_SURFACE,         0xF);
    emit_method(NV4097_SET_BLEND_ENABLE,      1);
    emit_method(NV4097_SET_BEGIN_END,         RSX_PRIMITIVE_TRIANGLES);
    emit_method(NV4097_DRAW_ARRAYS,           (2u << 24) | 0u); /* count 3, first 0 */
    emit_method(NV4097_SET_BEGIN_END,         0);

    int n = rsx_process_command_buffer(&st, g_fifo, g_fifo_len * sizeof(g_fifo[0]));

    printf("methods processed: %d\n", n);
    CHECK(n == 6, "all 6 methods dispatched");
    CHECK(rec_clear_calls == 1 && rec_clear_color == 0xAABBCCDD,
          "clear dispatched with correct color");
    CHECK(rec_blend_calls == 1 && rec_blend_enable == 1,
          "set_blend flushed on BEGIN_END with enable=1");
    CHECK(rec_draw_calls == 1 && rec_draw_prim == RSX_PRIMITIVE_TRIANGLES &&
          rec_draw_first == 0 && rec_draw_count == 3,
          "draw_arrays dispatched (prim=5, first=0, count=3)");

    /* Repeated DRAW_INDEX_ARRAY words inside one BEGIN_END are one draw. */
    rec_draw_calls = 0;
    rsx_process_method(&st, NV4097_SET_RESTART_INDEX_ENABLE, 1);
    rsx_process_method(&st, NV4097_SET_RESTART_INDEX, 0xFFFF);
    rsx_process_method(&st, NV4097_SET_BEGIN_END, RSX_PRIMITIVE_TRIANGLE_STRIP);
    rsx_process_method(&st, NV4097_DRAW_INDEX_ARRAY, (255u << 24) | 0u);
    rsx_process_method(&st, NV4097_DRAW_INDEX_ARRAY, (119u << 24) | 256u);
    CHECK(rec_draw_calls == 0, "indexed ranges deferred until END");
    rsx_process_method(&st, NV4097_SET_BEGIN_END, 0);
    CHECK(rec_draw_calls == 1 && rec_draw_prim == RSX_PRIMITIVE_TRIANGLE_STRIP &&
          rec_draw_first == 0 && rec_draw_count == 376,
          "contiguous indexed ranges batched as one strip");
    CHECK(st.restart_index_enable && st.restart_index == 0xFFFF,
          "restart-index registers recorded");
    CHECK(rsx_to_d3d12_front_ccw(0x901) &&
          !rsx_to_d3d12_front_ccw(0x900),
          "RSX winding preserved after shader viewport transform");

    /* Restart splits the stream and odd triangles retain strip winding. */
    {
        static const u32 input[] = {0, 1, 2, 3, 0xFFFF, 4, 5, 6};
        static const u32 expected[][3] = {{0,1,2}, {2,1,3}, {4,5,6}};
        rsx_triangle_strip_expander strip = {{0, 0}, 0};
        u32 triangles[3][3];
        u32 produced = 0;
        for (u32 i = 0; i < sizeof(input) / sizeof(input[0]); i++) {
            u32 a, b, c;
            if (rsx_triangle_strip_push(&strip, input[i], 1, 0xFFFF,
                                        &a, &b, &c)) {
                triangles[produced][0] = a;
                triangles[produced][1] = b;
                triangles[produced][2] = c;
                produced++;
            }
        }
        CHECK(produced == 3 &&
              memcmp(triangles, expected, sizeof(expected)) == 0,
              "triangle-strip restart and alternating winding expanded");
    }

    printf("\n===========================================\n");
    printf("Results: %s (%d failure(s))\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}

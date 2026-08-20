/* Self-check for the SPURS vertex-job skinning math (src/taiko_skin.h).
 *
 *   cc -I../../src -o /tmp/test_taiko_skin test_taiko_skin.c -lm && /tmp/test_taiko_skin
 *
 * The one thing that can silently be wrong here is the matrix convention: with
 * column-vector math the translation would be read from column 3 instead of
 * row 3, which still produces plausible-looking geometry.  Case 2 pins it.
 */
#include "taiko_skin.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static void expect3(const float got[3], float x, float y, float z)
{
    assert(fabsf(got[0] - x) < 1e-5f);
    assert(fabsf(got[1] - y) < 1e-5f);
    assert(fabsf(got[2] - z) < 1e-5f);
}

int main(void)
{
    /* bone 0: identity.  bone 1: translate (10, 20, 30), row 3.
     * bone 2: scale x2, translated (0, 0, 100). */
    float m[3 * 16] = {
        1, 0, 0, 0,   0, 1, 0, 0,   0, 0, 1, 0,   0,  0,  0,  1,
        1, 0, 0, 0,   0, 1, 0, 0,   0, 0, 1, 0,  10, 20, 30,  1,
        2, 0, 0, 0,   0, 2, 0, 0,   0, 0, 2, 0,   0,  0, 100, 1,
    };
    const float pos[3] = { 1.0f, 2.0f, 3.0f };
    const float nrm[3] = { 0.0f, 0.0f, 1.0f };
    float out_pos[3], out_nrm[3];

    /* 1. identity bone, full weight -> unchanged. */
    {
        const uint32_t bone[4] = { 0, 0, 0, 0 };
        const float w[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        taiko_skin_vertex(pos, nrm, bone, w, m, 3, out_pos, out_nrm);
        expect3(out_pos, 1, 2, 3);
        expect3(out_nrm, 0, 0, 1);
    }

    /* 2. translation must come from ROW 3, and must NOT touch the normal. */
    {
        const uint32_t bone[4] = { 1, 0, 0, 0 };
        const float w[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        taiko_skin_vertex(pos, nrm, bone, w, m, 3, out_pos, out_nrm);
        expect3(out_pos, 11, 22, 33);
        expect3(out_nrm, 0, 0, 1);
    }

    /* 3. weights blend, and a zero weight contributes nothing even if its bone
     *    index is garbage. */
    {
        const uint32_t bone[4] = { 0, 2, 0xDDDDDDDDu, 0 };
        const float w[4] = { 0.25f, 0.75f, 0.0f, 0.0f };
        taiko_skin_vertex(pos, nrm, bone, w, m, 3, out_pos, out_nrm);
        /* bone 0 -> (1,2,3); bone 2 -> (2,4,106) */
        expect3(out_pos, 0.25f * 1 + 0.75f * 2,
                         0.25f * 2 + 0.75f * 4,
                         0.25f * 3 + 0.75f * 106);
        expect3(out_nrm, 0, 0, 0.25f * 1 + 0.75f * 2);
    }

    /* 4. out-of-range bone id is dropped rather than read out of bounds. */
    {
        const uint32_t bone[4] = { 99, 0, 0, 0 };
        const float w[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        taiko_skin_vertex(pos, nrm, bone, w, m, 3, out_pos, out_nrm);
        expect3(out_pos, 0, 0, 0);
    }

    printf("test_taiko_skin: ok\n");
    return 0;
}

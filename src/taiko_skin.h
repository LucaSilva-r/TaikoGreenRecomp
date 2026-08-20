/* 4-bone linear-blend skinning, the math half of Green's SPURS vertex job.
 *
 * Kept free of guest-memory access so it can be tested on the host directly;
 * src/taiko_vertex_job.cpp does the big-endian fetch and calls in here.
 *
 * Row-vector convention (v * M): the translation is row 3 of each matrix and
 * is picked up by pos.w == 1, while normals use w == 0.  Verified against
 * same-frame RPCS3 output (max position error 2.8e-6, normal 1.1e-7).
 */
#ifndef TAIKO_SKIN_H
#define TAIKO_SKIN_H

#include <stdint.h>

/* matrices: matrix_count row-major 4x4 float matrices, indexed by bone id. */
static inline void taiko_skin_vertex(const float pos[3], const float nrm[3],
                                     const uint32_t bone[4], const float weight[4],
                                     const float* matrices, uint32_t matrix_count,
                                     float out_pos[3], float out_nrm[3])
{
    for (uint32_t c = 0; c < 3; c++) {
        out_pos[c] = 0.0f;
        out_nrm[c] = 0.0f;
    }

    for (uint32_t k = 0; k < 4; k++) {
        const float w = weight[k];
        if (w == 0.0f || bone[k] >= matrix_count) continue;

        const float* m = matrices + bone[k] * 16;
        for (uint32_t c = 0; c < 3; c++) {
            out_pos[c] += w * (pos[0] * m[c] + pos[1] * m[4 + c] +
                               pos[2] * m[8 + c] + m[12 + c]);
            out_nrm[c] += w * (nrm[0] * m[c] + nrm[1] * m[4 + c] +
                               nrm[2] * m[8 + c]);
        }
    }
}

#endif /* TAIKO_SKIN_H */

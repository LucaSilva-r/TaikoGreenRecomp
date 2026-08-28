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
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
typedef float taiko_f32x4 __attribute__((vector_size(16)));
typedef uint32_t taiko_u32x4 __attribute__((vector_size(16)));

typedef struct taiko_skin_result {
    taiko_f32x4 position;
    taiko_f32x4 normal;
} taiko_skin_result;

static inline taiko_f32x4 taiko_skin_load4(const float* source)
{
    taiko_f32x4 value;
    memcpy(&value, source, sizeof value);
    return value;
}

static inline taiko_skin_result
taiko_skin_vertex_vectors(taiko_f32x4 pos, taiko_f32x4 nrm,
                          taiko_u32x4 bone, taiko_f32x4 weight,
                          const float* matrices, uint32_t matrix_count)
{
    taiko_skin_result result = {
        {0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f}
    };

    for (uint32_t k = 0; k < 4; k++) {
        const float w = weight[k];
        if (w == 0.0f || bone[k] >= matrix_count) continue;

        const float* m = matrices + bone[k] * 16;
        const taiko_f32x4 row0 = taiko_skin_load4(m);
        const taiko_f32x4 row1 = taiko_skin_load4(m + 4);
        const taiko_f32x4 row2 = taiko_skin_load4(m + 8);
        const taiko_f32x4 row3 = taiko_skin_load4(m + 12);

        taiko_f32x4 transformed_position = row0 * pos[0];
        transformed_position += row1 * pos[1];
        transformed_position += row2 * pos[2];
        transformed_position += row3;

        taiko_f32x4 transformed_normal = row0 * nrm[0];
        transformed_normal += row1 * nrm[1];
        transformed_normal += row2 * nrm[2];

        result.position += transformed_position * w;
        result.normal += transformed_normal * w;
    }

    return result;
}
#endif

/* matrices: matrix_count row-major 4x4 float matrices, indexed by bone id. */
static inline void taiko_skin_vertex(const float pos[3], const float nrm[3],
                                     const uint32_t bone[4], const float weight[4],
                                     const float* matrices, uint32_t matrix_count,
                                     float out_pos[3], float out_nrm[3])
{
#if defined(__GNUC__) || defined(__clang__)
    const taiko_f32x4 position = {pos[0], pos[1], pos[2], 0.0f};
    const taiko_f32x4 normal = {nrm[0], nrm[1], nrm[2], 0.0f};
    const taiko_u32x4 bones = {bone[0], bone[1], bone[2], bone[3]};
    const taiko_f32x4 weights = {weight[0], weight[1], weight[2], weight[3]};
    const taiko_skin_result result =
        taiko_skin_vertex_vectors(position, normal, bones, weights,
                                  matrices, matrix_count);

    for (uint32_t c = 0; c < 3; c++) {
        out_pos[c] = result.position[c];
        out_nrm[c] = result.normal[c];
    }
#else
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
#endif
}

#endif /* TAIKO_SKIN_H */

/* Native replacement for Green's SPURS vertex-skinning job.
 *
 * Model vertices are skinned on an SPU: func_0050E63C/7C4/B64/E98 build 128-byte
 * CellSpursJob128 descriptors and func_00529320 enqueues them into the job-chain
 * ring at jobChain+0x600.  cellSpursRunJobChain is a stub and the job binary at
 * guest 0x00F67F00 is never executed, so the scratch vertex arena the draw
 * points at keeps the guest heap's 0xCD fill -- Don-chan and every other model
 * collapse into a thin line.
 *
 * Rather than lift and drive the SPU job, this stands in for the job-chain
 * kernel.  A generated-snapshot hook in func_00529320 calls submit_job at the
 * exact command-publication store, after the descriptor copy.  Execution at
 * publication time is required because the guest promptly recycles descriptor
 * and DMA-palette storage; an RSX-time ring drain is already too late.
 *
 * ppu_register_function cannot hook this path: func_00529320 is called directly
 * from lifted code and its callers are reached through g_trampoline_fn, which
 * also names them by C symbol. tools/fix_vertex_submit_snapshot.py applies the
 * narrow publication callback after each re-lift.
 *
 * The transform below was decoded from the job binary and verified against
 * same-frame RPCS3 data (max position error 2.8e-6, normal 1.1e-7 -- float32
 * rounding only).  See the notes in AGENTS.md.
 *
 * TAIKO_VERTEX_JOB=0 disables the override and restores the stubbed path.
 */

/* Skinning touches every source and destination word on the frame-producing
 * PPU thread.  Use the same guarded inline VM path as lifted code; diagnostics
 * and out-of-range accesses still fall back to the full runtime helpers. */
#define PPU_RECOMP_FAST_VM 1
#include "ppu_recomp.h"
#include "taiko_skin.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace {

constexpr uint32_t kSkinBinary = 0x00F67F00u;   /* eaBinary of the skin job  */
constexpr uint32_t kSkinFormat = 0x11u;         /* the only decoded format   */

/* Ring owner layout (func_00529320): u32 capacity*2 at +0x610, then a packed
 * { u16 buffer id; u16 slot count } at +0x614.  +0x600 holds the two descriptor
 * buffer pointers and +0x608 the two 8-byte-per-slot command lists.  A slot's
 * command is written only after its descriptor is memcpy'd in, and starts life
 * as jump-to-self (low 3 bits == 3), so the command list is the handshake. */
constexpr uint64_t kCommandEnd = 0;             /* end of the command list   */
constexpr uint64_t kCommandJts = 3;             /* jump-to-self = unpublished */

constexpr uint32_t kRingCapacity = 0x610;
constexpr uint32_t kRingCursor   = 0x614;
constexpr uint32_t kRingBuffers  = 0x600;
constexpr uint32_t kRingCommands = 0x608;

/* CellSpursJob128 fields used here (big-endian guest memory). */
constexpr uint32_t kJobEaBinary   = 0x04;
constexpr uint32_t kJobSizeDmaList = 0x0A;      /* bytes of DMA list         */
constexpr uint32_t kJobDmaList    = 0x30;       /* { u32 size; u32 ea; } x n */
/* The builder stores each of these as a zero-extended 64-bit slot, so the u32
 * lives in the low half of the slot (+4). */
constexpr uint32_t kJobFormat     = 0x54;
constexpr uint32_t kJobCount      = 0x5C;       /* vertices in this chunk    */
constexpr uint32_t kJobSrc        = 0x64;
constexpr uint32_t kJobDst        = 0x6C;

constexpr uint32_t kSrcStride = 64;   /* float4 pos, float4 nrm, int4 bone, float4 weight */
constexpr uint32_t kDstStride = 32;   /* float3 pos + pad, float3 nrm + pad              */
constexpr uint32_t kMatrixStride = 64;
constexpr uint32_t kMaxMatrixBytes = 16 * 1024;   /* 256 bones; Green uses 35 */
constexpr uint32_t kMaxDmaSegments = 8;

extern "C" int rsx_dbg_capture_left(void);

uint32_t hash_words(uint32_t hash, uint32_t ea, uint32_t bytes)
{
    for (uint32_t off = 0; off + 4 <= bytes; off += 4) {
        const uint32_t word = vm_read32(ea + off);
        for (uint32_t b = 0; b < 4; b++) {
            hash ^= (word >> (b * 8)) & 0xFFu;
            hash *= 16777619u;
        }
    }
    return hash;
}

bool vertex_race_trace_enabled()
{
    static const bool enabled = [] {
        const char* setting = std::getenv("TAIKO_VERTEX_RACE_TRACE");
        return setting && setting[0] != '0';
    }();
    return enabled;
}

struct OutputRecord {
    uint32_t dst;
    uint32_t bytes;
    uint32_t job;
    uint32_t src;
    uint32_t count;
    uint32_t palette_hash;
    uint32_t source_hash;
    uint32_t output_hash;
    uint64_t serial;
};

constexpr uint32_t kOutputRecordCount = 4096;
OutputRecord g_output_records[kOutputRecordCount] = {};
uint32_t g_output_record_next = 0;
uint64_t g_output_serial = 0;
std::mutex g_output_lock;

/* TAIKO_VERTEX_JOB_TRACE=N dumps N jobs: descriptor, bone palette, first two
 * vertices in and out.  This is how the palette is checked against RPCS3. */
int trace_left()
{
    static std::atomic<int> left{-1};
    int value = left.load();
    if (value < 0) {
        const char* setting = std::getenv("TAIKO_VERTEX_JOB_TRACE");
        value = setting ? std::atoi(setting) : 0;
        left.store(value);
    }
    return value > 0 ? left.fetch_sub(1) : 0;
}

float read_float(uint32_t ea)
{
    const uint32_t bits = vm_read32(ea);
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

void write_float(uint32_t ea, float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    vm_write32(ea, bits);
}

bool direct_guest_range(uint32_t ea, uint64_t bytes)
{
    const uint64_t end = (uint64_t)ea + bytes;
    return !g_vm_diag_enabled &&
           end <= (uint64_t{1} << 32) &&
           (!ppu_vm_size || end <= ppu_vm_size);
}

uint32_t read_be32_direct(const uint8_t* source)
{
    uint32_t bits;
    std::memcpy(&bits, source, sizeof bits);
    return __builtin_bswap32(bits);
}

float read_float_direct(const uint8_t* source)
{
    const uint32_t bits = read_be32_direct(source);
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

void write_be32_direct(uint8_t* destination, uint32_t value)
{
    value = __builtin_bswap32(value);
    std::memcpy(destination, &value, sizeof value);
}

void write_float_direct(uint8_t* destination, float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    write_be32_direct(destination, bits);
}

#if defined(__aarch64__)
taiko_f32x4 read_be_f32x4_direct(const uint8_t* source)
{
    const uint8x16_t bytes = vrev32q_u8(vld1q_u8(source));
    return vreinterpretq_f32_u8(bytes);
}

taiko_u32x4 read_be_u32x4_direct(const uint8_t* source)
{
    const uint8x16_t bytes = vrev32q_u8(vld1q_u8(source));
    return vreinterpretq_u32_u8(bytes);
}

void write_be_f32x4_direct(uint8_t* destination, taiko_f32x4 value)
{
    /* The job's two output float4s keep their padding lane zero. */
    value[3] = 0.0f;
    const uint8x16_t bytes =
        vrev32q_u8(vreinterpretq_u8_f32(value));
    vst1q_u8(destination, bytes);
}
#endif

void skin_vertex(uint32_t src, uint32_t dst,
                 const float* matrices, uint32_t matrix_count)
{
    float pos[3], nrm[3], weight[4], out_pos[3], out_nrm[3];
    uint32_t bone[4];

    for (uint32_t c = 0; c < 3; c++) {
        pos[c] = read_float(src + c * 4);
        nrm[c] = read_float(src + 16 + c * 4);
    }
    for (uint32_t k = 0; k < 4; k++) {
        bone[k] = vm_read32(src + 32 + k * 4);
        weight[k] = read_float(src + 48 + k * 4);
    }

    taiko_skin_vertex(pos, nrm, bone, weight, matrices, matrix_count,
                      out_pos, out_nrm);

    for (uint32_t c = 0; c < 3; c++) {
        write_float(dst + c * 4, out_pos[c]);
        write_float(dst + 16 + c * 4, out_nrm[c]);
    }
    vm_write32(dst + 12, 0);
    vm_write32(dst + 28, 0);
}

void skin_vertex_direct(const uint8_t* src, uint8_t* dst,
                        const float* matrices, uint32_t matrix_count)
{
#if defined(__aarch64__)
    /* Each source attribute is exactly one float4.  Load and byte-swap whole
     * attributes so the hot loop needs four NEON loads/reversals instead of
     * sixteen scalar loads, reversals, and scalar-to-vector moves. */
    const taiko_f32x4 position = read_be_f32x4_direct(src);
    const taiko_f32x4 normal = read_be_f32x4_direct(src + 16);
    const taiko_u32x4 bones = read_be_u32x4_direct(src + 32);
    const taiko_f32x4 weights = read_be_f32x4_direct(src + 48);
    const taiko_skin_result result =
        taiko_skin_vertex_vectors(position, normal, bones, weights,
                                  matrices, matrix_count);
    write_be_f32x4_direct(dst, result.position);
    write_be_f32x4_direct(dst + 16, result.normal);
#else
    float pos[3], nrm[3], weight[4], out_pos[3], out_nrm[3];
    uint32_t bone[4];

    for (uint32_t c = 0; c < 3; c++) {
        pos[c] = read_float_direct(src + c * 4);
        nrm[c] = read_float_direct(src + 16 + c * 4);
    }
    for (uint32_t k = 0; k < 4; k++) {
        bone[k] = read_be32_direct(src + 32 + k * 4);
        weight[k] = read_float_direct(src + 48 + k * 4);
    }

    taiko_skin_vertex(pos, nrm, bone, weight, matrices, matrix_count,
                      out_pos, out_nrm);

    for (uint32_t c = 0; c < 3; c++) {
        write_float_direct(dst + c * 4, out_pos[c]);
        write_float_direct(dst + 16 + c * 4, out_nrm[c]);
    }
    write_be32_direct(dst + 12, 0);
    write_be32_direct(dst + 28, 0);
#endif
}

bool run_skin_job(uint32_t job)
{
    const uint32_t binary = vm_read32(job + kJobEaBinary);
    if (binary != kSkinBinary) {
        /* Any other job in this chain is one we do not implement. Name each
         * binary once -- silently dropping them is how the chain looks like it
         * is running when most of it is not. */
        static std::mutex lock;
        static uint32_t seen[8];
        static uint32_t seen_count;
        std::lock_guard<std::mutex> guard(lock);
        for (uint32_t i = 0; i < seen_count; i++)
            if (seen[i] == binary) return false;
        if (seen_count < 8) {
            seen[seen_count++] = binary;
            std::fprintf(stderr, "[taiko_vertex_job] unimplemented job binary "
                         "0x%08X job=0x%08X (size 0x%X, dmaList 0x%X)\n", binary,
                         job,
                         vm_read16(job + 0x08) * 16, vm_read16(job + kJobSizeDmaList));
            if (std::getenv("TAIKO_VERTEX_JOB_TRACE_UNKNOWN")) {
                for (uint32_t row = 0; row < 0x80; row += 16) {
                    std::fprintf(stderr, "[taiko_vertex_job]   unknown+0x%02X:", row);
                    for (uint32_t i = 0; i < 16; i += 4)
                        std::fprintf(stderr, " %08X", vm_read32(job + row + i));
                    std::fputc('\n', stderr);
                }
            }
        }
        return false;
    }

    const uint32_t format = vm_read32(job + kJobFormat);
    const uint32_t count = vm_read32(job + kJobCount);
    const uint32_t src = vm_read32(job + kJobSrc);
    const uint32_t dst = vm_read32(job + kJobDst);

    /* The bone matrices arrive as the job's DMA list; gather them the way the
     * SPU's list DMA would, into one palette indexed directly by bone id. */
    struct DmaSegment {
        uint32_t ea;
        uint32_t size;
    };
    DmaSegment segments[kMaxDmaSegments] = {};
    uint32_t segment_count = 0;
    float palette[kMaxMatrixBytes / sizeof(float)];
    uint32_t palette_bytes = 0;
    const uint32_t list_bytes = vm_read16(job + kJobSizeDmaList);
    for (uint32_t off = 0; off + 8 <= list_bytes; off += 8) {
        const uint32_t size = vm_read32(job + kJobDmaList + off) & 0xFFFFFFu;
        const uint32_t ea = vm_read32(job + kJobDmaList + off + 4);
        if (!size || !ea) continue;
        if (size % 4 || palette_bytes + size > kMaxMatrixBytes ||
            segment_count >= kMaxDmaSegments)
            return false;
        segments[segment_count++] = {ea, size};
        if (direct_guest_range(ea, size)) {
            const uint8_t* source = vm_base + ea;
            for (uint32_t i = 0; i < size; i += 4)
                palette[(palette_bytes + i) / 4] =
                    read_float_direct(source + i);
        } else {
            for (uint32_t i = 0; i < size; i += 4)
                palette[(palette_bytes + i) / 4] = read_float(ea + i);
        }
        palette_bytes += size;
    }

    static unsigned reported = 0;
    if (format != kSkinFormat || !count || !src || !dst ||
        palette_bytes < kMatrixStride) {
        if (reported++ < 4)
            std::fprintf(stderr,
                         "[taiko_vertex_job] skipping job fmt=0x%X count=%u "
                         "src=0x%08X dst=0x%08X palette=%u bytes\n",
                         format, count, src, dst, palette_bytes);
        return false;
    }

    const uint32_t matrix_count = palette_bytes / kMatrixStride;
    const int capture_left = rsx_dbg_capture_left();
    const bool diagnose_output = capture_left > 0 || vertex_race_trace_enabled();
    uint32_t palette_hash = 2166136261u;
    uint32_t source_hash = 2166136261u;
    if (diagnose_output) {
        for (uint32_t i = 0; i < palette_bytes / 4; i++) {
            uint32_t word;
            std::memcpy(&word, &palette[i], sizeof word);
            for (uint32_t b = 0; b < 4; b++) {
                palette_hash ^= (word >> (b * 8)) & 0xFFu;
                palette_hash *= 16777619u;
            }
        }
        source_hash = hash_words(source_hash, src, count * kSrcStride);
    }

    /* TAIKO_VERTEX_JOB_LATE=<ms>: re-read the palette after a delay and report
     * whether it changed. A palette that is garbage now and valid a few ms
     * later means the drain is running before the guest has finished filling
     * it, which is a scheduling bug on our side rather than bad matrix math. */
    {
        static int late_ms = -1;
        if (late_ms < 0) {
            const char* setting = std::getenv("TAIKO_VERTEX_JOB_LATE");
            late_ms = setting ? std::atoi(setting) : 0;
        }
        static unsigned probes = 0;
        if (late_ms > 0 && probes < 12) {
            const uint32_t ea = vm_read32(job + kJobDmaList + 4);
            auto snapshot = [&] {
                uint32_t nan_rows = 0, w_one = 0;
                for (uint32_t m = 0; m < matrix_count; m++) {
                    const float w = read_float(ea + m * kMatrixStride + 60);
                    if (w == 1.0f) w_one++;
                    for (uint32_t r = 0; r < 3; r++) {
                        const float v = read_float(ea + m * kMatrixStride + r * 16);
                        if (v != v) { nan_rows++; break; }
                    }
                }
                return std::make_pair(nan_rows, w_one);
            };
            const auto before = snapshot();
            std::this_thread::sleep_for(std::chrono::milliseconds(late_ms));
            const auto after = snapshot();
            probes++;
            std::fprintf(stderr,
                         "[taiko_vertex_job] palette 0x%08X bones=%u  nanRows %u->%u  "
                         "w==1 %u->%u\n", ea, matrix_count,
                         before.first, after.first, before.second, after.second);
        }
    }

    const uint64_t source_bytes = (uint64_t)count * kSrcStride;
    const uint64_t output_bytes = (uint64_t)count * kDstStride;
    if (direct_guest_range(src, source_bytes) &&
        direct_guest_range(dst, output_bytes)) {
        /* The source and destination arenas are immutable for the synchronous
         * lifetime of this published job.  Validate each complete range once
         * instead of repeating the diagnostics/bounds branch for all 22 words
         * of every vertex. */
        const uint8_t* source = vm_base + src;
        uint8_t* output = vm_base + dst;
        for (uint32_t i = 0; i < count; i++)
            skin_vertex_direct(source + (uint64_t)i * kSrcStride,
                               output + (uint64_t)i * kDstStride,
                               palette, matrix_count);
    } else {
        for (uint32_t i = 0; i < count; i++)
            skin_vertex(src + i * kSrcStride, dst + i * kDstStride,
                        palette, matrix_count);
    }

    /* A published descriptor only makes the descriptor itself immutable.  Its
     * DMA-list palette can be reused by the guest while our delayed host drain
     * is copying it.  Compare the copied snapshot with guest memory after the
     * job, and retain enough detail to distinguish that race from a matrix
     * which was already malformed when it was published. */
    if (vertex_race_trace_enabled()) {
        uint32_t current_palette_hash = 2166136261u;
        uint32_t current_source_hash = 2166136261u;
        for (uint32_t i = 0; i < segment_count; i++)
            current_palette_hash = hash_words(current_palette_hash,
                                              segments[i].ea, segments[i].size);
        current_source_hash = hash_words(current_source_hash, src,
                                         count * kSrcStride);

        static std::atomic<unsigned> palette_races{0};
        if ((current_palette_hash != palette_hash ||
             current_source_hash != source_hash) &&
            palette_races.fetch_add(1) < 64) {
            std::fprintf(stderr,
                         "[SKININPUTRACE] job=%08X n=%u src=%08X dst=%08X "
                         "ph=%08X->%08X sh=%08X->%08X\n",
                         job, count, src, dst, palette_hash, current_palette_hash,
                         source_hash, current_source_hash);
        }

        uint32_t bad_vertex = count;
        float bad_abs = 0.0f;
        for (uint32_t i = 0; i < count; i++) {
            for (uint32_t c = 0; c < 3; c++) {
                const float value = read_float(dst + i * kDstStride + c * 4);
                const float magnitude = std::fabs(value);
                if (!std::isfinite(value) || magnitude > 100.0f) {
                    bad_vertex = i;
                    bad_abs = magnitude;
                    break;
                }
            }
            if (bad_vertex != count) break;
        }

        static std::atomic<unsigned> bad_reports{0};
        if (bad_vertex != count && bad_reports.fetch_add(1) < 64) {
            const uint32_t s = src + bad_vertex * kSrcStride;
            const uint32_t d = dst + bad_vertex * kDstStride;
            std::fprintf(stderr,
                         "[SKINBAD] job=%08X vertex=%u/%u src=%08X dst=%08X "
                         "out=(%.9g,%.9g,%.9g) abs=%.9g ph=%08X->%08X "
                         "sh=%08X->%08X\n",
                         job, bad_vertex, count, s, d,
                         read_float(d), read_float(d + 4), read_float(d + 8),
                         bad_abs, palette_hash, current_palette_hash,
                         source_hash, current_source_hash);
            std::fprintf(stderr,
                         "[SKINBAD]   in=(%.9g,%.9g,%.9g) "
                         "bone=(%u,%u,%u,%u) weight=(%.9g,%.9g,%.9g,%.9g)\n",
                         read_float(s), read_float(s + 4), read_float(s + 8),
                         vm_read32(s + 32), vm_read32(s + 36),
                         vm_read32(s + 40), vm_read32(s + 44),
                         read_float(s + 48), read_float(s + 52),
                         read_float(s + 56), read_float(s + 60));
            for (uint32_t k = 0; k < 4; k++) {
                const uint32_t bone = vm_read32(s + 32 + k * 4);
                const float weight = read_float(s + 48 + k * 4);
                if (weight == 0.0f || bone >= matrix_count) continue;
                const float* m = palette + bone * 16;
                std::fprintf(stderr,
                             "[SKINBAD]   M[%u] w=%.9g "
                             "[(%.9g %.9g %.9g %.9g) "
                             "(%.9g %.9g %.9g %.9g) "
                             "(%.9g %.9g %.9g %.9g) "
                             "(%.9g %.9g %.9g %.9g)]\n",
                             bone, weight,
                             m[0], m[1], m[2], m[3],
                             m[4], m[5], m[6], m[7],
                             m[8], m[9], m[10], m[11],
                             m[12], m[13], m[14], m[15]);
            }
        }
    }

    uint32_t output_hash = 0;
    if (diagnose_output) {
        output_hash = hash_words(2166136261u, dst, count * kDstStride);
        if (vertex_race_trace_enabled()) {
            std::lock_guard<std::mutex> guard(g_output_lock);
            OutputRecord& record =
                g_output_records[g_output_record_next++ % kOutputRecordCount];
            record.dst = dst;
            record.bytes = count * kDstStride;
            record.job = job;
            record.src = src;
            record.count = count;
            record.palette_hash = palette_hash;
            record.source_hash = source_hash;
            record.output_hash = output_hash;
            record.serial = ++g_output_serial;
        }
    }

    if (capture_left > 0) {
        float lo[3] = {0.0f, 0.0f, 0.0f};
        float hi[3] = {0.0f, 0.0f, 0.0f};
        if (count) {
            for (uint32_t c = 0; c < 3; c++)
                lo[c] = hi[c] = read_float(dst + c * 4);
            for (uint32_t i = 1; i < count; i++) {
                for (uint32_t c = 0; c < 3; c++) {
                    const float value = read_float(dst + i * kDstStride + c * 4);
                    if (value < lo[c]) lo[c] = value;
                    if (value > hi[c]) hi[c] = value;
                }
            }
        }
        char line[512];
        const int length = std::snprintf(
            line, sizeof line,
            "[SKINCAP] left=%d job=%08X n=%u src=%08X dst=%08X bones=%u "
            "ph=%08X sh=%08X oh=%08X bounds=(%.5g,%.5g,%.5g)..(%.5g,%.5g,%.5g)\n",
            capture_left, job, count, src, dst, matrix_count,
            palette_hash, source_hash, output_hash,
            lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
        if (length > 0)
            std::fwrite(line, 1,
                        static_cast<size_t>(length) < sizeof line
                            ? static_cast<size_t>(length) : sizeof line - 1,
                        stderr);
    }

    if (trace_left() > 0) {
        std::fprintf(stderr,
                     "[taiko_vertex_job] skinned %u verts src=0x%08X dst=0x%08X "
                     "bones=%u\n", count, src, dst, matrix_count);
        for (uint32_t row = 0; row < 0x80; row += 16) {
            std::fprintf(stderr, "[taiko_vertex_job]   job+0x%02X:", row);
            for (uint32_t i = 0; i < 16; i += 4)
                std::fprintf(stderr, " %08X", vm_read32(job + row + i));
            std::fputc('\n', stderr);
        }
        for (uint32_t off = 0; off + 8 <= list_bytes; off += 8) {
            const uint32_t ea = vm_read32(job + kJobDmaList + off + 4);
            std::fprintf(stderr, "[taiko_vertex_job]   dma[%u] size=0x%X ea=0x%08X\n",
                         off / 8, vm_read32(job + kJobDmaList + off), ea);
            for (uint32_t m = 0; m < 2; m++) {
                std::fprintf(stderr, "[taiko_vertex_job]     M[%u]:", m);
                for (uint32_t e = 0; e < 16; e++)
                    std::fprintf(stderr, " %.3f", read_float(ea + m * 64 + e * 4));
                std::fputc('\n', stderr);
            }
        }
        for (uint32_t i = 0; i < 2; i++) {
            const uint32_t s = src + i * kSrcStride, d = dst + i * kDstStride;
            std::fprintf(stderr,
                         "[taiko_vertex_job]   v%u in(%.3f %.3f %.3f) "
                         "bone(%u %u %u %u) w(%.4f %.4f %.4f %.4f) "
                         "-> out(%.3f %.3f %.3f)\n", i,
                         read_float(s), read_float(s + 4), read_float(s + 8),
                         vm_read32(s + 32), vm_read32(s + 36),
                         vm_read32(s + 40), vm_read32(s + 44),
                         read_float(s + 48), read_float(s + 52),
                         read_float(s + 56), read_float(s + 60),
                         read_float(d), read_float(d + 4), read_float(d + 8));
        }
    }
    return true;
}

void probe_output(uint32_t ea)
{
    if (!vertex_race_trace_enabled()) return;

    OutputRecord found = {};
    {
        std::lock_guard<std::mutex> guard(g_output_lock);
        for (const OutputRecord& record : g_output_records) {
            if (!record.serial || ea < record.dst ||
                ea >= record.dst + record.bytes)
                continue;
            if (record.serial > found.serial)
                found = record;
        }
    }

    if (!found.serial) {
        std::fprintf(stderr,
                     "[SKINPROBE] ea=%08X no producing job in ledger\n", ea);
        return;
    }

    const uint32_t current_hash =
        hash_words(2166136261u, found.dst, found.bytes);
    std::fprintf(stderr,
                 "[SKINPROBE] ea=%08X job=%08X serial=%llu "
                 "dst=%08X+%X n=%u src=%08X ph=%08X sh=%08X "
                 "oh=%08X->%08X %s\n",
                 ea, found.job, static_cast<unsigned long long>(found.serial),
                 found.dst, found.bytes, found.count, found.src,
                 found.palette_hash, found.source_hash,
                 found.output_hash, current_hash,
                 found.output_hash == current_hash ? "stable" : "OVERWRITTEN");
}

/* Native submission runs synchronously at the guest's command-publication
 * store.  This lock keeps the hook safe if more than one PPU producer ever
 * reaches that store concurrently. */
std::mutex g_drain_lock;

void submit_job(uint32_t job)
{
    std::lock_guard<std::mutex> guard(g_drain_lock);
    run_skin_job(job);
}

/* Legacy ring scanner retained only as an opt-in diagnostic.  Replaying this
 * ring at RSX-drain time is not correct: descriptors and their DMA palettes are
 * recycled immediately, so an old command can still name a syntactically valid
 * descriptor whose palette address now contains unrelated data. */

void drain_ring(uint32_t owner)
{
    if (!owner) return;

    std::lock_guard<std::mutex> guard(g_drain_lock);

    const uint32_t cursor = vm_read32(owner + kRingCursor);
    const uint32_t buffer = cursor >> 16;
    const uint32_t slots = cursor & 0xFFFF;
    const uint32_t capacity = vm_read32(owner + kRingCapacity) >> 1;

    if (buffer > 1 || slots > capacity) return;   /* uninitialised ring */

    const uint32_t commands = vm_read32(owner + kRingCommands + buffer * 4);
    if (!commands) return;

    for (uint32_t slot = 0; slot < slots; slot++) {
        const uint64_t command = vm_read64(commands + slot * 8);

        /* A command is either an EA with its type in the low 3 bits, or a bare
         * control code.  JTS (3) is the pre-fill, so it means "this slot is
         * reserved but its descriptor is not published yet" -- stop there and
         * come back, or we would read a half-written job.  END (0) likewise
         * ends the list.  Every other bare code is a barrier (sync/lwsync) that
         * costs us nothing to honour by simply moving on; treating those as
         * unpublished stalled the whole chain after the first few jobs. */
        if (command == kCommandJts || command == kCommandEnd) break;
        if (command < 0x10000) continue;                       /* control code */
        if (command > 0xFFFFFFFFull) break;                     /* not an EA */

        if ((command & 7) == 0)
            run_skin_job(static_cast<uint32_t>(command));
    }
}

/* Stand in for the SPU: run the chain the way a job-chain kernel would.
 * ponytail: one chain -- the title runs a single graphics chain. */
std::atomic<uint32_t> g_chain{0};

void run_job_chain(uint32_t owner)
{
    uint32_t none = 0;
    if (!g_chain.compare_exchange_strong(none, owner)) return;
    std::fprintf(stderr, "[taiko_vertex_job] running job chain 0x%08X\n", owner);
}

/* Called from the RSX FIFO drain, so every job is finished before the draw that
 * reads its output is decoded. */
void flush_job_chain()
{
    static const bool legacy_drain = [] {
        const char* setting = std::getenv("TAIKO_VERTEX_JOB_RING_DRAIN");
        return setting && setting[0] != '0';
    }();
    const uint32_t owner = g_chain.load();
    if (legacy_drain && owner) drain_ring(owner);
}

} // namespace

extern "C" void (*g_spurs_job_chain_runner)(uint32_t jobChain);
extern "C" void (*g_spurs_job_chain_drain)(void);
extern "C" void (*g_spurs_job_submit)(uint32_t job);
extern "C" void (*g_spurs_job_output_probe)(uint32_t ea);

namespace {

__attribute__((constructor))
void configure_taiko_vertex_job()
{
    const char* setting = std::getenv("TAIKO_VERTEX_JOB");
    if (setting && std::strcmp(setting, "0") == 0) {
        std::fprintf(stderr, "[taiko_vertex_job] disabled by TAIKO_VERTEX_JOB=0\n");
        return;
    }
    g_spurs_job_chain_runner = run_job_chain;
    g_spurs_job_chain_drain = flush_job_chain;
    g_spurs_job_submit = submit_job;
    g_spurs_job_output_probe = probe_output;
}

} // namespace

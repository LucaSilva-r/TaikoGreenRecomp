#include "spu_dma.h"

#include <stdint.h>

/* Unused parts of spu_dma.h reference these runtime symbols. */
uint8_t* vm_base;
uint32_t g_taiko_audio_ring_trace_ea;
int g_cri_video_dma;
int spu_taiko_audio_ring_is_registered(uint32_t ea) { (void)ea; return 0; }
uint64_t ps3_host_monotonic_ns(void) { return 0; }
uint64_t ps3_host_monotonic_ms(void) { return 0; }

static void put_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int expect_be32(const uint8_t* p, uint32_t expected,
                       const char* field)
{
    const uint32_t actual = mfc_load_be32(p);
    if (actual == expected)
        return 1;
    fprintf(stderr, "%s: got %u, expected %u\n", field, actual, expected);
    return 0;
}

int main(void)
{
    uint32_t ea_words[9] = {0};
    uint32_t ls_words[9] = {0};
    uint8_t* ea = (uint8_t*)ea_words;
    uint8_t* ls = (uint8_t*)ls_words;

    /* The SPU snapshot knows through sequence 101. While it mixes, the PPU
     * recycles future slot 0 and publishes sequence 102. Publishing the stale
     * whole header here is the real-game race: it changes slot 0 back to 2048. */
    put_be32(ls + 0x00, 101);
    put_be32(ls + 0x04, 2048);
    put_be32(ls + 0x08, 2048);
    put_be32(ls + 0x0c, 1872);
    put_be32(ls + 0x18, 102);
    put_be32(ea + 0x00, 100);
    put_be32(ea + 0x04, 0);
    put_be32(ea + 0x08, 2048);
    put_be32(ea + 0x0c, 1700);
    put_be32(ea + 0x18, 103);

    const uint32_t preserved = mfc_publish_taiko_audio_header(ea, ls);
    if (preserved != 1u) {
        fprintf(stderr, "preserved mask: got %X, expected 1\n", preserved);
        return 1;
    }
    if (!expect_be32(ea + 0x00, 101, "consumer") ||
        !expect_be32(ea + 0x04, 0, "PPU-recycled future counter") ||
        !expect_be32(ea + 0x08, 2048, "retired counter") ||
        !expect_be32(ea + 0x0c, 1872, "active counter"))
        return 1;

    /* If the SPU snapshot contains no active slot, preserve a reset that was
     * published after its GET instead of treating the new slot as exhausted. */
    put_be32(ls + 0x00, 102);
    put_be32(ls + 0x04, 2048);
    put_be32(ls + 0x18, 102);
    put_be32(ea + 0x04, 0);
    if (mfc_publish_taiko_audio_header(ea, ls) != 1u) {
        fprintf(stderr, "late-reset mask was not reported\n");
        return 1;
    }
    if (!expect_be32(ea + 0x00, 102, "empty consumer") ||
        !expect_be32(ea + 0x04, 0, "late PPU reset"))
        return 1;

    puts("Taiko shared audio header publication: OK");
    return 0;
}

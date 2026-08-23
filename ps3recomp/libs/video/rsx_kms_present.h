#ifndef PS3RECOMP_RSX_KMS_PRESENT_H
#define PS3RECOMP_RSX_KMS_PRESENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Presentation straight to KMS, with no compositor and no Vulkan WSI in the
 * path. On the Raspberry Pi both of those sit between the finished frame and
 * the display, and V3DV lets the compositor sample a buffer whose GPU work is
 * still running -- a half-drawn frame reaches the screen as a tile-shaped
 * staircase. Driving the display ourselves removes that entire layer, and the
 * vc4 HVS scales the game's 1280x720 output up to the display mode during
 * scanout for free.
 *
 * init() takes the source size the frames will be handed in at; the mode comes
 * from TAIKOS_OUTPUT_MODE (WIDTHxHEIGHT[@RATE]) or the connector's preferred
 * mode. Returns 0 when KMS presentation is available. */
int rsx_kms_present_init(unsigned source_width, unsigned source_height);
int rsx_kms_present_active(void);

/* pixels are tightly packed R8G8B8A8 rows of the source size. The optional
 * timings split waiting for a scanout buffer, copying into it and issuing the
 * KMS plane update. They are measured here, where those operations actually
 * happen, rather than around the asynchronous GPU download submit. */
int rsx_kms_present_frame(const void *pixels, unsigned pitch,
                          uint64_t *scanout_wait_ns, uint64_t *copy_ns,
                          uint64_t *flip_ns);
void rsx_kms_present_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif

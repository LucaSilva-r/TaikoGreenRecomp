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

/* pixels are tightly packed R8G8B8A8 rows of the source size. An optional
 * opaque RGBA overlay is copied into the source-sized scanout image after the
 * frame copy; pass NULL/zeroes when unused. The optional timings split waiting
 * for a scanout buffer, copying into it and issuing the KMS plane update. They
 * are measured here, where those operations actually happen, rather than
 * around the asynchronous GPU download submit. */
int rsx_kms_present_frame(const void *pixels, unsigned pitch,
                          const void *overlay_pixels, unsigned overlay_pitch,
                          unsigned overlay_x, unsigned overlay_y,
                          unsigned overlay_width, unsigned overlay_height,
                          uint64_t *scanout_wait_ns, uint64_t *copy_ns,
                          uint64_t *flip_ns);

/* Zero-copy variant. Returns the DRM modifiers accepted by the selected KMS
 * plane for XBGR8888, preferring tiled modifiers over linear. Each imported
 * dma-buf is a 1280x720 image rendered directly by Vulkan. import_dmabuf takes
 * ownership of dma_fd. acquire_dmabuf waits until KMS has stopped
 * scanning out that slot before the renderer writes it again. */
unsigned rsx_kms_present_get_modifiers(uint64_t *modifiers,
                                       unsigned capacity);
int rsx_kms_present_import_dmabuf(unsigned index, int dma_fd,
                                  unsigned pitch, uint64_t offset,
                                  uint64_t modifier);
int rsx_kms_present_acquire_dmabuf(unsigned index, uint64_t *wait_ns);
int rsx_kms_present_dmabuf(unsigned index,
                           const void *overlay_pixels, unsigned overlay_pitch,
                           unsigned overlay_x, unsigned overlay_y,
                           unsigned overlay_width, unsigned overlay_height,
                           uint64_t *copy_ns, uint64_t *flip_ns);
void rsx_kms_present_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif

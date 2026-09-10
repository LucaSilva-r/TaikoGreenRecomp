#ifndef PS3RECOMP_RSX_HOST_FRAME_H
#define PS3RECOMP_RSX_HOST_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum HostFrameMode {
    HOST_FRAME_NONE = 0,
    HOST_FRAME_OVERLAY = 1,
    HOST_FRAME_FULLSCREEN = 2,
} HostFrameMode;

typedef struct HostFrameInfo {
    HostFrameMode mode;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t version;
} HostFrameInfo;

/* A NULL destination queries metadata. A non-NULL destination receives one
 * coherent snapshot, copied while the provider protects its mutable state. */
typedef int (*RsxHostFrameCopy)(HostFrameInfo* info, void* destination,
                                size_t destination_bytes);

extern RsxHostFrameCopy g_rsx_host_frame_copy;

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_HOST_FRAME_H */

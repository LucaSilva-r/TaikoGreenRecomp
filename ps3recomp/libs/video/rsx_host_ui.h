#ifndef RSX_HOST_UI_H
#define RSX_HOST_UI_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Synchronous render-thread visitor. Pixels are borrowed only during emit;
 * consumers must copy new textures before returning. Positions are logical
 * 1280x720 units; text payloads are rasterized at the requested pixel scale. */
typedef struct HostUiDraw {
    float x, y, w, h, radius;
    uint32_t colour;
    uint64_t texture_id; /* zero: solid rounded rectangle */
    const uint32_t* pixels;
    uint32_t width, height;
} HostUiDraw;
typedef void (*HostUiEmit)(void*, const HostUiDraw*);
typedef struct HostUiInfo {
    uint32_t version;
    int animated;
    int overlay; /* Transparent handoff over the live guest, not a host-only screen. */
} HostUiInfo;
typedef int (*HostUiVisit)(float scale, HostUiEmit emit, void* user, HostUiInfo* info);
extern HostUiVisit g_rsx_host_ui_visit;
#ifdef __cplusplus
}
#endif
#endif

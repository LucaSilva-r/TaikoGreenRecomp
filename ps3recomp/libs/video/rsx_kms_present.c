/* See rsx_kms_present.h. Legacy KMS: one modeset, then an overlay plane that
 * the display controller scales from the game's resolution to the mode. */
#include "rsx_kms_present.h"

#ifndef RSX_SDL_KMS_PRESENT
/* Built without libdrm: KMS presentation is unavailable and every entry point
 * reports that, so callers fall back to the windowed path. */
int rsx_kms_present_init(unsigned source_width, unsigned source_height)
{ (void)source_width; (void)source_height; return -1; }
int rsx_kms_present_active(void) { return 0; }
int rsx_kms_present_frame(const void *pixels, unsigned pitch)
{ (void)pixels; (void)pitch; return -1; }
void rsx_kms_present_shutdown(void) {}
#else

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define KMS_BUFFERS 3u

typedef struct kms_buffer {
    uint32_t handle;
    uint32_t pitch;
    uint32_t fb;
    uint64_t size;
    uint8_t *map;
} kms_buffer;

static struct {
    int fd;
    int active;
    int had_master;
    uint32_t crtc_id;
    uint32_t connector_id;
    uint32_t plane_id;
    drmModeModeInfo mode;
    drmModeCrtc *saved_crtc;
    unsigned width;
    unsigned height;
    kms_buffer background;
    kms_buffer buffers[KMS_BUFFERS];
    unsigned next;
    int swizzle;
    unsigned long frames;
} s_kms;

static int kms_create_buffer(uint32_t width, uint32_t height, uint32_t format,
                             kms_buffer *out)
{
    struct drm_mode_create_dumb create;
    memset(&create, 0, sizeof(create));
    create.width = width;
    create.height = height;
    create.bpp = 32;
    if (drmIoctl(s_kms.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) return -1;
    out->handle = create.handle;
    out->pitch = create.pitch;
    out->size = create.size;

    uint32_t handles[4] = {create.handle, 0, 0, 0};
    uint32_t pitches[4] = {create.pitch, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};
    if (drmModeAddFB2(s_kms.fd, width, height, format, handles, pitches,
                      offsets, &out->fb, 0) != 0)
        return -1;

    struct drm_mode_map_dumb map;
    memset(&map, 0, sizeof(map));
    map.handle = create.handle;
    if (drmIoctl(s_kms.fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) return -1;
    out->map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    s_kms.fd, map.offset);
    if (out->map == MAP_FAILED) { out->map = NULL; return -1; }
    return 0;
}

static void kms_destroy_buffer(kms_buffer *buffer)
{
    if (buffer->map) munmap(buffer->map, (size_t)buffer->size);
    if (buffer->fb) drmModeRmFB(s_kms.fd, buffer->fb);
    if (buffer->handle) {
        struct drm_mode_destroy_dumb destroy;
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = buffer->handle;
        drmIoctl(s_kms.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    memset(buffer, 0, sizeof(*buffer));
}

/* TAIKOS_OUTPUT_MODE picks the mode, so the appliance keeps one place that
 * decides what the HDMI output runs at. */
static void kms_choose_mode(drmModeConnector *connector)
{
    const char *wanted = getenv("TAIKOS_OUTPUT_MODE");
    unsigned want_w = 0, want_h = 0, want_rate = 0;
    if (wanted && sscanf(wanted, "%ux%u@%u", &want_w, &want_h, &want_rate) < 2) {
        want_w = want_h = 0;
    }
    s_kms.mode = connector->modes[0];
    for (int i = 0; i < connector->count_modes; ++i) {
        drmModeModeInfo *mode = &connector->modes[i];
        if (want_w && mode->hdisplay == want_w && mode->vdisplay == want_h &&
            (!want_rate || mode->vrefresh == want_rate)) {
            s_kms.mode = *mode;
            return;
        }
        if (!want_w && (mode->type & DRM_MODE_TYPE_PREFERRED)) s_kms.mode = *mode;
    }
}

static int kms_pick_plane(int crtc_index)
{
    drmModePlaneRes *planes = drmModeGetPlaneResources(s_kms.fd);
    if (!planes) return -1;
    uint32_t overlay = 0, primary = 0;
    for (uint32_t i = 0; i < planes->count_planes; ++i) {
        drmModePlane *plane = drmModeGetPlane(s_kms.fd, planes->planes[i]);
        if (!plane) continue;
        if (plane->possible_crtcs & (1u << crtc_index)) {
            drmModeObjectProperties *props = drmModeObjectGetProperties(
                s_kms.fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
            uint64_t type = DRM_PLANE_TYPE_OVERLAY;
            for (uint32_t p = 0; props && p < props->count_props; ++p) {
                drmModePropertyRes *property = drmModeGetProperty(s_kms.fd,
                                                                 props->props[p]);
                if (property && strcmp(property->name, "type") == 0)
                    type = props->prop_values[p];
                if (property) drmModeFreeProperty(property);
            }
            if (props) drmModeFreeObjectProperties(props);
            if (type == DRM_PLANE_TYPE_OVERLAY && !overlay) overlay = plane->plane_id;
            if (type == DRM_PLANE_TYPE_PRIMARY && !primary) primary = plane->plane_id;
        }
        drmModeFreePlane(plane);
    }
    drmModeFreePlaneResources(planes);
    /* An overlay plane leaves the primary showing the background, and on vc4
     * it is the one that scales. */
    s_kms.plane_id = overlay ? overlay : primary;
    return s_kms.plane_id ? 0 : -1;
}

int rsx_kms_present_init(unsigned source_width, unsigned source_height)
{
    if (s_kms.active) return 0;
    memset(&s_kms, 0, sizeof(s_kms));
    s_kms.fd = -1;
    s_kms.width = source_width;
    s_kms.height = source_height;

    char path[32];
    drmModeRes *resources = NULL;
    for (int card = 0; card < 4 && !resources; ++card) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", card);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmModeRes *candidate = drmModeGetResources(fd);
        if (candidate && candidate->count_connectors > 0) {
            s_kms.fd = fd;
            resources = candidate;
            break;
        }
        if (candidate) drmModeFreeResources(candidate);
        close(fd);
    }
    if (!resources) {
        fprintf(stderr, "[KMS] no display device found\n");
        return -1;
    }
    fprintf(stderr, "[KMS] using %s\n", path);

    /* Whoever held the display before us (a compositor) must be gone; without
     * master every modeset below fails. */
    s_kms.had_master = drmSetMaster(s_kms.fd) == 0;
    if (!s_kms.had_master)
        fprintf(stderr, "[KMS] drmSetMaster failed: %s\n", strerror(errno));

    drmModeConnector *connector = NULL;
    for (int i = 0; i < resources->count_connectors && !connector; ++i) {
        drmModeConnector *candidate = drmModeGetConnector(s_kms.fd,
                                                          resources->connectors[i]);
        if (candidate && candidate->connection == DRM_MODE_CONNECTED &&
            candidate->count_modes > 0)
            connector = candidate;
        else if (candidate)
            drmModeFreeConnector(candidate);
    }
    if (!connector) {
        fprintf(stderr, "[KMS] no connected connector\n");
        goto fail;
    }
    s_kms.connector_id = connector->connector_id;
    kms_choose_mode(connector);

    drmModeEncoder *encoder = drmModeGetEncoder(s_kms.fd, connector->encoder_id);
    s_kms.crtc_id = encoder ? encoder->crtc_id : resources->crtcs[0];
    int crtc_index = 0;
    for (int i = 0; i < resources->count_crtcs; ++i)
        if (resources->crtcs[i] == s_kms.crtc_id) crtc_index = i;
    if (encoder) drmModeFreeEncoder(encoder);
    s_kms.saved_crtc = drmModeGetCrtc(s_kms.fd, s_kms.crtc_id);

    /* R8G8B8A8 frames match XBGR8888 byte for byte; XRGB is the fallback and
     * costs a per-pixel swap. */
    uint32_t format = DRM_FORMAT_XBGR8888;
    if (kms_create_buffer(s_kms.width, s_kms.height, format,
                          &s_kms.buffers[0]) != 0) {
        kms_destroy_buffer(&s_kms.buffers[0]);
        format = DRM_FORMAT_XRGB8888;
        s_kms.swizzle = 1;
        if (kms_create_buffer(s_kms.width, s_kms.height, format,
                              &s_kms.buffers[0]) != 0) {
            fprintf(stderr, "[KMS] frame buffer creation failed: %s\n",
                    strerror(errno));
            goto fail;
        }
    }
    for (unsigned i = 1; i < KMS_BUFFERS; ++i)
        if (kms_create_buffer(s_kms.width, s_kms.height, format,
                              &s_kms.buffers[i]) != 0) {
            fprintf(stderr, "[KMS] frame buffer %u failed: %s\n", i,
                    strerror(errno));
            goto fail;
        }

    /* The CRTC needs a framebuffer spanning the whole mode; the scaled game
     * frame rides on the overlay plane above it. */
    if (kms_create_buffer(s_kms.mode.hdisplay, s_kms.mode.vdisplay,
                          DRM_FORMAT_XRGB8888, &s_kms.background) != 0) {
        fprintf(stderr, "[KMS] background creation failed: %s\n", strerror(errno));
        goto fail;
    }
    memset(s_kms.background.map, 0, (size_t)s_kms.background.size);
    if (drmModeSetCrtc(s_kms.fd, s_kms.crtc_id, s_kms.background.fb, 0, 0,
                       &s_kms.connector_id, 1, &s_kms.mode) != 0) {
        fprintf(stderr, "[KMS] modeset failed: %s\n", strerror(errno));
        goto fail;
    }
    if (kms_pick_plane(crtc_index) != 0) {
        fprintf(stderr, "[KMS] no usable plane for crtc %u\n", s_kms.crtc_id);
        goto fail;
    }

    fprintf(stderr, "[KMS] %ux%u -> %ux%u@%u on crtc %u plane %u%s\n",
            s_kms.width, s_kms.height, s_kms.mode.hdisplay, s_kms.mode.vdisplay,
            s_kms.mode.vrefresh, s_kms.crtc_id, s_kms.plane_id,
            s_kms.swizzle ? " (swizzled)" : "");
    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);
    s_kms.active = 1;
    return 0;

fail:
    if (connector) drmModeFreeConnector(connector);
    if (resources) drmModeFreeResources(resources);
    rsx_kms_present_shutdown();
    return -1;
}

int rsx_kms_present_active(void)
{
    return s_kms.active;
}

int rsx_kms_present_frame(const void *pixels, unsigned pitch)
{
    if (!s_kms.active || !pixels) return -1;
    kms_buffer *buffer = &s_kms.buffers[s_kms.next];
    s_kms.next = (s_kms.next + 1u) % KMS_BUFFERS;

    const uint8_t *source = (const uint8_t *)pixels;
    for (unsigned y = 0; y < s_kms.height; ++y) {
        uint8_t *destination = buffer->map + (size_t)y * buffer->pitch;
        const uint8_t *row = source + (size_t)y * pitch;
        if (!s_kms.swizzle) {
            memcpy(destination, row, (size_t)s_kms.width * 4u);
            continue;
        }
        for (unsigned x = 0; x < s_kms.width; ++x) {
            destination[x * 4 + 0] = row[x * 4 + 2];
            destination[x * 4 + 1] = row[x * 4 + 1];
            destination[x * 4 + 2] = row[x * 4 + 0];
            destination[x * 4 + 3] = 0xff;
        }
    }

    if (drmModeSetPlane(s_kms.fd, s_kms.plane_id, s_kms.crtc_id, buffer->fb, 0,
                        0, 0, s_kms.mode.hdisplay, s_kms.mode.vdisplay,
                        0, 0, s_kms.width << 16, s_kms.height << 16) != 0) {
        static int reported;
        if (!reported++)
            fprintf(stderr, "[KMS] SetPlane failed: %s\n", strerror(errno));
        return -1;
    }
    ++s_kms.frames;
    return 0;
}

void rsx_kms_present_shutdown(void)
{
    if (s_kms.fd < 0) return;
    if (s_kms.saved_crtc) {
        drmModeSetCrtc(s_kms.fd, s_kms.saved_crtc->crtc_id,
                       s_kms.saved_crtc->buffer_id, s_kms.saved_crtc->x,
                       s_kms.saved_crtc->y, &s_kms.connector_id, 1,
                       &s_kms.saved_crtc->mode);
        drmModeFreeCrtc(s_kms.saved_crtc);
        s_kms.saved_crtc = NULL;
    }
    for (unsigned i = 0; i < KMS_BUFFERS; ++i) kms_destroy_buffer(&s_kms.buffers[i]);
    kms_destroy_buffer(&s_kms.background);
    if (s_kms.had_master) drmDropMaster(s_kms.fd);
    close(s_kms.fd);
    s_kms.fd = -1;
    s_kms.active = 0;
}

#endif /* RSX_SDL_KMS_PRESENT */

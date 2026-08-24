/* See rsx_kms_present.h. Legacy KMS: one modeset, then an overlay plane that
 * the display controller scales from the game's resolution to the mode. */
#include "rsx_kms_present.h"

#ifndef RSX_SDL_KMS_PRESENT
/* Built without libdrm: KMS presentation is unavailable and every entry point
 * reports that, so callers fall back to the windowed path. */
int rsx_kms_present_init(unsigned source_width, unsigned source_height)
{ (void)source_width; (void)source_height; return -1; }
int rsx_kms_present_active(void) { return 0; }
int rsx_kms_present_frame(const void *pixels, unsigned pitch,
                          const void *overlay_pixels, unsigned overlay_pitch,
                          unsigned overlay_x, unsigned overlay_y,
                          unsigned overlay_width, unsigned overlay_height,
                          uint64_t *scanout_wait_ns, uint64_t *copy_ns,
                          uint64_t *flip_ns)
{
    (void)pixels; (void)pitch;
    (void)overlay_pixels; (void)overlay_pitch;
    (void)overlay_x; (void)overlay_y;
    (void)overlay_width; (void)overlay_height;
    if (scanout_wait_ns) *scanout_wait_ns = 0;
    if (copy_ns) *copy_ns = 0;
    if (flip_ns) *flip_ns = 0;
    return -1;
}
unsigned rsx_kms_present_get_modifiers(uint64_t *modifiers,
                                       unsigned capacity)
{ (void)modifiers; (void)capacity; return 0; }
int rsx_kms_present_import_dmabuf(unsigned index, int dma_fd,
                                  unsigned pitch, uint64_t offset,
                                  uint64_t modifier)
{ (void)index; (void)dma_fd; (void)pitch; (void)offset; (void)modifier; return -1; }
int rsx_kms_present_acquire_dmabuf(unsigned index, uint64_t *wait_ns)
{ (void)index; if (wait_ns) *wait_ns = 0; return -1; }
int rsx_kms_present_dmabuf(unsigned index,
                           const void *overlay_pixels, unsigned overlay_pitch,
                           unsigned overlay_x, unsigned overlay_y,
                           unsigned overlay_width, unsigned overlay_height,
                           uint64_t *copy_ns, uint64_t *flip_ns)
{
    (void)index; (void)overlay_pixels; (void)overlay_pitch;
    (void)overlay_x; (void)overlay_y;
    (void)overlay_width; (void)overlay_height;
    if (copy_ns) *copy_ns = 0;
    if (flip_ns) *flip_ns = 0;
    return -1;
}
void rsx_kms_present_shutdown(void) {}
#else

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define KMS_BUFFERS 3u

typedef struct kms_buffer {
    uint32_t handle;
    uint32_t pitch;
    uint32_t fb;
    uint64_t size;
    uint64_t data_offset;
    uint8_t *map;
    int prime_fd;
    int release_fd;
    int imported;
} kms_buffer;

typedef struct kms_plane_properties {
    uint32_t fb_id;
    uint32_t crtc_id;
    uint32_t crtc_x;
    uint32_t crtc_y;
    uint32_t crtc_w;
    uint32_t crtc_h;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t src_w;
    uint32_t src_h;
} kms_plane_properties;

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
    int displayed;
    int pending_fence_fd;
    int swizzle;
    int atomic;
    int atomic_wait;
    int external_active;
    uint32_t crtc_out_fence_ptr;
    kms_plane_properties plane_props;
    unsigned long frames;
} s_kms;

static uint64_t kms_monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static int kms_create_buffer(uint32_t width, uint32_t height, uint32_t format,
                             kms_buffer *out)
{
    out->prime_fd = -1;
    out->release_fd = -1;
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
    /* The display controller and CPU share this allocation. Exporting its GEM
     * handle gives us DMA_BUF_IOCTL_SYNC, the kernel-defined cache/coherency
     * boundary for CPU writes before scanout. Without it, a memory-pressure
     * pulse can expose rows from a still-draining memcpy on the physical
     * panel even though every render/download fence has completed. */
    if (drmPrimeHandleToFD(s_kms.fd, out->handle,
                           DRM_CLOEXEC | DRM_RDWR, &out->prime_fd) != 0)
        out->prime_fd = -1;
    return 0;
}

static void kms_destroy_buffer(kms_buffer *buffer)
{
    if (buffer->release_fd >= 0) close(buffer->release_fd);
    if (buffer->prime_fd >= 0) close(buffer->prime_fd);
    if (buffer->map) munmap(buffer->map, (size_t)buffer->size);
    if (buffer->fb) drmModeRmFB(s_kms.fd, buffer->fb);
    if (buffer->handle && buffer->imported) {
        struct drm_gem_close close_request;
        memset(&close_request, 0, sizeof(close_request));
        close_request.handle = buffer->handle;
        drmIoctl(s_kms.fd, DRM_IOCTL_GEM_CLOSE, &close_request);
    } else if (buffer->handle) {
        struct drm_mode_destroy_dumb destroy;
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = buffer->handle;
        drmIoctl(s_kms.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    memset(buffer, 0, sizeof(*buffer));
    buffer->prime_fd = -1;
    buffer->release_fd = -1;
}

static int kms_import_dmabuf(int dma_fd, unsigned pitch, uint64_t offset,
                             uint64_t modifier, kms_buffer *out)
{
    const uint64_t image_bytes = (uint64_t)pitch * s_kms.height;
    if (offset > UINT32_MAX || image_bytes > UINT64_MAX - offset ||
        offset + image_bytes > SIZE_MAX) {
        close(dma_fd);
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->prime_fd = dma_fd;
    out->release_fd = -1;
    if (drmPrimeFDToHandle(s_kms.fd, dma_fd, &out->handle) != 0) {
        kms_destroy_buffer(out);
        return -1;
    }
    out->pitch = pitch;
    out->size = offset + image_bytes;
    out->data_offset = offset;
    out->imported = 1;
    uint32_t handles[4] = {out->handle, 0, 0, 0};
    uint32_t pitches[4] = {pitch, 0, 0, 0};
    uint32_t offsets[4] = {(uint32_t)offset, 0, 0, 0};
    uint64_t modifiers[4] = {modifier, 0, 0, 0};
    const uint32_t flags = modifier != DRM_FORMAT_MOD_LINEAR
        ? DRM_MODE_FB_MODIFIERS : 0;
    if (drmModeAddFB2WithModifiers(
            s_kms.fd, s_kms.width, s_kms.height, DRM_FORMAT_XBGR8888,
            handles, pitches, offsets, modifiers, &out->fb, flags) != 0) {
        kms_destroy_buffer(out);
        return -1;
    }
    out->map = mmap(NULL, (size_t)out->size, PROT_READ | PROT_WRITE,
                    MAP_SHARED, dma_fd, 0);
    if (out->map == MAP_FAILED) out->map = NULL;
    return 0;
}

static int kms_cpu_sync(kms_buffer *buffer, uint64_t flags)
{
    if (buffer->prime_fd < 0) return -1;
    struct dma_buf_sync sync;
    memset(&sync, 0, sizeof(sync));
    sync.flags = flags;
    int result;
    do {
        result = ioctl(buffer->prime_fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (result != 0 && errno == EINTR);
    return result;
}

static int kms_wait_fence(int *fence_fd)
{
    if (!fence_fd || *fence_fd < 0) return 0;
    struct pollfd poll_fd;
    poll_fd.fd = *fence_fd;
    poll_fd.events = POLLIN;
    poll_fd.revents = 0;
    int result;
    do {
        result = poll(&poll_fd, 1, -1);
    } while (result < 0 && errno == EINTR);
    close(*fence_fd);
    *fence_fd = -1;
    return result < 0 ? -1 : 0;
}

static uint32_t kms_find_property(uint32_t object_id, uint32_t object_type,
                                  const char *name)
{
    drmModeObjectProperties *properties = drmModeObjectGetProperties(
        s_kms.fd, object_id, object_type);
    if (!properties) return 0;
    uint32_t result = 0;
    for (uint32_t i = 0; i < properties->count_props && !result; ++i) {
        drmModePropertyRes *property = drmModeGetProperty(s_kms.fd,
                                                          properties->props[i]);
        if (property && strcmp(property->name, name) == 0)
            result = property->prop_id;
        if (property) drmModeFreeProperty(property);
    }
    drmModeFreeObjectProperties(properties);
    return result;
}

static int kms_load_atomic_properties(void)
{
#define LOAD_PLANE_PROPERTY(field, name)                                      \
    do {                                                                       \
        s_kms.plane_props.field = kms_find_property(                           \
            s_kms.plane_id, DRM_MODE_OBJECT_PLANE, name);                     \
        if (!s_kms.plane_props.field) return -1;                               \
    } while (0)
    LOAD_PLANE_PROPERTY(fb_id, "FB_ID");
    LOAD_PLANE_PROPERTY(crtc_id, "CRTC_ID");
    LOAD_PLANE_PROPERTY(crtc_x, "CRTC_X");
    LOAD_PLANE_PROPERTY(crtc_y, "CRTC_Y");
    LOAD_PLANE_PROPERTY(crtc_w, "CRTC_W");
    LOAD_PLANE_PROPERTY(crtc_h, "CRTC_H");
    LOAD_PLANE_PROPERTY(src_x, "SRC_X");
    LOAD_PLANE_PROPERTY(src_y, "SRC_Y");
    LOAD_PLANE_PROPERTY(src_w, "SRC_W");
    LOAD_PLANE_PROPERTY(src_h, "SRC_H");
#undef LOAD_PLANE_PROPERTY
    s_kms.crtc_out_fence_ptr = kms_find_property(
        s_kms.crtc_id, DRM_MODE_OBJECT_CRTC, "OUT_FENCE_PTR");
    return s_kms.crtc_out_fence_ptr ? 0 : -1;
}

static int kms_atomic_add(drmModeAtomicReq *request, uint32_t object,
                          uint32_t property, uint64_t value)
{
    return drmModeAtomicAddProperty(request, object, property, value) < 0
        ? -1 : 0;
}

static int kms_atomic_set_plane(unsigned buffer_index, uint64_t *wait_ns)
{
    kms_buffer *buffer = &s_kms.buffers[buffer_index];
    drmModeAtomicReq *request = drmModeAtomicAlloc();
    if (!request) return -1;
    int out_fence_fd = -1;
    const kms_plane_properties *p = &s_kms.plane_props;
    int failed =
        kms_atomic_add(request, s_kms.plane_id, p->fb_id, buffer->fb) ||
        kms_atomic_add(request, s_kms.plane_id, p->crtc_id, s_kms.crtc_id) ||
        kms_atomic_add(request, s_kms.plane_id, p->crtc_x, 0) ||
        kms_atomic_add(request, s_kms.plane_id, p->crtc_y, 0) ||
        kms_atomic_add(request, s_kms.plane_id, p->crtc_w,
                       s_kms.mode.hdisplay) ||
        kms_atomic_add(request, s_kms.plane_id, p->crtc_h,
                       s_kms.mode.vdisplay) ||
        kms_atomic_add(request, s_kms.plane_id, p->src_x, 0) ||
        kms_atomic_add(request, s_kms.plane_id, p->src_y, 0) ||
        kms_atomic_add(request, s_kms.plane_id, p->src_w,
                       (uint64_t)s_kms.width << 16) ||
        kms_atomic_add(request, s_kms.plane_id, p->src_h,
                       (uint64_t)s_kms.height << 16) ||
        kms_atomic_add(request, s_kms.crtc_id, s_kms.crtc_out_fence_ptr,
                       (uint64_t)(uintptr_t)&out_fence_fd);
    int result = -1;
    if (!failed) {
        result = drmModeAtomicCommit(s_kms.fd, request,
                                     DRM_MODE_ATOMIC_NONBLOCK, NULL);
        if (result != 0 && errno == EBUSY && s_kms.pending_fence_fd >= 0) {
            /* vc4 permits one pending nonblocking update. A cold shader burst
             * can catch up fast enough to reach the next commit before the
             * previous vblank; its out-fence is the precise retry boundary. */
            if (out_fence_fd >= 0) {
                close(out_fence_fd);
                out_fence_fd = -1;
            }
            const uint64_t wait_start = kms_monotonic_ns();
            if (kms_wait_fence(&s_kms.pending_fence_fd) == 0)
                result = drmModeAtomicCommit(s_kms.fd, request,
                                             DRM_MODE_ATOMIC_NONBLOCK, NULL);
            const uint64_t wait_end = kms_monotonic_ns();
            if (wait_ns && wait_end >= wait_start)
                *wait_ns += wait_end - wait_start;
        }
    }
    drmModeAtomicFree(request);
    if (result != 0) {
        if (out_fence_fd >= 0) close(out_fence_fd);
        return -1;
    }

    /* The new commit's out-fence is the release fence for the buffer it
     * replaces. Keep a duplicate as the most recent commit fence so an EBUSY
     * fallback can quiesce KMS before returning to the legacy ioctl. */
    int pending_fence_fd = out_fence_fd >= 0 ? dup(out_fence_fd) : -1;
    if (s_kms.displayed >= 0) {
        kms_buffer *old = &s_kms.buffers[s_kms.displayed];
        if (old->release_fd >= 0) close(old->release_fd);
        old->release_fd = out_fence_fd;
    } else if (out_fence_fd >= 0) {
        close(out_fence_fd);
    }
    if (s_kms.pending_fence_fd >= 0) close(s_kms.pending_fence_fd);
    s_kms.pending_fence_fd = pending_fence_fd;
    s_kms.displayed = (int)buffer_index;
    return 0;
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
    s_kms.displayed = -1;
    s_kms.pending_fence_fd = -1;
    s_kms.background.prime_fd = -1;
    s_kms.background.release_fd = -1;
    for (unsigned i = 0; i < KMS_BUFFERS; ++i) {
        s_kms.buffers[i].prime_fd = -1;
        s_kms.buffers[i].release_fd = -1;
    }
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

    const int zero_copy = getenv("TAIKO_KMS_ZERO_COPY") != NULL;
    const int zero_copy_atomic =
        getenv("TAIKO_KMS_ZERO_COPY_ATOMIC") != NULL;
    if (getenv("TAIKO_KMS_ATOMIC") && (!zero_copy || zero_copy_atomic)) {
        if (drmSetClientCap(s_kms.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0 &&
            drmSetClientCap(s_kms.fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0) {
            s_kms.atomic = 1;
            s_kms.atomic_wait = getenv("TAIKO_KMS_ATOMIC_WAIT") != NULL;
        } else {
            fprintf(stderr, "[KMS] atomic client capability unavailable: %s; "
                            "using legacy plane updates\n", strerror(errno));
        }
    } else if (getenv("TAIKO_KMS_ATOMIC") && zero_copy) {
        fprintf(stderr,
                "[KMS] zero-copy uses legacy plane updates; "
                "TAIKO_KMS_ZERO_COPY_ATOMIC=1 re-enables atomic diagnosis\n");
    }

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
    int dma_sync_available = 1;
    for (unsigned i = 0; i < KMS_BUFFERS; ++i)
        if (s_kms.buffers[i].prime_fd < 0) dma_sync_available = 0;
    fprintf(stderr, "[KMS] CPU write synchronization: %s\n",
            dma_sync_available ? "DMA-BUF" : "msync fallback");

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
    if (s_kms.atomic && kms_load_atomic_properties() != 0) {
        fprintf(stderr, "[KMS] atomic plane properties incomplete; "
                        "using legacy plane updates\n");
        s_kms.atomic = 0;
    }

    const char *update_mode = s_kms.atomic
        ? (s_kms.atomic_wait ? "atomic vblank-wait" : "atomic nonblocking")
        : "legacy blocking";
    fprintf(stderr, "[KMS] %ux%u -> %ux%u@%u on crtc %u plane %u%s (%s)\n",
            s_kms.width, s_kms.height, s_kms.mode.hdisplay, s_kms.mode.vdisplay,
            s_kms.mode.vrefresh, s_kms.crtc_id, s_kms.plane_id,
            s_kms.swizzle ? " (swizzled)" : "",
            update_mode);
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

unsigned rsx_kms_present_get_modifiers(uint64_t *modifiers,
                                       unsigned capacity)
{
    if (!s_kms.active || !modifiers || capacity == 0) return 0;
    drmModeObjectProperties *object = drmModeObjectGetProperties(
        s_kms.fd, s_kms.plane_id, DRM_MODE_OBJECT_PLANE);
    if (!object) return 0;
    uint32_t blob_id = 0;
    for (uint32_t i = 0; i < object->count_props; ++i) {
        drmModePropertyRes *property = drmModeGetProperty(
            s_kms.fd, object->props[i]);
        if (property && strcmp(property->name, "IN_FORMATS") == 0)
            blob_id = (uint32_t)object->prop_values[i];
        if (property) drmModeFreeProperty(property);
    }
    drmModeFreeObjectProperties(object);
    if (!blob_id) return 0;

    drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(s_kms.fd, blob_id);
    if (!blob || blob->length < sizeof(struct drm_format_modifier_blob)) {
        if (blob) drmModeFreePropertyBlob(blob);
        return 0;
    }
    const uint8_t *data = (const uint8_t *)blob->data;
    const struct drm_format_modifier_blob *header =
        (const struct drm_format_modifier_blob *)data;
    const uint64_t formats_end = (uint64_t)header->formats_offset +
        (uint64_t)header->count_formats * sizeof(uint32_t);
    const uint64_t modifiers_end = (uint64_t)header->modifiers_offset +
        (uint64_t)header->count_modifiers * sizeof(struct drm_format_modifier);
    if (formats_end > blob->length || modifiers_end > blob->length) {
        drmModeFreePropertyBlob(blob);
        return 0;
    }
    const uint32_t *formats =
        (const uint32_t *)(data + header->formats_offset);
    const struct drm_format_modifier *available =
        (const struct drm_format_modifier *)(data + header->modifiers_offset);

    unsigned count = 0;
    /* Keep linear as the last fallback so Vulkan chooses a native tiled
     * layout whenever VC4 and V3DV share one. */
    for (unsigned pass = 0; pass < 2 && count < capacity; ++pass) {
        for (uint32_t f = 0; f < header->count_formats && count < capacity; ++f) {
            if (formats[f] != DRM_FORMAT_XBGR8888) continue;
            for (uint32_t m = 0; m < header->count_modifiers && count < capacity; ++m) {
                const struct drm_format_modifier *candidate = &available[m];
                if (f < candidate->offset || f >= candidate->offset + 64u ||
                    !(candidate->formats & (1ULL << (f - candidate->offset))))
                    continue;
                const int linear = candidate->modifier == DRM_FORMAT_MOD_LINEAR;
                if ((pass == 0 && linear) || (pass == 1 && !linear)) continue;
                int duplicate = 0;
                for (unsigned i = 0; i < count; ++i)
                    if (modifiers[i] == candidate->modifier) duplicate = 1;
                if (!duplicate) modifiers[count++] = candidate->modifier;
            }
        }
    }
    drmModeFreePropertyBlob(blob);
    fprintf(stderr, "[KMS] XBGR8888 scanout modifiers:");
    for (unsigned i = 0; i < count; ++i)
        fprintf(stderr, " %#llx", (unsigned long long)modifiers[i]);
    fprintf(stderr, "\n");
    return count;
}

int rsx_kms_present_import_dmabuf(unsigned index, int dma_fd,
                                  unsigned pitch, uint64_t offset,
                                  uint64_t modifier)
{
    if (dma_fd < 0) return -1;
    if (!s_kms.active || index >= KMS_BUFFERS) {
        close(dma_fd);
        return -1;
    }
    if (!s_kms.external_active) {
        kms_wait_fence(&s_kms.pending_fence_fd);
        for (unsigned i = 0; i < KMS_BUFFERS; ++i)
            kms_destroy_buffer(&s_kms.buffers[i]);
        s_kms.displayed = -1;
        s_kms.next = 0;
        s_kms.external_active = 1;
    }
    if (s_kms.buffers[index].handle)
        kms_destroy_buffer(&s_kms.buffers[index]);
    if (kms_import_dmabuf(dma_fd, pitch, offset, modifier,
                          &s_kms.buffers[index]) != 0) {
        fprintf(stderr, "[KMS] dma-buf %u import failed: %s\n",
                index, strerror(errno));
        return -1;
    }
    fprintf(stderr,
            "[KMS] dma-buf %u imported pitch=%u offset=%llu modifier=%#llx "
            "CPU-map=%s\n",
            index, pitch, (unsigned long long)offset,
            (unsigned long long)modifier,
            s_kms.buffers[index].map ? "yes" : "no");
    return 0;
}

int rsx_kms_present_acquire_dmabuf(unsigned index, uint64_t *wait_ns)
{
    if (wait_ns) *wait_ns = 0;
    if (!s_kms.active || !s_kms.external_active || index >= KMS_BUFFERS ||
        !s_kms.buffers[index].fb)
        return -1;
    const uint64_t start = kms_monotonic_ns();
    const int result = kms_wait_fence(&s_kms.buffers[index].release_fd);
    const uint64_t end = kms_monotonic_ns();
    if (wait_ns && end >= start) *wait_ns = end - start;
    return result;
}

int rsx_kms_present_dmabuf(unsigned index,
                           const void *overlay_pixels, unsigned overlay_pitch,
                           unsigned overlay_x, unsigned overlay_y,
                           unsigned overlay_width, unsigned overlay_height,
                           uint64_t *copy_ns, uint64_t *flip_ns)
{
    if (copy_ns) *copy_ns = 0;
    if (flip_ns) *flip_ns = 0;
    if (!s_kms.active || !s_kms.external_active || index >= KMS_BUFFERS ||
        !s_kms.buffers[index].fb)
        return -1;
    kms_buffer *buffer = &s_kms.buffers[index];
    if (overlay_pixels && overlay_pitch && buffer->map &&
        overlay_x < s_kms.width && overlay_y < s_kms.height) {
        const uint64_t copy_start = kms_monotonic_ns();
        unsigned copy_width = overlay_width;
        unsigned copy_height = overlay_height;
        if (copy_width > s_kms.width - overlay_x)
            copy_width = s_kms.width - overlay_x;
        if (copy_height > s_kms.height - overlay_y)
            copy_height = s_kms.height - overlay_y;
        const int dma_sync = kms_cpu_sync(
            buffer, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) == 0;
        const uint8_t *overlay = (const uint8_t *)overlay_pixels;
        for (unsigned y = 0; y < copy_height; ++y) {
            uint8_t *destination = buffer->map + buffer->data_offset +
                (size_t)(overlay_y + y) * buffer->pitch +
                (size_t)overlay_x * 4u;
            memcpy(destination, overlay + (size_t)y * overlay_pitch,
                   (size_t)copy_width * 4u);
        }
        if (dma_sync)
            kms_cpu_sync(buffer, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        else
            msync(buffer->map, (size_t)buffer->size, MS_SYNC);
        __sync_synchronize();
        const uint64_t copy_end = kms_monotonic_ns();
        if (copy_ns && copy_end >= copy_start)
            *copy_ns = copy_end - copy_start;
    }
    const uint64_t start = kms_monotonic_ns();
    int result = s_kms.atomic ? kms_atomic_set_plane(index, NULL) :
        drmModeSetPlane(s_kms.fd, s_kms.plane_id, s_kms.crtc_id,
                        s_kms.buffers[index].fb, 0,
                        0, 0, s_kms.mode.hdisplay, s_kms.mode.vdisplay,
                        0, 0, s_kms.width << 16, s_kms.height << 16);
    if (result != 0 && s_kms.atomic) {
        const int atomic_errno = errno;
        kms_wait_fence(&s_kms.pending_fence_fd);
        fprintf(stderr, "[KMS] atomic dma-buf commit failed: %s; "
                        "falling back to legacy updates\n",
                strerror(atomic_errno));
        s_kms.atomic = 0;
        result = drmModeSetPlane(
            s_kms.fd, s_kms.plane_id, s_kms.crtc_id,
            s_kms.buffers[index].fb, 0,
            0, 0, s_kms.mode.hdisplay, s_kms.mode.vdisplay,
            0, 0, s_kms.width << 16, s_kms.height << 16);
    }
    if (result == 0 && s_kms.atomic && s_kms.atomic_wait)
        result = kms_wait_fence(&s_kms.pending_fence_fd);
    const uint64_t end = kms_monotonic_ns();
    if (flip_ns && end >= start) *flip_ns = end - start;
    if (result == 0) ++s_kms.frames;
    return result;
}

int rsx_kms_present_frame(const void *pixels, unsigned pitch,
                          const void *overlay_pixels, unsigned overlay_pitch,
                          unsigned overlay_x, unsigned overlay_y,
                          unsigned overlay_width, unsigned overlay_height,
                          uint64_t *scanout_wait_ns, uint64_t *copy_ns,
                          uint64_t *flip_ns)
{
    if (scanout_wait_ns) *scanout_wait_ns = 0;
    if (copy_ns) *copy_ns = 0;
    if (flip_ns) *flip_ns = 0;
    if (!s_kms.active || !pixels) return -1;
    const unsigned buffer_index = s_kms.next;
    kms_buffer *buffer = &s_kms.buffers[buffer_index];
    s_kms.next = (s_kms.next + 1u) % KMS_BUFFERS;

    const uint64_t wait_start = kms_monotonic_ns();
    if (kms_wait_fence(&buffer->release_fd) != 0) {
        static int reported;
        if (!reported++)
            fprintf(stderr, "[KMS] scanout fence wait failed: %s\n",
                    strerror(errno));
        return -1;
    }
    const uint64_t wait_end = kms_monotonic_ns();
    if (scanout_wait_ns && wait_end >= wait_start)
        *scanout_wait_ns = wait_end - wait_start;

    const uint64_t copy_start = wait_end;
    const int dma_sync = kms_cpu_sync(
        buffer, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) == 0;
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
    /* Small opaque diagnostics belong in the copy we already have to perform,
     * not in a separate GPU render pass. On V3D even a 128x40 quad can make a
     * 1280x720 target pay another complete tile load/store. The FPS badge is
     * black and white, so its byte order is unchanged by the optional R/B
     * swizzle. Clipping keeps this helper generally safe for source-sized
     * overlays without turning the full-frame memcpy into a per-pixel loop. */
    if (overlay_pixels && overlay_pitch && overlay_x < s_kms.width &&
        overlay_y < s_kms.height) {
        unsigned copy_width = overlay_width;
        unsigned copy_height = overlay_height;
        if (copy_width > s_kms.width - overlay_x)
            copy_width = s_kms.width - overlay_x;
        if (copy_height > s_kms.height - overlay_y)
            copy_height = s_kms.height - overlay_y;
        const uint8_t *overlay = (const uint8_t *)overlay_pixels;
        for (unsigned y = 0; y < copy_height; ++y) {
            uint8_t *destination = buffer->map +
                (size_t)(overlay_y + y) * buffer->pitch +
                (size_t)overlay_x * 4u;
            memcpy(destination, overlay + (size_t)y * overlay_pitch,
                   (size_t)copy_width * 4u);
        }
    }
    if (dma_sync) {
        if (kms_cpu_sync(buffer, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) != 0) {
            static int reported;
            if (!reported++)
                fprintf(stderr, "[KMS] DMA-BUF CPU sync failed: %s\n",
                        strerror(errno));
        }
    } else {
        /* PRIME export is optional on some DRM drivers. msync is the best
         * available completion boundary for their shared dumb mappings. */
        msync(buffer->map, (size_t)buffer->size, MS_SYNC);
    }
    __sync_synchronize();
    const uint64_t copy_end = kms_monotonic_ns();
    if (copy_ns && copy_end >= copy_start) *copy_ns = copy_end - copy_start;

    const uint64_t flip_start = copy_end;
    int result = s_kms.atomic ? kms_atomic_set_plane(buffer_index,
                                                     scanout_wait_ns) :
        drmModeSetPlane(s_kms.fd, s_kms.plane_id, s_kms.crtc_id, buffer->fb, 0,
                        0, 0, s_kms.mode.hdisplay, s_kms.mode.vdisplay,
                        0, 0, s_kms.width << 16, s_kms.height << 16);
    if (result != 0 && s_kms.atomic) {
        const int atomic_errno = errno;
        /* Never mix an in-flight atomic update with a legacy update. The most
         * recent out-fence makes the transition explicit and safe. */
        kms_wait_fence(&s_kms.pending_fence_fd);
        fprintf(stderr, "[KMS] atomic plane commit failed: %s; "
                        "falling back to legacy updates\n",
                strerror(atomic_errno));
        s_kms.atomic = 0;
        result = drmModeSetPlane(
            s_kms.fd, s_kms.plane_id, s_kms.crtc_id, buffer->fb, 0,
            0, 0, s_kms.mode.hdisplay, s_kms.mode.vdisplay,
            0, 0, s_kms.width << 16, s_kms.height << 16);
    }
    if (result == 0 && s_kms.atomic && s_kms.atomic_wait) {
        const uint64_t wait_start = kms_monotonic_ns();
        if (s_kms.pending_fence_fd < 0 ||
            kms_wait_fence(&s_kms.pending_fence_fd) != 0) {
            static int reported;
            if (!reported++)
                fprintf(stderr, "[KMS] atomic vblank fence wait failed: %s\n",
                        strerror(errno));
            return -1;
        }
        const uint64_t wait_end = kms_monotonic_ns();
        if (scanout_wait_ns && wait_end >= wait_start)
            *scanout_wait_ns += wait_end - wait_start;
    }
    if (result != 0) {
        static int reported;
        if (!reported++)
            fprintf(stderr, "[KMS] SetPlane failed: %s\n", strerror(errno));
        return -1;
    }
    const uint64_t flip_end = kms_monotonic_ns();
    if (flip_ns && flip_end >= flip_start) *flip_ns = flip_end - flip_start;
    ++s_kms.frames;
    return 0;
}

void rsx_kms_present_shutdown(void)
{
    if (s_kms.fd < 0) return;
    kms_wait_fence(&s_kms.pending_fence_fd);
    for (unsigned i = 0; i < KMS_BUFFERS; ++i)
        kms_wait_fence(&s_kms.buffers[i].release_fd);
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

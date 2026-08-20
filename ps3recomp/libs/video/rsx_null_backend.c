/*
 * ps3recomp - Null RSX Backend (Win32 window + color clear)
 *
 * Implements rsx_backend callbacks using a simple Win32 GDI window.
 * No GPU rendering — just clears to the RSX clear color and presents.
 * Used for debugging command buffer flow before a real backend exists.
 */

#include "rsx_null_backend.h"
#include "rsx_recorder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    HWND     hwnd;
    HDC      hdc;
    u32      width;
    u32      height;

    /* Current clear color (ARGB -> COLORREF) */
    COLORREF clear_color;

    /* Frame counter */
    u64      frame_count;
    u64      last_fps_time;
    u32      fps;

    int      window_closed;
} NullBackendState;

static NullBackendState s_state;

/* ---------------------------------------------------------------------------
 * Win32 window procedure
 * -----------------------------------------------------------------------*/

static LRESULT CALLBACK null_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        s_state.window_closed = 1;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        /* Fill with current clear color */
        HBRUSH brush = CreateSolidBrush(s_state.clear_color);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);

        /* Draw debug overlay text */
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        char buf[128];
        snprintf(buf, sizeof(buf), "ps3recomp null backend | %u FPS | frame %llu",
                 s_state.fps, (unsigned long long)s_state.frame_count);
        TextOutA(hdc, 10, 10, buf, (int)strlen(buf));

        char buf2[64];
        snprintf(buf2, sizeof(buf2), "Clear: #%06X",
                 (unsigned)(s_state.clear_color & 0xFFFFFF));
        TextOutA(hdc, 10, 30, buf2, (int)strlen(buf2));

        EndPaint(hwnd, &ps);
        return 0;
    }

    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---------------------------------------------------------------------------
 * Backend callbacks
 * -----------------------------------------------------------------------*/

static int null_init(void* ud, u32 width, u32 height)
{
    (void)ud;
    printf("[RSX null] init(%ux%u)\n", width, height);
    return 0;
}

static void null_shutdown(void* ud)
{
    (void)ud;
    printf("[RSX null] shutdown\n");
}

static void null_begin_frame(void* ud)
{
    (void)ud;
}

static void null_end_frame(void* ud)
{
    (void)ud;
    s_state.frame_count++;

    /* Compute FPS every second */
    ULONGLONG now = GetTickCount64();
    if (now - s_state.last_fps_time >= 1000) {
        /* Simple: just use frame_count delta. For first second, estimate. */
        s_state.fps = (u32)(s_state.frame_count -
                           (s_state.frame_count > 60 ? s_state.frame_count - 60 : 0));
        s_state.last_fps_time = now;
    }
}

static void null_present(void* ud, u32 buffer_id)
{
    (void)ud;
    (void)buffer_id;

    /* Trigger a repaint */
    if (s_state.hwnd)
        InvalidateRect(s_state.hwnd, NULL, FALSE);
}

static void null_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud;
    (void)flags;
    (void)depth;
    (void)stencil;

    /* Convert RSX ARGB to Win32 COLORREF (BGR) */
    u8 r = (color >> 16) & 0xFF;
    u8 g = (color >> 8) & 0xFF;
    u8 b = color & 0xFF;
    s_state.clear_color = RGB(r, g, b);
}

static void null_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud;
    printf("[RSX null] set_render_target(format=0x%X, %ux%u)\n",
           state->surface_format, state->surface_clip_w, state->surface_clip_h);
}

static void null_set_viewport(void* ud, const rsx_state* state)
{
    (void)ud;
    printf("[RSX null] set_viewport(%u,%u %ux%u)\n",
           state->viewport_x, state->viewport_y,
           state->viewport_w, state->viewport_h);
}

static void null_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    static int s_draw_log_count = 0;
    if (s_draw_log_count < 20) {
        printf("[RSX null] draw_arrays(prim=%u, first=%u, count=%u)\n",
               primitive, first, count);
        s_draw_log_count++;
    }
}

static void null_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count)
{
    (void)ud;
    static int s_draw_log_count = 0;
    if (s_draw_log_count < 20) {
        printf("[RSX null] draw_indexed(prim=%u, offset=%u, count=%u)\n",
               primitive, offset, count);
        s_draw_log_count++;
    }
}

/* ---------------------------------------------------------------------------
 * Backend registration
 * -----------------------------------------------------------------------*/

static rsx_backend s_null_backend = {
    .userdata          = &s_state,
    .init              = null_init,
    .shutdown          = null_shutdown,
    .begin_frame       = null_begin_frame,
    .end_frame         = null_end_frame,
    .present           = null_present,
    .set_render_target = null_set_render_target,
    .set_viewport      = null_set_viewport,
    .set_blend         = NULL,
    .set_depth_stencil = NULL,
    .clear             = null_clear,
    .draw_arrays       = null_draw_arrays,
    .draw_indexed      = null_draw_indexed,
    .bind_texture      = NULL,
};

/* ---------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

int rsx_null_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.width = width;
    s_state.height = height;
    s_state.clear_color = RGB(0, 0, 64); /* dark blue default */

    /* Register window class */
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = null_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "ps3recomp_null";
    RegisterClassExA(&wc);

    /* Compute window size from client area */
    RECT wr = {0, 0, (LONG)width, (LONG)height};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    s_state.hwnd = CreateWindowExA(
        0, "ps3recomp_null",
        title ? title : "ps3recomp",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, GetModuleHandle(NULL), NULL);

    if (!s_state.hwnd) {
        printf("[RSX null] ERROR: CreateWindow failed (%lu)\n", GetLastError());
        return -1;
    }

    s_state.hdc = GetDC(s_state.hwnd);
    s_state.last_fps_time = GetTickCount64();

    /* Register as the active RSX backend */
    rsx_set_backend(&s_null_backend);

    printf("[RSX null] Window created: %ux%u\n", width, height);
    return 0;
}

void rsx_null_backend_shutdown(void)
{
    if (s_state.hwnd) {
        ReleaseDC(s_state.hwnd, s_state.hdc);
        DestroyWindow(s_state.hwnd);
        s_state.hwnd = NULL;
    }
    rsx_set_backend(NULL);
    printf("[RSX null] Backend shut down after %llu frames\n",
           (unsigned long long)s_state.frame_count);
}

int rsx_null_backend_pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            return -1;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return s_state.window_closed ? -1 : 0;
}

void rsx_null_backend_get_counters(u64* clears, u64* draws,
                                   u64* textures, u64* presents)
{
    if (clears) *clears = 0;
    if (draws) *draws = s_state.frame_count;
    if (textures) *textures = 0;
    if (presents) *presents = s_state.frame_count;
}

#else /* !_WIN32 */

/* True headless backend: retain the command parser/state transitions and count
 * work, but never create a window or touch a host graphics API. */
typedef struct {
    u64 clears;
    u64 draws;
    u64 textures;
    u64 presents;
    int trace;
} HeadlessNullState;

static HeadlessNullState s_state;

static int headless_submit(void* userdata, const rsx_render_batch* batch)
{
    HeadlessNullState* state = (HeadlessNullState*)userdata;
    u64 textures = 0;
    for (u32 i = 0; i < batch->operation_count; ++i) {
        const rsx_render_op* op = &batch->operations[i];
        if (op->type == RSX_RENDER_OP_CLEAR)
            __atomic_add_fetch(&state->clears, 1, __ATOMIC_RELAXED);
        else if (op->type == RSX_RENDER_OP_DRAW) {
            __atomic_add_fetch(&state->draws, 1, __ATOMIC_RELAXED);
            textures += op->data.draw.texture_count;
        }
    }
    __atomic_add_fetch(&state->textures, textures, __ATOMIC_RELAXED);
    __atomic_add_fetch(&state->presents, 1, __ATOMIC_RELAXED);
    if (state->trace)
        fprintf(stderr, "[RSX null] batch %llu: %u operations\n",
                (unsigned long long)batch->serial, batch->operation_count);
    return 0;
}

static const char* headless_driver_name(void* userdata)
{
    (void)userdata;
    return "null";
}

static const rsx_render_backend_ops s_headless_consumer = {
    .submit_batch = headless_submit,
    .driver_name = headless_driver_name,
};

int rsx_null_backend_init(u32 width, u32 height, const char* title)
{
    (void)title;
    memset(&s_state, 0, sizeof(s_state));
    const char* trace = getenv("PS3RECOMP_NULL_RSX_TRACE");
    s_state.trace = trace && trace[0] != '0';
    if (rsx_recorder_install(NULL, &s_headless_consumer, &s_state) != 0)
        return -1;
    if (s_state.trace)
        fprintf(stderr, "[RSX null] headless surface %ux%u\n", width, height);
    fprintf(stderr, "[RSX null] native headless backend initialized\n");
    return 0;
}

void rsx_null_backend_shutdown(void)
{
    rsx_recorder_uninstall();
}

int rsx_null_backend_pump_messages(void)
{
    return 0;
}

void rsx_null_backend_get_counters(u64* clears, u64* draws,
                                   u64* textures, u64* presents)
{
    if (clears) *clears = __atomic_load_n(&s_state.clears, __ATOMIC_RELAXED);
    if (draws) *draws = __atomic_load_n(&s_state.draws, __ATOMIC_RELAXED);
    if (textures) *textures = __atomic_load_n(&s_state.textures, __ATOMIC_RELAXED);
    if (presents) *presents = __atomic_load_n(&s_state.presents, __ATOMIC_RELAXED);
}

#endif /* _WIN32 */

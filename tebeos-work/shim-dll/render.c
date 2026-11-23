/* render.c -- continuous screencast renderer (see render.h).
 *
 * The worker thread runs the CDP read loop: it receives Page.screencastFrame events,
 * base64-decodes and JPEG-decodes each one (stb_image), converts to BGRA, stores the
 * latest frame in a locked back-buffer, and StretchDIBits it into the target HWND.
 * Screencast acks are sent fire-and-forget via cdp_command() from the same (single)
 * worker thread, so c->next_id needs no locking here.
 */

#include "log.h"
#include "render.h"
#include "json.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DECODE (1u << 22)   /* 4 MiB: plenty for a 1080p JPEG frame */

struct RenderCtx {
    CdpConn *c;
    HWND hwnd;
    HANDLE thread;
    volatile LONG running;
    CRITICAL_SECTION lock;
    unsigned char *back;   /* latest BGRA (owned) */
    int w, h;
    size_t back_cap;
    long seq;
};

/* base64-decode a frame's data field fully; returns malloc'd buffer + length, or NULL. */
static unsigned char *frame_decode(const char *b64, int *outlen)
{
    unsigned char *raw = (unsigned char *)malloc(MAX_DECODE);
    int n;
    if (!raw)
        return NULL;
    n = b64_decode(b64, raw, MAX_DECODE);
    if (n <= 0) { free(raw); return NULL; }
    if (outlen)
        *outlen = n;
    return raw;
}

/* decode JPEG -> BGRA (top-down). Returns malloc'd buffer + dims, or NULL. */
static unsigned char *jpeg_to_bgra(const unsigned char *jpeg, int jlen, int *w, int *h)
{
    int x, y, comp, i;
    unsigned char *rgba = stbi_load_from_memory(jpeg, jlen, &x, &y, &comp, 4);
    unsigned char *bgra;
    size_t n;
    if (!rgba || x <= 0 || y <= 0) {
        stbi_image_free(rgba);
        return NULL;
    }
    bgra = (unsigned char *)malloc((size_t)x * y * 4);
    if (!bgra) { stbi_image_free(rgba); return NULL; }
    n = (size_t)x * y;
    for (i = 0; i < (int)n; i++) {
        bgra[i * 4 + 0] = rgba[i * 4 + 2]; /* B */
        bgra[i * 4 + 1] = rgba[i * 4 + 1]; /* G */
        bgra[i * 4 + 2] = rgba[i * 4 + 0]; /* R */
        bgra[i * 4 + 3] = rgba[i * 4 + 3]; /* A */
    }
    stbi_image_free(rgba);
    *w = x;
    *h = y;
    return bgra;
}

static void blit_to_hwnd(HWND hwnd, const unsigned char *bgra, int w, int h)
{
    HDC dc;
    BITMAPINFO bi;
    if (!hwnd)
        return;
    dc = GetDC(hwnd);
    if (!dc)
        return;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(dc, 0, 0, w, h, 0, 0, w, h, bgra, &bi, DIB_RGB_COLORS, SRCCOPY);
    GdiFlush();
    ReleaseDC(hwnd, dc);
}

static void ack_frame(RenderCtx *r, const char *session_id)
{
    char *qs = j_quote(session_id);
    char params[128];
    snprintf(params, sizeof(params), "{\"sessionId\":%s}", qs);
    free(qs);
    cdp_command(r->c, "Page.screencastFrameAck", params);
}

void render_handle_frame(RenderCtx *r, JVal *params)
{
    JVal *data = params ? j_get(params, "data") : NULL;
    const char *b64 = data ? j_str(data) : NULL;
    int rawlen = 0, w = 0, h = 0;
    unsigned char *raw, *bgra = NULL;
    HWND hwnd;

    if (!r || !b64)
        return;
    raw = frame_decode(b64, &rawlen);
    if (raw) {
        bgra = jpeg_to_bgra(raw, rawlen, &w, &h);
        free(raw);
    }
    if (!bgra)
        return;

    EnterCriticalSection(&r->lock);
    if ((size_t)w * h * 4 > r->back_cap) {
        r->back = (unsigned char *)realloc(r->back, (size_t)w * h * 4);
        r->back_cap = r->back ? (size_t)w * h * 4 : 0;
    }
    if (r->back) {
        memcpy(r->back, bgra, (size_t)w * h * 4);
        r->w = w;
        r->h = h;
        r->seq++;
    }
    hwnd = r->hwnd;
    LeaveCriticalSection(&r->lock);
    if (shim_tracing())
        shim_log("FRAME %ld %dx%d -> hwnd %p (%s)", r->seq, w, h, (void *)hwnd,
                 !hwnd ? "none" : !IsWindow(hwnd) ? "DEAD" : IsWindowVisible(hwnd) ? "visible" : "hidden");
    /* Paint on the window's own thread. Wine copies a window surface to X only when a thread
     * of the process waits for messages, so a frame blitted from this thread while the app's
     * UI thread sat idle never reached the screen (2026-10-22). */
    if (hwnd)
        InvalidateRect(hwnd, NULL, FALSE);
    free(bgra);
}

static DWORD WINAPI pump_thread(LPVOID arg)
{
    RenderCtx *r = (RenderCtx *)arg;
    int stale = 0;

    while (r->running) {
        JVal *m = NULL;
        const char *method;
        int rc = cdp_recv(r->c, &m);
        if (rc != 0 || !m) {
            /* The receive timeout fires while the page is idle (Medula repaints on
             * interaction, not on timers). Keep the pump alive across idle gaps, but
             * give up after a long run of failures (a truly-dead target). */
            if (++stale > 1800)
                break;
            if (rc == -1)
                Sleep(50);
            continue;
        }
        stale = 0;
        method = j_str(j_get(m, "method"));

        if (method && strcmp(method, "Page.screencastFrame") == 0) {
            JVal *params = j_get(m, "params");
            JVal *sid = params ? j_get(params, "sessionId") : NULL;
            render_handle_frame(r, params);
            if (sid && j_str(sid))
                ack_frame(r, j_str(sid));
        }
        j_free(m);
    }

    return 0;
}

RenderCtx *render_new(void)
{
    RenderCtx *r = (RenderCtx *)calloc(1, sizeof(*r));
    if (r)
        InitializeCriticalSection(&r->lock);
    return r;
}

void render_free(RenderCtx *r)
{
    if (!r)
        return;
    DeleteCriticalSection(&r->lock);
    free(r->back);
    free(r);
}

void render_set_hwnd(RenderCtx *r, HWND hwnd)
{
    if (!r)
        return;
    EnterCriticalSection(&r->lock);
    r->hwnd = hwnd;
    LeaveCriticalSection(&r->lock);
}

int render_start(CdpConn *c, HWND hwnd, RenderCtx **out)
{
    RenderCtx *r;
    char params[256];
    long id_base = 0x5C0000; /* id base distinct from element.c's g_cdp_id */

    if (!c || !out)
        return -1;
    *out = NULL;

    r = (RenderCtx *)calloc(1, sizeof(*r));
    if (!r)
        return -1;
    r->c = c;
    r->hwnd = hwnd;
    r->running = 1;
    InitializeCriticalSection(&r->lock);

    snprintf(params, sizeof(params),
             "{\"format\":\"jpeg\",\"quality\":80,\"maxWidth\":1920,\"maxHeight\":1080,\"everyNthFrame\":1}");

    {
        JVal *reply = NULL;
        int rc = cdp_call(c, id_base, "Page.startScreencast", params, &reply);
        if (reply)
            j_free(reply);
        if (rc != 0) {
            DeleteCriticalSection(&r->lock);
            free(r);
            return -1;
        }
    }

    r->thread = CreateThread(NULL, 0, pump_thread, r, 0, NULL);
    if (!r->thread) {
        DeleteCriticalSection(&r->lock);
        free(r);
        return -1;
    }

    *out = r;
    return 0;
}

void render_stop(RenderCtx *r)
{
    DWORD wait = WAIT_FAILED;
    if (!r)
        return;
    InterlockedExchange(&r->running, 0);

    if (r->thread) {
        wait = WaitForSingleObject(r->thread, 5000);
        if (wait == WAIT_TIMEOUT)
            fprintf(stderr, "[render_stop] pump thread did NOT exit in 5s; terminating\n");
        if (wait != WAIT_OBJECT_0) {
            /* The pump is stuck (e.g. blocked on a socket that never returns). Never let
             * shutdown hang: force the thread down. r->lock and r are freed below and no
             * one else touches them after html_doc_render_stop() nulls the pointer. */
            TerminateThread(r->thread, 0);
        }
        CloseHandle(r->thread);
        r->thread = NULL;
    }
    DeleteCriticalSection(&r->lock);
    free(r->back);
    free(r);
}

int render_repaint(RenderCtx *r, HWND hwnd)
{
    unsigned char *bgra = NULL;
    int w = 0, h = 0, ok = 0;
    if (!r || !hwnd)
        return -1;
    EnterCriticalSection(&r->lock);
    if (r->back) {
        size_t n = (size_t)r->w * r->h * 4;
        bgra = (unsigned char *)malloc(n);
        if (bgra) { memcpy(bgra, r->back, n); w = r->w; h = r->h; ok = 1; }
    }
    LeaveCriticalSection(&r->lock);
    if (ok) {
        blit_to_hwnd(hwnd, bgra, w, h);
        free(bgra);
        return 0;
    }
    return -1;
}

int render_frame_info(RenderCtx *r, int *w, int *h, long *seq)
{
    int ok = 0;
    if (!r)
        return -1;
    EnterCriticalSection(&r->lock);
    if (r->back) {
        if (w) *w = r->w;
        if (h) *h = r->h;
        if (seq) *seq = r->seq;
        ok = 1;
    }
    LeaveCriticalSection(&r->lock);
    return ok ? 0 : -1;
}

int render_last_frame(RenderCtx *r, unsigned char *dst, size_t dstcap, int *w, int *h, long *seq)
{
    int ok = 0;
    if (!r)
        return -1;
    EnterCriticalSection(&r->lock);
    if (r->back && (size_t)r->w * r->h * 4 <= dstcap) {
        memcpy(dst, r->back, (size_t)r->w * r->h * 4);
        if (w) *w = r->w;
        if (h) *h = r->h;
        if (seq) *seq = r->seq;
        ok = 1;
    }
    LeaveCriticalSection(&r->lock);
    return ok ? 0 : -1;
}

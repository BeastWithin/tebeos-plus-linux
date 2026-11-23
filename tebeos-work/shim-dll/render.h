/* render.h -- continuous screencast renderer for the Path-A mshtml shim (Milestone 5c).
 *
 * A worker thread owns the CDP read loop: Page.startScreencast, then a frame pump that
 * decodes each Page.screencastFrame (base64 JPEG -> RGB via stb_image -> BGRA) and (a)
 * StretchDIBits it into a target HWND, and (b) keeps the latest frame in a locked
 * back-buffer so callers (and the WM_PAINT handler) can read it without racing the pump.
 */

#ifndef SHIM_RENDER_H
#define SHIM_RENDER_H

#include "cdp.h"
#include <windows.h>

typedef struct RenderCtx RenderCtx;

/* Start the frame pump on an already-connected CDP session. `hwnd` is the blit target
 * (may be NULL to only fill the back-buffer). Sends Page.startScreencast and spawns the
 * worker. Returns 0 and sets *out, or -1 on failure. */
int render_start(CdpConn *c, HWND hwnd, RenderCtx **out);

/* Stop the pump: stop screencast, signal the worker, join it, and free the context. */
void render_stop(RenderCtx *r);

/* Copy the latest decoded BGRA frame into dst (must hold at least dstcap bytes).
 * Sets *w and *h and *seq (a monotonic frame counter). Returns 0 on success, -1 if no
 * frame has arrived yet. */
int render_last_frame(RenderCtx *r, unsigned char *dst, size_t dstcap, int *w, int *h, long *seq);

/* Blit the latest decoded frame (back-buffer) into hwnd. Intended for the WM_PAINT
 * handler of the hosting window. Returns 0 if a frame was available and blitted, -1 if
 * no frame has arrived yet. */
int render_repaint(RenderCtx *r, HWND hwnd);

/* Read the latest frame's dimensions + monotonic seq WITHOUT copying pixels. Returns 0
 * if a frame has arrived, -1 otherwise. (Diagnostic/test helper.) */
int render_frame_info(RenderCtx *r, int *w, int *h, long *seq);

/* Passive mode (page.c): the page's own pump reads the connection and hands each
 * Page.screencastFrame's params in. render_handle_frame decodes the frame, keeps it as the
 * latest frame and blits it into the current hwnd; it does not ack. */
RenderCtx *render_new(void);
void render_free(RenderCtx *r);
void render_set_hwnd(RenderCtx *r, HWND hwnd);
void render_handle_frame(RenderCtx *r, JVal *params);

#endif /* SHIM_RENDER_H */

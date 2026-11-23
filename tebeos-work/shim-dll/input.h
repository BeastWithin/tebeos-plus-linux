/* input.h -- CDP Input-domain forwarding (mouse/keyboard) for the shim (Milestone 5c).
 *
 * Translates window mouse/key events into Chrome DevTools Protocol Input.* commands so
 * the vendor app can drive the Medula page (click the menu, type into fields) as if the
 * IE WebBrowser control were rendering it natively.
 *
 * These sends are fire-and-forget via cdp_command(); cdp.c serializes socket writes with
 * a critical section, so they are safe to call from the UI thread while the render pump
 * (in render.c) owns the read loop.
 */

#ifndef SHIM_INPUT_H
#define SHIM_INPUT_H

#include "cdp.h"

/* Input.dispatchMouseEvent. type: "mouseMoved" | "mousePressed" | "mouseReleased".
 * button: "none" | "left" | "right" | "middle". Returns 0 on success. */
int input_mouse(CdpConn *c, const char *type, int x, int y, const char *button, int click_count);

/* Input.dispatchMouseEvent with the held-buttons mask (left=1, right=2, middle=4) and
 * modifiers (Alt=1, Ctrl=2, Shift=8): without "buttons" a move with the button down is a
 * hover to Chromium (no scrollbar drag, no text selection). */
int input_mouse_ex(CdpConn *c, const char *type, int x, int y, const char *button,
                   int click_count, int buttons, int modifiers);

/* Input.dispatchMouseEvent mouseWheel; deltas in CSS pixels (positive deltaY scrolls down). */
int input_wheel(CdpConn *c, int x, int y, int dx, int dy, int modifiers);

/* Full left-click: move + press + release. */
int input_click(CdpConn *c, int x, int y);

/* Input.dispatchKeyEvent. type: "keyDown" | "keyUp" | "rawKeyDown" | "char".
 * key is the DOM key value ("a", "Enter", ...); text only matters for "char".
 * vk is the Windows virtual-key code (0 if unknown). */
int input_key(CdpConn *c, const char *type, const char *key, const char *text, int vk);

/* Input.dispatchKeyEvent with a DOM code and CDP modifiers (Alt=1, Ctrl=2, Meta=4, Shift=8),
 * for named keys (F3, Escape, ...) whose key and code strings are the same. */
int input_key_mod(CdpConn *c, const char *type, const char *key, int vk, int modifiers);

/* Input.insertText: insert text into the currently focused element. */
int input_insert_text(CdpConn *c, const char *text);

#endif /* SHIM_INPUT_H */

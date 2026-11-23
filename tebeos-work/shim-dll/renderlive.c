/* renderlive.c -- prove the continuous render worker (Milestone 5c-i).
 *
 * Navigates Chromium to an animated red/blue page, starts render.c's worker thread
 * (screencast pump + JPEG decode + blit into a real HWND), and then polls the latest
 * back-buffer to confirm (a) frames arrive continuously (seq increases) and (b) the
 * animation is really being decoded (both red and blue show up). Also exercises the
 * GDI blit-into-HWND path from the worker thread (a top-level window is the target).
 *
 * Usage:  wine renderlive32.exe [port]
 */

#include "render.h"
#include "cdp.h"
#include "json.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char ANIM_URL[] =
    "{\"url\":\"data:text/html;base64,"
    "PCFET0NUWVBFIGh0bWw+PGh0bWw+PGJvZHkgc3R5bGU9ImJhY2tncm91bmQ6cmVkIj48aDEgaWQ9InQiPjA8L2gxPjxzY3JpcHQ+dmFyIG49MDtzZXRJbnRlcnZhbChmdW5jdGlvbigpe24rKztkb2N1bWVudC5nZXRFbGVtZW50QnlJZCgndCcpLnRleHRDb250ZW50PW47ZG9jdW1lbnQuYm9keS5zdHlsZS5iYWNrZ3JvdW5kPShuJTIpPydyZWQnOidibHVlJzt9LDIwMCk7PC9zY3JpcHQ+PC9ib2R5PjwvaHRtbD4=\"}";

static int is_red_bgra(const unsigned char *p)  { return p[2] > 200 && p[0] < 100 && p[1] < 100; }
static int is_blue_bgra(const unsigned char *p) { return p[0] > 200 && p[2] < 100 && p[1] < 100; }

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 9222;
    char wsurl[512];
    CdpConn *c;
    RenderCtx *r = NULL;
    HWND hwnd = NULL;
    long id = 0, seq = -1, last_seq = -1;
    int saw_red = 0, saw_blue = 0, frames = 0, failures = 0, poll;

    setbuf(stdout, NULL);

    if (cdp_first_page_wsurl("127.0.0.1", port, wsurl, sizeof(wsurl)) != 0) {
        printf("FAIL: no page target on %d\n", port);
        return 1;
    }
    c = cdp_connect_wsurl(wsurl);
    if (!c) { printf("FAIL: connect\n"); return 1; }

    /* Navigate to the animated page, then wait for it to settle. */
    {
        JVal *reply = NULL;
        cdp_call(c, ++id, "Page.navigate", ANIM_URL, &reply);
        if (reply) j_free(reply);
    }
    Sleep(600);

    /* A real top-level window as the blit target (mirrors docobj.c's d->hwnd). */
    {
        WNDCLASSW wc;
        HINSTANCE inst = GetModuleHandleW(NULL);
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = inst;
        wc.lpszClassName = L"RenderLiveWnd";
        RegisterClassW(&wc);
        hwnd = CreateWindowExW(0, L"RenderLiveWnd", L"render", WS_OVERLAPPEDWINDOW,
                               0, 0, 800, 600, NULL, NULL, inst, NULL);
    }

    if (render_start(c, hwnd, &r) != 0) {
        printf("FAIL: render_start\n");
        cdp_close(c);
        return 1;
    }
    printf("render worker started\n");

    for (poll = 0; poll < 12; poll++) {
        {
            unsigned char *fb = (unsigned char *)malloc(1920 * 1080 * 4);
            int w = 0, h = 0;
            long s = -1;
            if (render_last_frame(r, fb, 1920 * 1080 * 4, &w, &h, &s) == 0) {
                const unsigned char *cpx = fb + ((size_t)h / 2) * w * 4 + ((size_t)w / 2) * 4;
                frames++;
                seq = s;
                if (is_red_bgra(cpx))  saw_red = 1;
                if (is_blue_bgra(cpx)) saw_blue = 1;
                if (s != last_seq) {
                    printf("  frame seq=%ld %dx%d centre BGRA=%3d,%3d,%3d (%s)\n",
                           s, w, h, cpx[0], cpx[1], cpx[2],
                           is_red_bgra(cpx) ? "RED" : is_blue_bgra(cpx) ? "BLUE" : "?");
                    last_seq = s;
                }
            }
            free(fb);
        }
        Sleep(180);
    }

    render_stop(r);
    cdp_close(c);
    if (hwnd) DestroyWindow(hwnd);

    printf("saw %d frames, seq advanced to %ld, red=%d blue=%d\n", frames, seq, saw_red, saw_blue);
    if (frames < 3)          { printf("FAIL: too few frames (pump not running?)\n"); failures++; }
    if (!saw_red || !saw_blue) { printf("FAIL: animation not decoded (red/blue missing)\n"); failures++; }
    printf("%s\n", failures == 0 ? "PASS (continuous render)" : "FAIL");
    return failures == 0 ? 0 : 1;
}

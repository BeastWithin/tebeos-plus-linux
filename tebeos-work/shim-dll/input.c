/* input.c -- CDP Input-domain forwarding (see input.h). */

#include "input.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int input_mouse(CdpConn *c, const char *type, int x, int y, const char *button, int click_count)
{
    char params[256];
    snprintf(params, sizeof(params),
             "{\"type\":\"%s\",\"x\":%d,\"y\":%d,\"button\":\"%s\",\"clickCount\":%d}",
             type, x, y, button, click_count);
    return cdp_command(c, "Input.dispatchMouseEvent", params);
}

int input_mouse_ex(CdpConn *c, const char *type, int x, int y, const char *button,
                   int click_count, int buttons, int modifiers)
{
    char params[320];
    snprintf(params, sizeof(params),
             "{\"type\":\"%s\",\"x\":%d,\"y\":%d,\"button\":\"%s\",\"clickCount\":%d,"
             "\"buttons\":%d,\"modifiers\":%d}",
             type, x, y, button, click_count, buttons, modifiers);
    return cdp_command(c, "Input.dispatchMouseEvent", params);
}

int input_wheel(CdpConn *c, int x, int y, int dx, int dy, int modifiers)
{
    char params[256];
    snprintf(params, sizeof(params),
             "{\"type\":\"mouseWheel\",\"x\":%d,\"y\":%d,\"deltaX\":%d,\"deltaY\":%d,\"modifiers\":%d}",
             x, y, dx, dy, modifiers);
    return cdp_command(c, "Input.dispatchMouseEvent", params);
}

int input_click(CdpConn *c, int x, int y)
{
    if (input_mouse(c, "mouseMoved", x, y, "none", 0) != 0)
        return -1;
    if (input_mouse(c, "mousePressed", x, y, "left", 1) != 0)
        return -1;
    return input_mouse(c, "mouseReleased", x, y, "left", 1);
}

int input_key(CdpConn *c, const char *type, const char *key, const char *text, int vk)
{
    char params[512];
    char *k = j_quote(key ? key : "");
    char *t = j_quote(text ? text : "");
    snprintf(params, sizeof(params),
             "{\"type\":\"%s\",\"key\":%s,\"text\":%s,\"windowsVirtualKeyCode\":%d,\"nativeVirtualKeyCode\":%d}",
             type, k, t, vk, vk);
    free(k);
    free(t);
    return cdp_command(c, "Input.dispatchKeyEvent", params);
}

int input_key_mod(CdpConn *c, const char *type, const char *key, int vk, int modifiers)
{
    char params[512];
    char *k = j_quote(key ? key : "");
    snprintf(params, sizeof(params),
             "{\"type\":\"%s\",\"key\":%s,\"code\":%s,\"windowsVirtualKeyCode\":%d,"
             "\"nativeVirtualKeyCode\":%d,\"modifiers\":%d}",
             type, k, k, vk, vk, modifiers);
    free(k);
    return cdp_command(c, "Input.dispatchKeyEvent", params);
}

/* any length: a paste can be long (a list of karekods), and a fixed buffer cut the JSON short,
 * which Chromium dropped without a word */
int input_insert_text(CdpConn *c, const char *text)
{
    char *t = j_quote(text);
    size_t n = t ? strlen(t) + 16 : 0;
    char *params = t ? malloc(n) : NULL;
    int rc = -1;
    if (params) {
        snprintf(params, n, "{\"text\":%s}", t);
        rc = cdp_command(c, "Input.insertText", params);
    }
    free(params);
    free(t);
    return rc;
}

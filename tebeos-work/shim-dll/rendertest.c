/* rendertest.c -- prove the CDP screencast session + async frame pump against real Chromium.
 *
 * This is the foundation of the Path-A "render target" (option b: drive Chromium over CDP
 * and stream its frames into ieframe's HWND). It does NOT decode pixels yet (that needs an
 * image codec); it proves the hard parts: Page.startScreencast, that Page.screencastFrame
 * events actually flow (with valid JPEG payload + device dimensions), per-frame ack, and
 * Page.stopScreencast.
 *
 * Usage:  wine rendertest32.exe [port]     (default 9222)
 * Chromium:
 *   chromium --headless=new --remote-debugging-port=9222 --no-sandbox about:blank
 */

#include "cdp.h"
#include "json.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int send_cmd(CdpConn *c, long *id, const char *method, const char *params)
{
    JVal *reply = NULL;
    int r = cdp_call(c, ++(*id), method, params, &reply);
    if (reply)
        j_free(reply);
    return r;
}

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 9222;
    char wsurl[512];
    CdpConn *c;
    long id = 0;
    int frames = 0, valid_jpeg = 0;
    int want = 4;
    int i;

    setbuf(stdout, NULL);

    if (cdp_first_page_wsurl("127.0.0.1", port, wsurl, sizeof(wsurl)) != 0) {
        printf("FAIL: no page target on %d\n", port);
        return 1;
    }
    c = cdp_connect_wsurl(wsurl);
    if (!c) {
        printf("FAIL: connect\n");
        return 1;
    }
    printf("connected\n");

    send_cmd(c, &id, "Page.navigate",
      "{\"url\":\"data:text/html;base64,PCFET0NUWVBFIGh0bWw+PGh0bWw+PGJvZHkgc3R5bGU9ImJhY2tncm91bmQ6cmVkIj48aDEgaWQ9InQiPjA8L2gxPjxzY3JpcHQ+dmFyIG49MDtzZXRJbnRlcnZhbChmdW5jdGlvbigpe24rKztkb2N1bWVudC5nZXRFbGVtZW50QnlJZCgndCcpLnRleHRDb250ZW50PW47ZG9jdW1lbnQuYm9keS5zdHlsZS5iYWNrZ3JvdW5kPShuJTIpPydyZWQnOidibHVlJzt9LDIwMCk7PC9zY3JpcHQ+PC9ib2R5PjwvaHRtbD4=\"}");
    Sleep(900);

    if (send_cmd(c, &id, "Page.startScreencast",
                 "{\"format\":\"jpeg\",\"quality\":80,\"maxWidth\":800,\"maxHeight\":600,\"everyNthFrame\":1}") != 0) {
        printf("FAIL: startScreencast\n");
        cdp_close(c);
        return 1;
    }
    printf("Page.startScreencast OK\n");
    fflush(stdout);

    for (i = 0; i < 300 && frames < want; i++) {
        JVal *m = NULL;
        const char *method;
        if (cdp_recv(c, &m) != 0)
            break;
        method = j_str(j_get(m, "method"));
        if (method && strcmp(method, "Page.screencastFrame") == 0) {
            JVal *params = j_get(m, "params");
            JVal *data = params ? j_get(params, "data") : NULL;
            JVal *meta = params ? j_get(params, "metadata") : NULL;
            JVal *sid = params ? j_get(params, "sessionId") : NULL;
            JVal *dw = meta ? j_get(meta, "deviceWidth") : NULL;
            JVal *dh = meta ? j_get(meta, "deviceHeight") : NULL;
            const char *b64 = data ? j_str(data) : NULL;
            unsigned char head[8];
            int hlen = 0;
            memset(head, 0, sizeof(head));
            frames++;
            if (b64) {
                hlen = b64_decode(b64, head, sizeof(head));
                if (hlen >= 2 && head[0] == 0xFF && head[1] == 0xD8)
                    valid_jpeg++;
            }
            printf("frame %d: %dx%d, base64=%d bytes, hdr=%02x%02x%02x%02x (%s)\n",
                   frames,
                   dw ? (int)j_num(dw) : -1,
                   dh ? (int)j_num(dh) : -1,
                   b64 ? (int)strlen(b64) : -1,
                   head[0], head[1], head[2], head[3],
                   (hlen >= 2 && head[0] == 0xFF && head[1] == 0xD8) ? "jpeg"
                   : (hlen >= 4 && head[0] == 0x89 && head[1] == 0x50) ? "png" : "?");
            if (sid && j_str(sid)) {
                char ack[256];
                char *qs = j_quote(j_str(sid));
                snprintf(ack, sizeof(ack), "{\"sessionId\":%s}", qs);
                free(qs);
                send_cmd(c, &id, "Page.screencastFrameAck", ack);
            }
        }
        j_free(m);
    }

    send_cmd(c, &id, "Page.stopScreencast", "{}");
    printf("captured %d frames, %d valid JPEG\n", frames, valid_jpeg);
    cdp_close(c);
    printf("%s\n", (frames > 0 && valid_jpeg > 0) ? "PASS (screencast frames)" : "FAIL");
    return (frames > 0 && valid_jpeg > 0) ? 0 : 1;
}

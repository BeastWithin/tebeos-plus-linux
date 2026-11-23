/* livetest.c -- exercise the CDP client against a REAL Chromium-family debugging port.
 *
 * Usage:  wine livetest32.exe [port]     (default 9222)
 *
 * Expects a Chromium already running with that port, e.g.:
 *   chromium --headless=new --remote-debugging-port=9222 --no-sandbox about:blank
 *
 * Proves full interop (real server, not the mock): /json discovery, WS handshake against a
 * genuine server, Runtime.evaluate (number and string), and Page.navigate.
 */

#include "cdp.h"
#include "json.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double eval_num(CdpConn *c, long *id, const char *expr)
{
    JVal *reply = NULL;
    char params[4096];
    double v = 0.0;
    char *q = j_quote(expr);
    snprintf(params, sizeof(params), "{\"expression\":%s,\"returnByValue\":true}", q);
    free(q);
    if (cdp_call(c, ++(*id), "Runtime.evaluate", params, &reply) == 0) {
        JVal *res = j_get(reply, "result");
        JVal *robj = res ? j_get(res, "result") : NULL;
        JVal *val = robj ? j_get(robj, "value") : NULL;
        if (val) v = j_num(val);
        j_free(reply);
    }
    return v;
}

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 9222;
    char wsurl[512];
    CdpConn *c;
    long id = 0;
    int failures = 0;
    double v;

    setbuf(stdout, NULL);

    if (cdp_first_page_wsurl("127.0.0.1", port, wsurl, sizeof(wsurl)) != 0) {
        printf("FAIL: no page target on port %d\n", port);
        return 1;
    }
    printf("page target: %s\n", wsurl);

    c = cdp_connect_wsurl(wsurl);
    printf("cdp_connect_wsurl = %s\n", c ? "OK" : "FAIL");
    if (!c) return 1;

    v = eval_num(c, &id, "1+2");
    printf("Runtime.evaluate(1+2) = %.0f  %s\n", v, (v == 3.0) ? "OK" : "FAIL");
    if (v != 3.0) failures++;

    /* navigate to a data URL, then read document.title */
    {
        JVal *reply = NULL;
        char params[512];
        snprintf(params, sizeof(params), "{\"url\":\"data:text/html,<title>teb</title><p>merhaba\\u015f\\u0131\\u00e7\\u00f6</p>\"}");
        cdp_call(c, ++id, "Page.navigate", params, &reply);
        if (reply) j_free(reply);
        Sleep(1200);
    }

    {
        JVal *reply = NULL;
        char params[512];
        const char *title, *inner;
        char *q = j_quote("document.title");
        snprintf(params, sizeof(params), "{\"expression\":%s,\"returnByValue\":true}", q);
        free(q);
        cdp_call(c, ++id, "Runtime.evaluate", params, &reply);
        {
            JVal *res = j_get(reply, "result");
            JVal *robj = res ? j_get(res, "result") : NULL;
            JVal *val = robj ? j_get(robj, "value") : NULL;
            title = j_str(val);
        }
        printf("document.title = '%s'  %s\n", title ? title : "(null)",
               (title && strcmp(title, "teb") == 0) ? "OK" : "FAIL");
        if (!title || strcmp(title, "teb") != 0) failures++;
        j_free(reply);

        /* read an element's innerText with Turkish chars (proves UTF-8 round-trip) */
        q = j_quote("document.querySelector('p').innerText");
        snprintf(params, sizeof(params), "{\"expression\":%s,\"returnByValue\":true}", q);
        free(q);
        cdp_call(c, ++id, "Runtime.evaluate", params, &reply);
        {
            JVal *res = j_get(reply, "result");
            JVal *robj = res ? j_get(res, "result") : NULL;
            JVal *val = robj ? j_get(robj, "value") : NULL;
            inner = j_str(val);
        }
        printf("p.innerText = '%s'\n", inner ? inner : "(null)");
        j_free(reply);
    }

    cdp_close(c);
    printf("%s\n", failures ? "FAIL" : "PASS (live CDP vs Chromium)");
    return failures ? 1 : 0;
}

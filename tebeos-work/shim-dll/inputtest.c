/* inputtest.c -- prove CDP input forwarding (Milestone 5c): drive a real page with
 * synthetic mouse + key events and confirm the page reacts.
 *
 * Loads a page with a button (increments window.clicked on click) and a text input,
 * then: (1) clicks the button and checks window.clicked==1; (2) focuses the input and
 * inserts text, checking the value. This proves Input.dispatchMouseEvent and
 * Input.insertText round-trip through Chromium and actually mutate the page.
 *
 * Usage:  wine inputtest32.exe [port]
 */

#include "input.h"
#include "cdp.h"
#include "json.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char PAGE_URL[] =
    "{\"url\":\"data:text/html;base64,"
    "PCFET0NUWVBFIGh0bWw+PGh0bWw+PGJvZHk+PGlucHV0IGlkPSJpIiBzdHlsZT0icG9zaXRpb246Zml4ZWQ7bGVmdDoxMHB4O3RvcDoyMHB4O3dpZHRoOjIwMHB4O2hlaWdodDozMHB4Ij48YnV0dG9uIGlkPSJiIiBzdHlsZT0icG9zaXRpb246Zml4ZWQ7bGVmdDoxMHB4O3RvcDo2MHB4O3dpZHRoOjEyMHB4O2hlaWdodDo0MHB4IiBvbmNsaWNrPSJ3aW5kb3cuY2xpY2tlZD0od2luZG93LmNsaWNrZWR8fDApKzEiPkI8L2J1dHRvbj48L2JvZHk+PC9odG1sPg==\"}";

/* Evaluate a JS expression (returnByValue) and read the result as a number. */
static int eval_num(CdpConn *c, long *id, const char *expr, double *out)
{
    char params[8192];
    char *q = j_quote(expr);
    JVal *reply = NULL, *r, *remote, *val;
    int rc;
    snprintf(params, sizeof(params), "{\"expression\":%s,\"returnByValue\":true}", q);
    free(q);
    rc = cdp_call(c, ++(*id), "Runtime.evaluate", params, &reply);
    if (rc != 0 || !reply) { if (reply) j_free(reply); return -1; }
    r = j_get(reply, "result");
    remote = r ? j_get(r, "result") : NULL;
    val = remote ? j_get(remote, "value") : NULL;
    if (!val) { j_free(reply); return -1; }
    if (val->t == J_NUM) *out = j_num(val);
    else if (val->t == J_BOOL) *out = j_is_true(val) ? 1 : 0;
    else { j_free(reply); return -1; }
    j_free(reply);
    return 0;
}

/* Evaluate a JS expression and read the result as a string (into out/cap). */
static int eval_str(CdpConn *c, long *id, const char *expr, char *out, int cap)
{
    char params[8192];
    char *q = j_quote(expr);
    JVal *reply = NULL, *r, *remote, *val;
    int rc;
    snprintf(params, sizeof(params), "{\"expression\":%s,\"returnByValue\":true}", q);
    free(q);
    rc = cdp_call(c, ++(*id), "Runtime.evaluate", params, &reply);
    if (rc != 0 || !reply) { if (reply) j_free(reply); return -1; }
    r = j_get(reply, "result");
    remote = r ? j_get(r, "result") : NULL;
    val = remote ? j_get(remote, "value") : NULL;
    if (!val) { j_free(reply); return -1; }
    if (val->t == J_STR) snprintf(out, cap, "%s", j_str(val));
    else if (val->t == J_NUM) snprintf(out, cap, "%g", j_num(val));
    else { snprintf(out, cap, "<%d>", (int)val->t); j_free(reply); return 0; }
    j_free(reply);
    return 0;
}

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 9222;
    char wsurl[512];
    CdpConn *c;
    long id = 0;
    double n = -1;
    char s[256];
    int failures = 0;

    setbuf(stdout, NULL);

    if (cdp_first_page_wsurl("127.0.0.1", port, wsurl, sizeof(wsurl)) != 0) {
        printf("FAIL: no page target on %d\n", port);
        return 1;
    }
    c = cdp_connect_wsurl(wsurl);
    if (!c) { printf("FAIL: connect\n"); return 1; }

    {
        JVal *reply = NULL;
        cdp_call(c, ++id, "Page.navigate", PAGE_URL, &reply);
        if (reply) j_free(reply);
    }
    Sleep(700);

    /* 1. click the button at its centre (fixed 10,60 / 120x40 -> centre 70,80). */
    if (input_click(c, 70, 80) != 0) { printf("FAIL: input_click send\n"); failures++; }
    Sleep(400);
    if (eval_num(c, &id, "window.clicked", &n) != 0 || n != 1) {
        printf("FAIL: button click not registered (window.clicked=%g)\n", n);
        failures++;
    } else {
        printf("button click OK -> window.clicked = %g\n", n);
    }

    /* 2. focus the text input and insert text. */
    if (input_click(c, 110, 35) != 0) { printf("FAIL: focus click send\n"); failures++; }
    Sleep(300);
    if (input_insert_text(c, "abc") != 0) { printf("FAIL: insertText send\n"); failures++; }
    Sleep(400);
    if (eval_str(c, &id, "document.getElementById('i').value", s, sizeof(s)) != 0 || strcmp(s, "abc") != 0) {
        printf("FAIL: input value = '%s' (expected 'abc')\n", s);
        failures++;
    } else {
        printf("typing OK -> input value = '%s'\n", s);
    }

    cdp_close(c);
    printf("%s\n", failures == 0 ? "PASS (input forwarding)" : "FAIL");
    return failures == 0 ? 0 : 1;
}

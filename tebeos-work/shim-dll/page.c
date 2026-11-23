/* page.c -- one Chromium tab per document (see page.h).
 *
 * Navigation follows Wine's own mshtml (navigate.c / nsevents.c / mutation.c):
 *   - a navigation ieframe asked for (Load, SuperNavigate) was already announced by ieframe,
 *     so the request it makes is "armed": the pump lets it through, applying the POST body
 *     and extra headers ieframe handed over (Page.navigate cannot POST);
 *   - any other main-frame document request (a link, a form submit, location.href, a
 *     script redirect) is paused by Fetch and announced on the UI thread with
 *     IDocObjectService::FireBeforeNavigate2; a cancel fails the request, as IE would;
 *   - DOMContentLoaded fires NavigateComplete2, the load event sets readyState "complete"
 *     and fires DocumentComplete.
 * Server redirects and sub-frames are let through without a BeforeNavigate2, as IE does.
 */

#include "html_doc.h"
#include "page.h"
#include "render.h"
#include "dom.h"
#include "log.h"
#include "util.h"
#include "iecompat.h"
#include "rcache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WM_PAGE_EVENT    (WM_APP + 0x51)
#define WM_PAGE_ALERT    (WM_APP + 0x52)
#define WM_PAGE_CATCHUP  (WM_APP + 0x53)
#define WM_PAGE_COMPLETE (WM_APP + 0x54)
#define WM_PAGE_FOCUS    (WM_APP + 0x55)

/* The focus events IHTMLElement2::focus/blur held back (element.c), fired in order. A
 * document that replaced theirs has no queue. */
static const char replay_focus_js[] =
    "(function(){var w=window,q=w.__shim_fq;if(!q||!q.length)return 0;w.__shim_fq=[];"
    "q.forEach(function(x){var b=x[1]==='focusin'||x[1]==='focusout';"
    "try{x[0].dispatchEvent(new FocusEvent(x[1],{bubbles:b,relatedTarget:x[2]||null}));}catch(y){}});"
    "return q.length;})()";

#define STASH_BLOCK 0x100000L   /* stash ids per document */

typedef struct Armed {
    int active;
    char *post_b64;     /* base64 POST body, or NULL */
    char *headers;      /* "Name: value\r\n..." extra request headers, or NULL */
} Armed;

struct Page {
    LONG ref;
    HTMLDocument *doc;          /* weak; cleared by page_close */
    CdpConn *dom;
    CdpConn *evt;
    volatile LONG evt_acked;    /* highest fire-and-forget reply id seen on evt (pump) */
    int legacy;
    char target_id[128];        /* = the main frame id */
    HANDLE thread;
    volatile LONG running;
    HWND msgwnd;
    struct RenderCtx *render;
    int screencast;
    int width, height;
    HWND embed_hwnd;            /* embed mode: the shim window Chromium's window covers */
    long embed_x, embed_y;      /* ... and what was last reported for it */
    int embed_w, embed_h;
    unsigned long embed_x11;
    CRITICAL_SECTION lock;      /* armed */
    Armed armed;
    int blank_expected;         /* an about:blank navigation was asked for */
    int committed;              /* a main-frame document has committed (cleared by set_content) */
    int dom_ready, loaded;      /* ... and reached DOMContentLoaded / load */
    int fired_nc2, fired_dc;    /* NavigateComplete2 / DocumentComplete reported for it */
    char loader_id[128];        /* = the Network requestId of the main document */
    char *source;               /* the current document's source as IE keeps it (page_save) */
    size_t source_len;
    long stash_base;            /* first stash id for the next document (dom.h) */
    struct HtmlEventObj *cur_event;
};

static char *fetch_source(Page *pg, size_t *len);
static int navigate_armed(Page *pg, const char *url, const BYTE *post, DWORD postlen,
                          const char *headers, const char *referrer);
static void set_source(Page *pg, char *src, size_t len);
static UINT charset_cp(const char *cs);

/* ------------------------------------------------------------------ Chromium bootstrap */

static CRITICAL_SECTION g_boot_lock;
static INIT_ONCE g_boot_once = INIT_ONCE_STATIC_INIT;
static int g_port;
static int g_embed;   /* TEBEOS_EMBED=1: headful Chromium embedded by an external helper */
static char g_embed_busy[128];   /* embed: the app window page a Page of this process holds */
static int g_launched;
static CdpConn *g_browser;      /* browser-level connection; owns the browser context */
static char g_context[128];
static char *g_user_agent;
static Page *g_legacy;

static BOOL CALLBACK boot_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_boot_lock);
    return TRUE;
}

static int env_int(const char *name, int def)
{
    char buf[32];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? atoi(buf) : def;
}

static int env_is(const char *name, const char *value)
{
    char buf[64];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return n > 0 && n < sizeof(buf) && _stricmp(buf, value) == 0;
}

static void launch_chromium(int port)
{
    WCHAR exe[MAX_PATH], prof[MAX_PATH], cmd[2048];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    /* A Unix binary started through CreateProcess is fork+exec'ed by Wine (ntdll/unix). */
    if (!GetEnvironmentVariableW(L"TEBEOS_CHROMIUM", exe, MAX_PATH))
        wcscpy(exe, L"Z:\\usr\\bin\\chromium");
    if (!GetEnvironmentVariableW(L"TEBEOS_CDP_PROFILE", prof, MAX_PATH))
        wcscpy(prof, L"/tmp/tebeos-cdp-profile");
    if (g_embed) {
        /* Embed mode (spike): a headful Chromium on X11 whose window is reparented into the
         * Medula window by an external helper (embed-spike/embed_helper.py). No screencast. */
        _snwprintf(cmd, ARRAY_SIZE(cmd),
                   L"\"%ls\" --ozone-platform=x11 --disable-gpu --app=data:text/html,<title>Medula</title> "
                   L"--remote-debugging-port=%d --user-data-dir=%ls "
                   L"--no-first-run --no-default-browser-check --lang=tr-TR "
                   L"--optimization-guide-performance-class=1 --optimization-guide-disable-installer "
                   L"--window-size=1000,700",
                   exe, port, prof);
    } else
    _snwprintf(cmd, ARRAY_SIZE(cmd),
               L"\"%ls\" --headless=new --remote-debugging-port=%d --user-data-dir=%ls "
               L"--no-first-run --no-default-browser-check --lang=tr-TR "
               /* no on-device AI: a preset performance class skips the GPU benchmark that
                * starts the OnDeviceModelService process ~3 minutes in, and no model installer */
               L"--optimization-guide-performance-class=1 --optimization-guide-disable-installer "
               L"about:blank",
               exe, port, prof);
    cmd[ARRAY_SIZE(cmd) - 1] = 0;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        shim_log("BOOT CreateProcess(chromium) failed: %lu", (unsigned long)GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    shim_log("BOOT launched chromium on port %d", port);
}

/* Find (or start) Chromium and open this process's browser context. g_boot_lock held. */
static int boot_locked(void)
{
    char *ver = NULL;
    JVal *v = NULL, *reply = NULL;
    const char *ws, *ua, *ctx;
    size_t off = 0;
    int i;

    if (g_browser)
        return 0;
    g_port = env_int("TEBEOS_CDP_PORT", 9333);
    g_embed = env_is("TEBEOS_EMBED", "1");
    ver = cdp_http_get("127.0.0.1", g_port, "/json/version");
    if (ver && g_embed) {
        /* embed mode: every Medula starts with a fresh Chromium. One left by an earlier Medula has
         * lost its window -- X destroys a window's children with it, and the helper had put
         * Chromium's window inside that Medula -- so there would be nothing to embed */
        JVal *old = j_parse(ver, &off);
        const char *ows = old ? j_str(j_get(old, "webSocketDebuggerUrl")) : NULL;
        CdpConn *oc = ows ? cdp_connect_wsurl(ows) : NULL;
        if (oc) {
            cdp_command(oc, "Browser.close", "{}");
            cdp_close(oc);
        }
        j_free(old);
        free(ver);
        ver = NULL;
        off = 0;
        for (i = 0; i < 50; i++) {      /* up to 5 s for the port to go quiet */
            Sleep(100);
            if (!(ver = cdp_http_get("127.0.0.1", g_port, "/json/version")))
                break;
            free(ver);
            ver = NULL;
        }
        shim_log("BOOT embed mode: closed the Chromium left on port %d", g_port);
    }
    if (!ver && !g_launched) {
        g_launched = 1;
        launch_chromium(g_port);
        for (i = 0; i < 200 && !ver; i++) {
            Sleep(100);
            ver = cdp_http_get("127.0.0.1", g_port, "/json/version");
        }
    }
    if (!ver) {
        shim_log("BOOT no Chromium on port %d", g_port);
        return -1;
    }
    v = j_parse(ver, &off);
    free(ver);
    ws = v ? j_str(j_get(v, "webSocketDebuggerUrl")) : NULL;
    ua = v ? j_str(j_get(v, "User-Agent")) : NULL;
    if (!ws || !(g_browser = cdp_connect_wsurl(ws))) {
        shim_log("BOOT cannot connect to %s", ws ? ws : "(no browser url)");
        j_free(v);
        return -1;
    }
    cdp_set_timeout(g_browser, 10000);
    if (ua) {
        /* the headless UA says "HeadlessChrome"; present the normal one */
        const char *h = strstr(ua, "HeadlessChrome");
        g_user_agent = h ? sfmt("%.*sChrome%s", (int)(h - ua), ua, h + 14) : dup_utf8(ua);
    }
    j_free(v);

    /* embed mode: no separate context -- a target in a new context opens a new browser window,
     * which is not the app window the helper embeds (verified with Target.createTarget) */
    if (!g_embed && cdp_call(g_browser, 0, "Target.createBrowserContext", "{\"disposeOnDetach\":true}", &reply) == 0 &&
        (ctx = j_str(j_get(j_get(reply, "result"), "browserContextId"))))
        snprintf(g_context, sizeof(g_context), "%s", ctx);
    else
        shim_log(g_embed ? "BOOT embed mode: default context (the app window's page)"
                         : "BOOT createBrowserContext failed; using the default context");
    j_free(reply);
    shim_log("BOOT chromium port %d context %s", g_port, g_context[0] ? g_context : "(default)");
    return 0;
}

/* ------------------------------------------------------------------ lifetime */

static Page *page_alloc(void)
{
    Page *pg = (Page *)calloc(1, sizeof(*pg));
    if (!pg)
        return NULL;
    pg->ref = 1;
    pg->stash_base = STASH_BLOCK;
    InitializeCriticalSection(&pg->lock);
    return pg;
}

static void armed_clear(Armed *a)
{
    free(a->post_b64);
    free(a->headers);
    memset(a, 0, sizeof(*a));
}

void page_addref(Page *pg)
{
    if (pg)
        InterlockedIncrement(&pg->ref);
}

void page_release(Page *pg)
{
    if (!pg || InterlockedDecrement(&pg->ref) != 0)
        return;
    if (pg->dom)
        cdp_close(pg->dom);
    if (pg->evt)
        cdp_close(pg->evt);
    render_free(pg->render);
    armed_clear(&pg->armed);
    free(pg->source);
    DeleteCriticalSection(&pg->lock);
    free(pg);
}

CdpConn *page_dom(Page *pg) { return pg ? pg->dom : NULL; }
CdpConn *page_evt(Page *pg) { return pg ? pg->evt : NULL; }

void page_evt_drain(Page *pg, DWORD timeout_ms)
{
    DWORD t0 = GetTickCount();
    long last;

    if (!pg || !pg->evt || !pg->running)
        return;
    last = cdp_last_id(pg->evt);
    while (pg->evt_acked < last && GetTickCount() - t0 < timeout_ms)
        Sleep(1);
}
HTMLDocument *page_doc(Page *pg) { return pg ? pg->doc : NULL; }
struct RenderCtx *page_render(Page *pg) { return pg ? pg->render : NULL; }
struct HtmlEventObj *page_current_event(Page *pg) { return pg ? pg->cur_event : NULL; }

void page_set_current_event(Page *pg, struct HtmlEventObj *evo)
{
    if (pg)
        pg->cur_event = evo;
}

void page_set_doc(Page *pg, HTMLDocument *doc)
{
    if (pg)
        pg->doc = doc;
}

long page_stash_base(Page *pg)
{
    return pg ? pg->stash_base : STASH_BLOCK;
}

void page_stash_used(Page *pg, long id)
{
    /* the id a document started its stash with: the next document gets the next block */
    if (pg && id == pg->stash_base)
        pg->stash_base = pg->stash_base >= 0x7FF00000L - STASH_BLOCK ? STASH_BLOCK : pg->stash_base + STASH_BLOCK;
}

/* SHIM_TRACE=1 logs every synchronous call on the dom connection with its duration, every
 * event the UI thread handles, and each Fire* into ieframe: the last ">>" without a "<<" is
 * where the UI thread is stuck. */
static int tracing(void)
{
    return shim_tracing();
}

/* Request profile: every synchronous dom call is counted under its "shape" (the params with
 * digit runs folded to '#'), with its time. At each page load (after the vendor's
 * DocumentComplete handler) and before each navigation, a load that made many calls logs its
 * busiest shapes ("PROF" lines), so slow pages can be traced to the vendor code that makes
 * them slow without SHIM_TRACE. Costs a hash and a few compares per call (~1 us vs ~1 ms). */
#define PROF_SLOTS 512
#define PROF_SAMPLE 200
typedef struct ProfSlot {
    unsigned long long h;
    long n;
    double ms;
    char sample[PROF_SAMPLE];
} ProfSlot;
static ProfSlot g_prof[PROF_SLOTS];
static long g_prof_n, g_prof_other;
static double g_prof_ms;
static CRITICAL_SECTION g_prof_lock;
static INIT_ONCE g_prof_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK prof_init(INIT_ONCE *o, void *p, void **c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_prof_lock);
    return TRUE;
}

static void prof_add(const char *method, const char *params, double ms)
{
    unsigned long long h = 1469598103934665603ULL;
    char norm[4096];
    size_t n = 0, len;
    const char *q;
    int i, k, dig = 0;

    for (q = method; *q; q++)
        h = (h ^ (unsigned char)*q) * 1099511628211ULL;
    for (q = params ? params : ""; *q; q++) {
        char c = *q >= '0' && *q <= '9' ? '#' : *q;
        if (c == '#' && dig)
            continue;
        dig = c == '#';
        h = (h ^ (unsigned char)c) * 1099511628211ULL;
        if (n < sizeof(norm) - 1)
            norm[n++] = c;
    }
    norm[n] = 0;
    InitOnceExecuteOnce(&g_prof_once, prof_init, NULL, NULL);
    EnterCriticalSection(&g_prof_lock);
    g_prof_n++;
    g_prof_ms += ms;
    for (i = 0; i < PROF_SLOTS; i++) {
        k = (int)((h + (unsigned long long)i) % PROF_SLOTS);
        if (g_prof[k].n && g_prof[k].h != h)
            continue;
        if (!g_prof[k].n) {     /* the distinguishing part is at the end (after STASH_DEF etc.) */
            len = strlen(norm);
            g_prof[k].h = h;
            snprintf(g_prof[k].sample, PROF_SAMPLE, "%s %s", method,
                     len > PROF_SAMPLE - 40 ? norm + len - (PROF_SAMPLE - 40) : norm);
        }
        g_prof[k].n++;
        g_prof[k].ms += ms;
        LeaveCriticalSection(&g_prof_lock);
        return;
    }
    g_prof_other++;
    LeaveCriticalSection(&g_prof_lock);
}

static void prof_dump(const char *what)
{
    int i, j, top[12], nt = 0;

    InitOnceExecuteOnce(&g_prof_once, prof_init, NULL, NULL);
    EnterCriticalSection(&g_prof_lock);
    if (g_prof_n >= 300) {
        shim_log("PROF %s: %ld calls, %.0f ms in calls (%ld in overflowed shapes)",
                 what, g_prof_n, g_prof_ms, g_prof_other);
        for (j = 0; j < 12; j++) {
            int best = -1;
            for (i = 0; i < PROF_SLOTS; i++) {
                int used = 0, t;
                for (t = 0; t < nt; t++)
                    used |= top[t] == i;
                if (g_prof[i].n && !used && (best < 0 || g_prof[i].ms > g_prof[best].ms))
                    best = i;
            }
            if (best < 0)
                break;
            top[nt++] = best;
            shim_log("PROF   %6ld x %7.0f ms  %s", g_prof[best].n, g_prof[best].ms, g_prof[best].sample);
        }
    }
    memset(g_prof, 0, sizeof(g_prof));
    g_prof_n = g_prof_other = 0;
    g_prof_ms = 0;
    LeaveCriticalSection(&g_prof_lock);
}

static volatile LONG g_dom_calls;

/* Test hook: synchronous dom calls made so far (domtest counts what a scan costs). */
__declspec(dllexport) LONG __cdecl shim_dom_calls(void)
{
    return g_dom_calls;
}

int page_call(Page *pg, const char *method, const char *params, JVal **reply)
{
    DWORD t0;
    int rc;
    LARGE_INTEGER f, a, b;

    *reply = NULL;
    if (!pg || !pg->dom)
        return -1;
    InterlockedIncrement(&g_dom_calls);
    if (!rc_pure())
        rc_bump();  /* anything but a known pure read may change the page (rcache.c) */
    if (!tracing()) {
        QueryPerformanceCounter(&a);
        rc = cdp_call(pg->dom, 0, method, params, reply);
        QueryPerformanceCounter(&b);
        QueryPerformanceFrequency(&f);
        prof_add(method, params, (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart);
        return rc;
    }
    {
        /* the start and the end: the shim's helper code fills the middle of most requests,
         * and the end names the element, property or id asked for */
        size_t n = params ? strlen(params) : 0;
        if (n <= 400)
            shim_log(">> %s %s", method, params ? params : "");
        else
            shim_log(">> %s %.100s ... %s", method, params, params + n - 300);
    }
    t0 = GetTickCount();
    rc = cdp_call(pg->dom, 0, method, params, reply);
    {
        /* the value, shortened: enough to see what the vendor's code was told */
        JVal *res = j_get(*reply, "result"), *v = j_get(j_get(res, "result"), "value");
        if (j_get(res, "exceptionDetails"))
            shim_log("<< exception: %.200s", j_str(j_get(j_get(j_get(res, "exceptionDetails"), "exception"), "description")) ? j_str(j_get(j_get(j_get(res, "exceptionDetails"), "exception"), "description")) : "?");
        if (v && v->t == J_STR)
            shim_log("<< %s rc=%d %lums \"%.120s\" (%lu)", method, rc,
                     (unsigned long)(GetTickCount() - t0), v->s, (unsigned long)strlen(v->s));
        else if (v && (v->t == J_NUM || v->t == J_BOOL))
            shim_log("<< %s rc=%d %lums %g", method, rc, (unsigned long)(GetTickCount() - t0),
                     v->t == J_NUM ? v->num : (double)v->b);
        else
            shim_log("<< %s rc=%d %lums%s", method, rc, (unsigned long)(GetTickCount() - t0),
                     v && v->t == J_NULL ? " null" : "");
    }
    return rc;
}

/* ------------------------------------------------------------------ Fetch helpers (any thread) */

static void fetch_continue(Page *pg, const char *rid)
{
    char *q = j_quote(rid);
    char *p = sfmt("{\"requestId\":%s}", q);
    cdp_command(pg->evt, "Fetch.continueRequest", p);
    free(p);
    free(q);
}

static void fetch_fail(Page *pg, const char *rid)
{
    char *q = j_quote(rid);
    char *p = sfmt("{\"requestId\":%s,\"errorReason\":\"Aborted\"}", q);
    cdp_command(pg->evt, "Fetch.failRequest", p);
    free(p);
    free(q);
}

static int header_overridden(const char *extra, const char *name)
{
    size_t n = strlen(name);
    const char *l = extra;
    while (l && *l) {
        while (*l == '\r' || *l == '\n' || *l == ' ')
            l++;
        if (_strnicmp(l, name, n) == 0 && l[n] == ':')
            return 1;
        l = strchr(l, '\n');
    }
    return 0;
}

/* Let the armed request through with ieframe's POST body and extra headers. */
static void fetch_continue_armed(Page *pg, const char *rid, JVal *request, Armed *a)
{
    JVal *hdrs = request ? j_get(request, "headers") : NULL;
    size_t cap = 256, len = 0, i;
    char *arr, *q, *p;
    const char *l;

    if (!a->post_b64 && !(a->headers && *a->headers)) {
        fetch_continue(pg, rid);
        return;
    }
    arr = (char *)malloc(cap);
    if (!arr)
        return;
    arr[0] = '\0';
#define ARR_ADD(NAME, VALUE) do { \
        char *qn_ = j_quote(NAME), *qv_ = j_quote(VALUE); \
        char *it_ = sfmt("%s{\"name\":%s,\"value\":%s}", len ? "," : "", qn_, qv_); \
        size_t il_ = it_ ? strlen(it_) : 0; \
        if (len + il_ + 1 > cap) { while (len + il_ + 1 > cap) cap *= 2; arr = (char *)realloc(arr, cap); } \
        if (arr && it_) { memcpy(arr + len, it_, il_ + 1); len += il_; } \
        free(it_); free(qn_); free(qv_); \
    } while (0)
    if (hdrs && hdrs->t == J_OBJ)
        for (i = 0; i < hdrs->n && arr; i++)
            if (!header_overridden(a->headers, hdrs->keys[i]) && j_str(&hdrs->vals[i]))
                ARR_ADD(hdrs->keys[i], j_str(&hdrs->vals[i]));
    for (l = a->headers; l && *l && arr; ) {
        const char *eol = strpbrk(l, "\r\n"), *colon = strchr(l, ':');
        size_t ln = eol ? (size_t)(eol - l) : strlen(l);
        if (colon && (size_t)(colon - l) < ln) {
            char *name = sfmt("%.*s", (int)(colon - l), l);
            const char *v = colon + 1;
            char *value;
            while (*v == ' ' && v < l + ln)
                v++;
            value = sfmt("%.*s", (int)(l + ln - v), v);
            if (name && value && *name)
                ARR_ADD(name, value);
            free(name);
            free(value);
        }
        l += ln;
        while (*l == '\r' || *l == '\n')
            l++;
    }
#undef ARR_ADD
    if (!arr)
        return;
    q = j_quote(rid);
    p = a->post_b64
        ? sfmt("{\"requestId\":%s,\"method\":\"POST\",\"postData\":\"%s\",\"headers\":[%s]}", q, a->post_b64, arr)
        : sfmt("{\"requestId\":%s,\"headers\":[%s]}", q, arr);
    cdp_command(pg->evt, "Fetch.continueRequest", p);
    free(p);
    free(q);
    free(arr);
}

/* ------------------------------------------------------------------ the pump (worker thread) */

typedef struct PageMsg {
    Page *pg;
    JVal *m;
} PageMsg;

static void post_ui(Page *pg, JVal *m)
{
    PageMsg *pm = (PageMsg *)malloc(sizeof(*pm));
    if (!pm) {
        j_free(m);
        return;
    }
    page_addref(pg);
    pm->pg = pg;
    pm->m = m;
    if (!pg->msgwnd || !PostMessageW(pg->msgwnd, WM_PAGE_EVENT, 0, (LPARAM)pm)) {
        shim_log("page %p: dropped %s (no message window)", (void *)pg, j_str(j_get(m, "method")));
        j_free(m);
        page_release(pg);
        free(pm);
    }
}

static void pump_request_paused(Page *pg, JVal *m)
{
    JVal *p = j_get(m, "params");
    JVal *req = j_get(p, "request");
    const char *rid = j_str(j_get(p, "requestId"));
    const char *frame = j_str(j_get(p, "frameId"));
    const char *url = j_str(j_get(req, "url"));
    int main_frame = frame && strcmp(frame, pg->target_id) == 0;
    int redirect = j_get(p, "redirectedRequestId") != NULL;
    Armed a;

    if (!rid) {
        j_free(m);
        return;
    }
    memset(&a, 0, sizeof(a));
    if (main_frame && !redirect) {
        EnterCriticalSection(&pg->lock);
        if (pg->armed.active) {
            a = pg->armed;
            memset(&pg->armed, 0, sizeof(pg->armed));
        }
        LeaveCriticalSection(&pg->lock);
        if (a.active) {
            shim_log("NAV armed %s%s", a.post_b64 ? "POST " : "", url ? url : "?");
            fetch_continue_armed(pg, rid, req, &a);
            armed_clear(&a);
            j_free(m);
            return;
        }
        if (pg->doc) {
            /* While a navigation is pending, Chromium holds every Runtime/DOM message for the
             * renderer, so a BeforeNavigate2 handler could not read the page it is leaving
             * (each call timed out). Abort it, let BeforeNavigate2 decide on the UI thread,
             * and issue it again if allowed. */
            fetch_fail(pg, rid);
            post_ui(pg, m);
            return;
        }
    }
    if (main_frame)
        shim_log("NAV %s%s", redirect ? "redirect " : "", url ? url : "?");
    fetch_continue(pg, rid);
    j_free(m);
}

static void pump_dialog(Page *pg, JVal *m)
{
    JVal *p = j_get(m, "params");
    const char *type = j_str(j_get(p, "type"));
    const char *msg = j_str(j_get(p, "message"));
    const char *def = j_str(j_get(p, "defaultPrompt"));
    int automatic = env_is("SHIM_DIALOGS", "auto");
    int accept = 1;
    char *params, *q;

    shim_log("DIALOG %s: %.300s", type ? type : "?", msg ? msg : "");
    if (type && strcmp(type, "alert") != 0 && strcmp(type, "prompt") != 0 && !automatic) {
        WCHAR *w = utf8_to_wstr(msg ? msg : "");
        /* confirm/beforeunload: the page is blocked until it is answered anyway */
        accept = MessageBoxW(NULL, w ? w : L"", L"Medula",
                             MB_OKCANCEL | MB_ICONQUESTION | MB_TASKMODAL | MB_SETFOREGROUND | MB_TOPMOST) == IDOK;
        free(w);
    }
    q = j_quote(def ? def : "");
    params = sfmt("{\"accept\":%s,\"promptText\":%s}", accept ? "true" : "false", q);
    cdp_command(pg->evt, "Page.handleJavaScriptDialog", params);
    free(params);
    free(q);
    if (type && strcmp(type, "alert") == 0 && !automatic && pg->msgwnd) {
        /* answered at once so the page (and any evaluate waiting on it) goes on; shown after */
        WCHAR *w = utf8_to_wstr(msg ? msg : "");
        if (w && !PostMessageW(pg->msgwnd, WM_PAGE_ALERT, 0, (LPARAM)w))
            free(w);
    }
    j_free(m);
}

static void ack_frame(Page *pg, JVal *params)
{
    JVal *sid = j_get(params, "sessionId");
    char p[64];
    if (!sid)
        return;
    snprintf(p, sizeof(p), "{\"sessionId\":%.0f}", j_num(sid));
    cdp_command(pg->evt, "Page.screencastFrameAck", p);
}

static DWORD WINAPI page_pump(LPVOID arg)
{
    Page *pg = (Page *)arg;

    while (pg->running) {
        JVal *m = NULL;
        const char *method;
        int rc = cdp_recv(pg->evt, &m);
        if (rc == -2)
            continue;
        if (rc != 0) {
            if (pg->running)
                shim_log("page %p: event connection lost", (void *)pg);
            break;
        }
        method = j_str(j_get(m, "method"));
        if (!method) {
            LONG id = (LONG)j_num(j_get(m, "id"));
            if (id > pg->evt_acked)
                InterlockedExchange(&pg->evt_acked, id);
            if (tracing() && j_get(m, "error"))
                shim_log("evt reply %ld: %s", (long)j_num(j_get(m, "id")),
                         j_str(j_get(j_get(m, "error"), "message")));
            j_free(m);          /* the reply to a fire-and-forget command */
        } else if (!strcmp(method, "Page.screencastFrame")) {
            if (tracing())
                shim_log("FRAME render %p", (void *)pg->render);
            render_handle_frame(pg->render, j_get(m, "params"));
            ack_frame(pg, j_get(m, "params"));
            j_free(m);
        } else if (!strcmp(method, "Fetch.requestPaused")) {
            pump_request_paused(pg, m);
        } else if (!strcmp(method, "Page.javascriptDialogOpening")) {
            pump_dialog(pg, m);
        } else if (!strcmp(method, "Runtime.bindingCalled") ||
                   !strcmp(method, "Page.frameNavigated") ||
                   !strcmp(method, "Page.navigatedWithinDocument") ||
                   !strcmp(method, "Page.domContentEventFired") ||
                   !strcmp(method, "Page.loadEventFired")) {
            post_ui(pg, m);
        } else {
            if (!strcmp(method, "Inspector.targetCrashed") || !strcmp(method, "Inspector.detached"))
                shim_log("page %p: %s", (void *)pg, method);
            j_free(m);
        }
    }
    page_release(pg);   /* the pump's reference */
    return 0;
}

/* ------------------------------------------------------------------ UI thread */

static void ui_before_navigate(Page *pg, HTMLDocument *d, JVal *p)
{
    JVal *req = j_get(p, "request");
    const char *url = j_str(j_get(req, "url"));
    const char *method = j_str(j_get(req, "method"));
    JVal *entries = j_get(req, "postDataEntries");
    const char *post_text = j_str(j_get(req, "postData"));
    const char *ctype = j_str(j_get(j_get(req, "headers"), "Content-Type"));
    BYTE *post = NULL;
    DWORD postlen = 0;
    WCHAR *wurl, *whdr = NULL;
    BOOL cancel = FALSE;
    size_t i;

    if (method && !strcmp(method, "POST")) {
        if (entries && entries->t == J_ARR) {
            size_t cap = 0;
            for (i = 0; i < entries->n; i++) {
                const char *b = j_str(j_get(&entries->items[i], "bytes"));
                if (b)
                    cap += strlen(b) / 4 * 3 + 3;
            }
            post = (BYTE *)malloc(cap + 1);
            for (i = 0; post && i < entries->n; i++) {
                const char *b = j_str(j_get(&entries->items[i], "bytes"));
                int n = b ? b64_decode(b, post + postlen, cap - postlen) : 0;
                if (n > 0)
                    postlen += (DWORD)n;
            }
        } else if (post_text) {
            postlen = (DWORD)strlen(post_text);
            post = (BYTE *)dup_utf8(post_text);
        }
        if (ctype) {
            char *h = sfmt("Content-Type: %s\r\n", ctype);
            whdr = utf8_to_wstr(h);
            free(h);
        }
    }
    wurl = utf8_to_wstr(url ? url : "");
    html_doc_addref(d);
    if (d->doc_object_service)
        d->doc_object_service->lpVtbl->FireBeforeNavigate2(d->doc_object_service, NULL, wurl, 0x40, NULL,
                                                             post, postlen, whdr, TRUE, &cancel);
    shim_log("NAV %s%s%s", cancel ? "CANCELLED " : "", postlen ? "POST " : "", url ? url : "?");
    if (!cancel && url) {
        /* The pump aborted it: go again, armed with the same body, Content-Type, Origin and
         * referrer. Page.navigate sends no Origin, and SGK's ReceteSorgu.jsp silently ignored
         * a POST without one (login.jsp did not). Sec-Fetch-Site stays "none": Chromium
         * recomputes it whatever continueRequest says. */
        const char *origin = j_str(j_get(j_get(req, "headers"), "Origin"));
        char *h = sfmt("%s%s%s%s%s%s", ctype ? "Content-Type: " : "", ctype ? ctype : "", ctype ? "\r\n" : "",
                       origin ? "Origin: " : "", origin ? origin : "", origin ? "\r\n" : "");
        navigate_armed(pg, url, post, postlen, h, j_str(j_get(j_get(req, "headers"), "Referer")));
        free(h);
    }
    html_doc_release(d);
    free(wurl);
    free(whdr);
    free(post);
}

/* Report what the document has reached and ieframe has not heard yet: NavigateComplete2
 * once it is interactive, then DocumentComplete once it has loaded. Called on each step and
 * again when the site's IDocObjectService arrives (a fast page can get there first). */
static void ui_report(Page *pg, HTMLDocument *d)
{
    IDocObjectService *dos = d->doc_object_service;
    if (!dos || !pg->committed)
        return;
    if (pg->dom_ready && !pg->fired_nc2) {
        pg->fired_nc2 = 1;
        if (tracing())
            shim_log(">> FireNavigateComplete2");
        dos->lpVtbl->FireNavigateComplete2(dos, &d->window.IHTMLWindow2_iface, 0);
        if (tracing())
            shim_log("<< FireNavigateComplete2");
    }
    if (pg->loaded && !pg->fired_dc) {
        pg->fired_dc = 1;
        if (tracing())
            shim_log(">> FireDocumentComplete");
        dos->lpVtbl->FireDocumentComplete(dos, &d->window.IHTMLWindow2_iface, 0);
        if (tracing())
            shim_log("<< FireDocumentComplete");
    }
}

static void page_ui(Page *pg, JVal *m)
{
    const char *method = j_str(j_get(m, "method"));
    JVal *p = j_get(m, "params");
    HTMLDocument *d = pg->doc;

    if (!strcmp(method, "Runtime.bindingCalled")) {
        const char *name = j_str(j_get(p, "name"));
        const char *payload = j_str(j_get(p, "payload"));
        if (name && payload && !strcmp(name, "__shim_evt")) {
            if (!strncmp(payload, "{\"t\":\"__print\"", 14)) {   /* iecompat.js §6 */
                if (d)
                    html_doc_print_later(d);
            } else
                shim_dom_event_dispatch(pg, payload);
        }
        return;
    }
    if (!strcmp(method, "Fetch.requestPaused")) {
        if (d)
            ui_before_navigate(pg, d, p);   /* the pump has aborted it already */
        return;
    }
    if (!d || pg->legacy)
        return;

    html_doc_addref(d);
    if (!strcmp(method, "Page.frameNavigated")) {
        JVal *frame = j_get(p, "frame");
        const char *url = j_str(j_get(frame, "url"));
        const char *loader = j_str(j_get(frame, "loaderId"));
        if (j_str(j_get(frame, "unreachableUrl")))
            url = j_str(j_get(frame, "unreachableUrl"));    /* Chromium's error page for it */
        if (!j_get(frame, "parentId") && url &&
            (strcmp(url, "about:blank") != 0 || pg->blank_expected)) {
            WCHAR *w = utf8_to_wstr(url);
            if (!strcmp(url, "about:blank"))
                pg->blank_expected = 0;
            pg->committed = 1;
            pg->dom_ready = pg->loaded = 0;
            pg->fired_nc2 = pg->fired_dc = 0;
            snprintf(pg->loader_id, sizeof(pg->loader_id), "%s", loader ? loader : "");
            set_source(pg, NULL, 0);
            html_doc_set_url(d, w);
            free(w);
            dom_events_reset(pg);
            html_doc_set_ready_state(d, L"loading");
            shim_log("NAV commit %s", url);
            prof_dump("before this page");
        }
    } else if (!strcmp(method, "Page.navigatedWithinDocument")) {
        const char *frame = j_str(j_get(p, "frameId"));
        const char *url = j_str(j_get(p, "url"));
        if (frame && url && !strcmp(frame, pg->target_id)) {
            WCHAR *w = utf8_to_wstr(url);
            html_doc_set_url(d, w);
            free(w);
        }
    } else if (!strcmp(method, "Page.domContentEventFired")) {
        if (pg->committed && !pg->dom_ready) {
            size_t n = 0;
            char *src = fetch_source(pg, &n);
            set_source(pg, src, n);     /* the response is complete once the parser is done */
            pg->dom_ready = 1;
            html_doc_set_ready_state(d, L"interactive");
            ui_report(pg, d);
        }
    } else if (!strcmp(method, "Page.loadEventFired")) {
        if (pg->committed && !pg->loaded) {
            pg->dom_ready = pg->loaded = 1;
            html_doc_set_ready_state(d, L"complete");
            ui_report(pg, d);
            shim_log("NAV complete");
            prof_dump("page load");
        }
    }
    html_doc_release(d);
}

static LRESULT CALLBACK page_wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_PAGE_EVENT) {
        PageMsg *pm = (PageMsg *)l;
        rc_bump();  /* navigation, load, a vendor event: the page may have changed */
        if (tracing())
            shim_log("UI %s", j_str(j_get(pm->m, "method")));
        page_ui(pm->pg, pm->m);
        j_free(pm->m);
        page_release(pm->pg);
        free(pm);
        return 0;
    }
    if (msg == WM_PAGE_CATCHUP || msg == WM_PAGE_COMPLETE) {
        Page *pg = (Page *)l;
        HTMLDocument *d = pg->doc;
        if (d && !pg->legacy) {
            html_doc_addref(d);
            if (msg == WM_PAGE_COMPLETE) {
                /* IPersistStreamInit::Load finished: IE reports DocumentComplete alone */
                html_doc_set_ready_state(d, L"complete");
                if (d->doc_object_service)
                    d->doc_object_service->lpVtbl->FireDocumentComplete(d->doc_object_service,
                                                                        &d->window.IHTMLWindow2_iface, 0);
            } else {
                ui_report(pg, d);
            }
            html_doc_release(d);
        }
        page_release(pg);
        return 0;
    }
    if (msg == WM_PAGE_FOCUS) {
        Page *pg = (Page *)l;
        if (tracing())
            shim_log("UI focus replay");
        if (pg->doc)
            dom_exec(pg, replay_focus_js);
        page_release(pg);
        return 0;
    }
    if (msg == WM_PAGE_ALERT) {
        WCHAR *text = (WCHAR *)l;
        MessageBoxW(NULL, text, L"Medula", MB_OK | MB_ICONINFORMATION | MB_TASKMODAL | MB_SETFOREGROUND);
        free(text);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

static HWND page_msgwnd(void)
{
    static ATOM cls;
    if (!cls) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = page_wndproc;
        wc.hInstance = shim_hinst;
        wc.lpszClassName = L"ShimPageEvents";
        cls = RegisterClassW(&wc);
    }
    return CreateWindowExW(0, L"ShimPageEvents", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, shim_hinst, NULL);
}

/* The DOM-event binding (window.__shim_evt). Chromium keeps a binding across navigations
 * only on a session with Runtime enabled: without it the next document has no binding and
 * every event is lost (measured on Chromium 153). */
static void add_binding(CdpConn *c)
{
    JVal *reply = NULL;
    if (cdp_call(c, 0, "Runtime.enable", "{}", &reply) == 0)
        j_free(reply);
    if (cdp_call(c, 0, "Runtime.addBinding", "{\"name\":\"__shim_evt\"}", &reply) == 0)
        j_free(reply);
}

/* IE behaviour Chromium lacks, run in every new document of the tab: untyped <button>s
 * become type=button, and <select> drop-downs open a list drawn in the page (headless
 * Chromium never shows its own popup). See iecompat.js. */
static void add_ie_compat(CdpConn *c)
{
    JVal *reply = NULL;
    char *q, *params;
    /* the script is silently never run unless Page is enabled on this session (measured on
     * Chromium 153); page_create has already done it, the test harness tab has not */
    if (cdp_call(c, 0, "Page.enable", "{}", &reply) == 0)
        j_free(reply);
    reply = NULL;
    q = j_quote(IECOMPAT_JS);
    params = q ? sfmt("{\"source\":%s}", q) : NULL;
    free(q);
    if (params && cdp_call(c, 0, "Page.addScriptToEvaluateOnNewDocument", params, &reply) == 0)
        j_free(reply);
    free(params);
}

/* Start the pump; the message window belongs to the calling (UI) thread. */
static int page_start(Page *pg)
{
    DWORD tid = 0;
    pg->msgwnd = page_msgwnd();
    if (!pg->render)
        pg->render = render_new();
    cdp_set_timeout(pg->evt, 1000);
    pg->running = 1;
    page_addref(pg);
    pg->thread = CreateThread(NULL, 0, page_pump, pg, 0, &tid);
    if (pg->thread)
        shim_log("page %p: pump thread %04lx", (void *)pg, tid);
    if (!pg->thread) {
        pg->running = 0;
        page_release(pg);
        return -1;
    }
    return 0;
}

static void page_stop(Page *pg)
{
    MSG msg;

    if (pg->thread) {
        InterlockedExchange(&pg->running, 0);
        cdp_shutdown(pg->evt);
        if (WaitForSingleObject(pg->thread, 5000) != WAIT_OBJECT_0)
            shim_log("page %p: pump did not stop", (void *)pg);
        CloseHandle(pg->thread);
        pg->thread = NULL;
    }
    if (pg->msgwnd) {
        while (PeekMessageW(&msg, pg->msgwnd, WM_PAGE_EVENT, WM_PAGE_FOCUS, PM_REMOVE)) {
            if (msg.message == WM_PAGE_EVENT) {
                PageMsg *pm = (PageMsg *)msg.lParam;
                j_free(pm->m);
                page_release(pm->pg);
                free(pm);
            } else if (msg.message == WM_PAGE_ALERT) {
                free((void *)msg.lParam);
            } else {
                page_release((Page *)msg.lParam);
            }
        }
        DestroyWindow(pg->msgwnd);
        pg->msgwnd = NULL;
    }
}

/* ------------------------------------------------------------------ public */

/* embed mode: a page of the app window to take over. First its start page (url data:), else a page
 * an earlier Medula left there (the shim does not close pages in embed mode: closing the last one
 * would close the app window). Never the one a Page of this process already holds. */
static const char *embed_app_tid(JVal *reply)
{
    JVal *arr = j_get(j_get(reply, "result"), "targetInfos");
    size_t i;
    int pass;
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; arr && i < arr->n; i++) {
            JVal *t = &arr->items[i];
            const char *type = j_str(j_get(t, "type"));
            const char *url = j_str(j_get(t, "url"));
            const char *id = j_str(j_get(t, "targetId"));
            if (!type || !url || !id || strcmp(type, "page") || !strcmp(id, g_embed_busy))
                continue;
            if (pass == 0 ? !strncmp(url, "data:", 5)
                          : strncmp(url, "chrome", 6) && strncmp(url, "devtools", 8))
                return id;
        }
    }
    return NULL;
}

Page *page_create(HTMLDocument *doc)
{
    Page *pg;
    JVal *reply = NULL;
    const char *tid;
    char *q, *params, *ws;
    int rc;

    InitOnceExecuteOnce(&g_boot_once, boot_init, NULL, NULL);
    EnterCriticalSection(&g_boot_lock);
    if (boot_locked() != 0) {
        LeaveCriticalSection(&g_boot_lock);
        return NULL;
    }
    q = j_quote(g_context);
    params = g_context[0] ? sfmt("{\"url\":\"about:blank\",\"browserContextId\":%s}", q)
                          : sfmt("{\"url\":\"about:blank\"}");
    tid = NULL;
    if (g_embed) {
        /* embed: a page of the app window (launched with --app=data:...). Target.createTarget
         * would open a normal browser window with tabs and an address bar */
        rc = cdp_call(g_browser, 0, "Target.getTargets", "{}", &reply);
        tid = rc == 0 ? embed_app_tid(reply) : NULL;
        if (tid) {
            snprintf(g_embed_busy, sizeof(g_embed_busy), "%s", tid);
        } else {
            shim_log("EMBED no free page in the app window; opening a normal window");
            j_free(reply);
            reply = NULL;
        }
    }
    if (!tid)
        rc = cdp_call(g_browser, 0, "Target.createTarget", params, &reply);
    free(params);
    free(q);
    LeaveCriticalSection(&g_boot_lock);
    if (!tid)
        tid = rc == 0 ? j_str(j_get(j_get(reply, "result"), "targetId")) : NULL;
    if (!tid) {
        shim_log("PAGE createTarget failed");
        j_free(reply);
        return NULL;
    }

    pg = page_alloc();
    if (!pg) {
        j_free(reply);
        return NULL;
    }
    pg->doc = doc;
    snprintf(pg->target_id, sizeof(pg->target_id), "%s", tid);
    j_free(reply);

    ws = sfmt("ws://127.0.0.1:%d/devtools/page/%s", g_port, pg->target_id);
    pg->dom = ws ? cdp_connect_wsurl(ws) : NULL;
    pg->evt = ws ? cdp_connect_wsurl(ws) : NULL;
    free(ws);
    if (!pg->dom || !pg->evt) {
        shim_log("PAGE cannot connect to tab %s", pg->target_id);
        page_close(pg);
        page_release(pg);
        return NULL;
    }
    cdp_set_timeout(pg->dom, 10000);

    /* dom: keep response bodies for Save, present a normal browser */
    if (page_call(pg, "Network.enable", "{}", &reply) == 0)
        j_free(reply);
    if (g_user_agent) {
        char *qu = j_quote(g_user_agent);
        params = sfmt("{\"userAgent\":%s,\"acceptLanguage\":\"tr-TR,tr;q=0.9,en;q=0.5\"}", qu);
        if (page_call(pg, "Network.setUserAgentOverride", params, &reply) == 0)
            j_free(reply);
        free(params);
        free(qu);
    }
    /* evt: everything the pump listens to, enabled before it starts reading */
    if (cdp_call(pg->evt, 0, "Page.enable", "{}", &reply) == 0)
        j_free(reply);
    add_binding(pg->evt);
    add_ie_compat(pg->evt);
    if (cdp_call(pg->evt, 0, "Fetch.enable",
                 "{\"patterns\":[{\"urlPattern\":\"*\",\"resourceType\":\"Document\",\"requestStage\":\"Request\"}]}",
                 &reply) == 0)
        j_free(reply);

    if (page_start(pg) != 0) {
        page_close(pg);
        page_release(pg);
        return NULL;
    }
    shim_log("PAGE %p tab %s for doc %p", (void *)pg, pg->target_id, (void *)doc);
    return pg;
}

static void post_self(Page *pg, UINT msg)
{
    page_addref(pg);
    if (!pg->msgwnd || !PostMessageW(pg->msgwnd, msg, 0, (LPARAM)pg))
        page_release(pg);
}

void page_catch_up(Page *pg)
{
    if (pg && !pg->legacy)
        post_self(pg, WM_PAGE_CATCHUP);
}

void page_replay_focus(Page *pg)
{
    if (!pg)
        return;
    if (pg->legacy)
        dom_exec(pg, replay_focus_js);
    else
        post_self(pg, WM_PAGE_FOCUS);
}

void page_close(Page *pg)
{
    if (!pg)
        return;
    pg->doc = NULL;
    if (pg->legacy)
        return;
    page_stop(pg);
    dom_events_reset(pg);
    if (g_embed && pg->target_id[0]) {
        /* keep the page: it is the app window's; the next Medula takes it over (embed_app_tid) */
        EnterCriticalSection(&g_boot_lock);
        if (!strcmp(g_embed_busy, pg->target_id))
            g_embed_busy[0] = '\0';
        LeaveCriticalSection(&g_boot_lock);
        pg->target_id[0] = '\0';
    }
    if (pg->target_id[0] && g_browser) {
        JVal *reply = NULL;
        char *q = j_quote(pg->target_id);
        char *p = sfmt("{\"targetId\":%s}", q);
        EnterCriticalSection(&g_boot_lock);
        if (cdp_call(g_browser, 0, "Target.closeTarget", p, &reply) == 0)
            j_free(reply);
        LeaveCriticalSection(&g_boot_lock);
        free(p);
        free(q);
        pg->target_id[0] = '\0';
    }
    if (pg->dom) {
        cdp_close(pg->dom);
        pg->dom = NULL;
    }
    if (pg->evt && !pg->thread) {
        cdp_close(pg->evt);
        pg->evt = NULL;
    }
}

static int navigate_armed(Page *pg, const char *url, const BYTE *post, DWORD postlen,
                          const char *headers, const char *referrer)
{
    char *q, *qr, *p;
    int rc;

    if (!pg || !pg->evt || !url)
        return -1;
    if (!_strnicmp(url, "javascript:", 11)) {
        dom_exec(pg, url + 11);
        return 0;
    }
    if (!_strnicmp(url, "http:", 5) || !_strnicmp(url, "https:", 6)) {
        EnterCriticalSection(&pg->lock);
        armed_clear(&pg->armed);
        pg->armed.active = 1;
        pg->armed.post_b64 = postlen ? b64_encode(post, postlen) : NULL;
        pg->armed.headers = headers && *headers ? dup_utf8(headers) : NULL;
        LeaveCriticalSection(&pg->lock);
    } else if (!strcmp(url, "about:blank")) {
        pg->blank_expected = 1;
    }
    q = j_quote(url);
    qr = referrer && *referrer ? j_quote(referrer) : NULL;
    p = qr ? sfmt("{\"url\":%s,\"referrer\":%s}", q, qr) : sfmt("{\"url\":%s}", q);
    rc = cdp_command(pg->evt, "Page.navigate", p);
    free(p);
    free(qr);
    free(q);
    shim_log("NAVIGATE %s%s", postlen ? "POST " : "", url);
    return rc;
}

int page_navigate(Page *pg, const char *url, const BYTE *post, DWORD postlen, const char *headers)
{
    return navigate_armed(pg, url, post, postlen, headers, NULL);
}

int page_set_content(Page *pg, const char *html)
{
    JVal *reply = NULL;
    char *q, *qf, *p;
    int rc;

    if (!pg || !pg->dom)
        return -1;
    if (!pg->target_id[0]) {
        if (page_call(pg, "Page.getFrameTree", "{}", &reply) == 0) {
            const char *id = j_str(j_get(j_get(j_get(j_get(reply, "result"), "frameTree"), "frame"), "id"));
            if (id)
                snprintf(pg->target_id, sizeof(pg->target_id), "%s", id);
        }
        j_free(reply);
    }
    q = j_quote(html ? html : "");
    qf = j_quote(pg->target_id);
    p = sfmt("{\"frameId\":%s,\"html\":%s}", qf, q);
    /* the new content is not the response the tab loaded: Save serialises it, and the
     * tab's own load events no longer report anything until the next navigation */
    pg->loader_id[0] = '\0';
    pg->committed = 0;
    set_source(pg, html ? _strdup(html) : NULL, html ? strlen(html) : 0);
    dom_events_reset(pg);
    rc = page_call(pg, "Page.setDocumentContent", p, &reply);
    j_free(reply);
    free(p);
    free(qf);
    free(q);
    if (rc == 0 && !pg->legacy)
        post_self(pg, WM_PAGE_COMPLETE);
    return rc;
}

static UINT charset_cp(const char *cs)
{
    if (!cs || !_stricmp(cs, "UTF-8"))
        return CP_UTF8;
    if (!_strnicmp(cs, "windows-", 8))
        return (UINT)atoi(cs + 8);
    if (!_stricmp(cs, "ISO-8859-9"))
        return 28599;
    if (!_strnicmp(cs, "ISO-8859-", 9))
        return 28590 + (UINT)atoi(cs + 9);
    if (!_stricmp(cs, "US-ASCII"))
        return 20127;
    return CP_UTF8;
}

/* The main document's response body as the server sent it: Chromium hands it back decoded
 * (or base64 for binary), so text is re-encoded into the page's charset. NULL if Chromium no
 * longer has it. */
static char *fetch_source(Page *pg, size_t *len)
{
    JVal *reply = NULL;
    const char *body = NULL;
    char *bytes = NULL, *q, *p;
    int b64 = 0, n = 0;

    *len = 0;
    if (!pg->loader_id[0])
        return NULL;
    /* the main document's Network requestId is its loaderId */
    q = j_quote(pg->loader_id);
    p = sfmt("{\"requestId\":%s}", q);
    if (page_call(pg, "Network.getResponseBody", p, &reply) == 0) {
        JVal *res = j_get(reply, "result");
        body = j_str(j_get(res, "body"));
        b64 = j_is_true(j_get(res, "base64Encoded"));
    }
    free(p);
    free(q);
    if (body && b64) {
        size_t cap = strlen(body) / 4 * 3 + 3;
        bytes = (char *)malloc(cap);
        n = bytes ? b64_decode(body, (unsigned char *)bytes, cap) : 0;
    } else if (body) {
        char *cs = dom_eval_str(pg, "document.characterSet");
        UINT cp = charset_cp(cs);
        WCHAR *w = utf8_to_wstr(body);
        if (w) {
            n = WideCharToMultiByte(cp, 0, w, -1, NULL, 0, NULL, NULL);
            bytes = n > 0 ? (char *)malloc((size_t)n) : NULL;
            if (bytes)
                WideCharToMultiByte(cp, 0, w, -1, bytes, n, NULL, NULL);
            n = n > 0 ? n - 1 : 0;
            free(w);
        }
        free(cs);
    }
    j_free(reply);
    *len = bytes && n > 0 ? (size_t)n : 0;
    return bytes;
}

static void set_source(Page *pg, char *src, size_t len)
{
    free(pg->source);
    pg->source = src;
    pg->source_len = src ? len : 0;
}

/* IE keeps the source of the document on screen, also while the next one loads. */
HRESULT page_save(Page *pg, IStream *stm)
{
    ULONG written;
    HRESULT hr;

    if (!pg || !stm)
        return E_FAIL;
    if (!pg->source) {
        size_t n = 0;
        char *src = fetch_source(pg, &n);
        if (src) {
            set_source(pg, src, n);
        } else {
            char *html = dom_eval_str(pg, "document.documentElement?document.documentElement.outerHTML:''");
            shim_log("SAVE no response body for %s; serialising the DOM", pg->loader_id);
            hr = stm->lpVtbl->Write(stm, html ? html : "", html ? (ULONG)strlen(html) : 0, &written);
            free(html);
            return hr;
        }
    }
    return stm->lpVtbl->Write(stm, pg->source, (ULONG)pg->source_len, &written);
}

static void embed_place(Page *pg)
{
    RECT rc;
    POINT o = { 0, 0 };
    HWND top;
    HANDLE x11;
    JVal *reply = NULL;
    char path[MAX_PATH], *p;
    long wid = -1;
    int w, h;
    FILE *f;

    if (!g_embed || !pg || !pg->embed_hwnd || !IsWindow(pg->embed_hwnd))
        return;
    GetClientRect(pg->embed_hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0)
        return;
    MapWindowPoints(pg->embed_hwnd, NULL, &o, 1);
    top = GetAncestor(pg->embed_hwnd, GA_ROOT);
    x11 = top ? GetPropW(top, L"__wine_x11_whole_window") : NULL;
    if (o.x == pg->embed_x && o.y == pg->embed_y && w == pg->embed_w && h == pg->embed_h &&
        (unsigned long)(ULONG_PTR)x11 == pg->embed_x11)
        return;     /* nothing moved (called every 0.5 s from the shim window's timer) */

    /* only a new size goes to Chromium; a move is the helper's job */
    if (w == pg->embed_w && h == pg->embed_h)
        goto report;
    EnterCriticalSection(&g_boot_lock);
    p = sfmt("{\"targetId\":\"%s\"}", pg->target_id);
    if (p && cdp_call(g_browser, 0, "Browser.getWindowForTarget", p, &reply) == 0)
        wid = (long)j_num(j_get(j_get(reply, "result"), "windowId"));
    j_free(reply);
    reply = NULL;
    free(p);
    if (wid >= 0) {
        p = sfmt("{\"windowId\":%ld,\"bounds\":{\"width\":%d,\"height\":%d}}", wid, w, h);
        if (p && cdp_call(g_browser, 0, "Browser.setWindowBounds", p, &reply) == 0)
            j_free(reply);
        free(p);
    }
    LeaveCriticalSection(&g_boot_lock);

report:
    pg->embed_x = o.x;
    pg->embed_y = o.y;
    pg->embed_w = w;
    pg->embed_h = h;
    pg->embed_x11 = (unsigned long)(ULONG_PTR)x11;
    /* for embed-spike/embed_run.py: the X window to embed into, and the shim window's screen rect.
     * Rewritten on every change, so the screen position always matches Medula's current place */
    if (GetTempPathA(sizeof(path), path) && strlen(path) + 24 < sizeof(path)) {
        strcat(path, "mshtml-cdp-embed.txt");
        if ((f = fopen(path, "w"))) {
            fprintf(f, "%lx %ld %ld %d %d\n", (unsigned long)(ULONG_PTR)x11, (long)o.x, (long)o.y, w, h);
            fclose(f);
        }
    }
    shim_log("EMBED x11 %lx at %ld,%ld size %dx%d window %ld",
             (unsigned long)(ULONG_PTR)x11, (long)o.x, (long)o.y, w, h, wid);
}

int page_embed_mode(void)
{
    return g_embed;
}

/* embed mode, every 0.5 s (shim window timer): report the shim window again if it moved or changed
 * size -- TEBEOS's layout, a moved Medula window, or a resize. Cheap when nothing changed. */
void page_embed_tick(Page *pg)
{
    if (g_embed && pg && !pg->legacy)
        embed_place(pg);
}

void page_resize(Page *pg, int w, int h)
{
    char p[160];
    if (g_embed && pg && !pg->legacy)
        embed_place(pg);        /* also on a move without a new size */
    if (!pg || !pg->evt || pg->legacy || w <= 0 || h <= 0 || (w == pg->width && h == pg->height))
        return;
    pg->width = w;
    pg->height = h;
    snprintf(p, sizeof(p), "{\"width\":%d,\"height\":%d,\"deviceScaleFactor\":1,\"mobile\":false}", w, h);
    cdp_command(pg->evt, "Emulation.setDeviceMetricsOverride", p);
}

void page_attach_window(Page *pg, HWND hwnd)
{
    if (!pg || !pg->evt)
        return;
    if (!pg->render)
        pg->render = render_new();
    if (tracing())
        shim_log("RENDER attach hwnd %p (screencast %d)", (void *)hwnd, pg->screencast);
    render_set_hwnd(pg->render, hwnd);
    if (g_embed) {
        pg->embed_hwnd = hwnd;
        embed_place(pg);
    }
    if (hwnd && !pg->screencast && !g_embed) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        page_resize(pg, rc.right - rc.left, rc.bottom - rc.top);
        cdp_command(pg->evt, "Page.startScreencast",
                    "{\"format\":\"jpeg\",\"quality\":80,\"maxWidth\":4096,\"maxHeight\":4096,\"everyNthFrame\":1}");
        pg->screencast = 1;
    } else if (!hwnd && pg->screencast) {
        cdp_command(pg->evt, "Page.stopScreencast", "{}");
        pg->screencast = 0;
    }
}

/* ------------------------------------------------------------------ legacy (test harness) tab */

static Page *legacy_get(void)
{
    if (!g_legacy && (g_legacy = page_alloc()))
        g_legacy->legacy = 1;
    return g_legacy;
}

Page *page_legacy(void)
{
    return g_legacy && g_legacy->dom ? g_legacy : NULL;
}

int page_legacy_connect_dom(int port)
{
    char wsurl[512];
    Page *pg = legacy_get();
    if (!pg || cdp_first_page_wsurl("127.0.0.1", port, wsurl, sizeof(wsurl)) != 0)
        return -1;
    if (pg->dom)
        cdp_close(pg->dom);
    pg->dom = cdp_connect_wsurl(wsurl);
    return pg->dom ? 0 : -1;
}

int page_legacy_connect_evt(int port)
{
    char wsurl[512];
    Page *pg = legacy_get();
    if (!pg || cdp_first_page_wsurl("127.0.0.1", port, wsurl, sizeof(wsurl)) != 0)
        return -1;
    page_stop(pg);
    if (pg->evt)
        cdp_close(pg->evt);
    pg->evt = cdp_connect_wsurl(wsurl);
    if (!pg->evt)
        return -1;
    add_binding(pg->evt);
    add_ie_compat(pg->evt);
    return page_start(pg);
}

void page_legacy_disconnect(void)
{
    Page *pg = g_legacy;
    if (!pg)
        return;
    g_legacy = NULL;
    page_stop(pg);
    pg->doc = NULL;
    page_release(pg);
}

/* rcache.c -- read cache for the vendor's element-list scans.
 *
 * TEBEOS finds page elements by walking lists and comparing ids, often a full scan per table
 * row: `from HtmlElement he in col where he.Id == "form1:tableEx1:" + l + ":text62"`. In IE
 * every step is an in-process call; here each is a CDP round trip (~1 ms of Chromium CPU on
 * the pharmacy's laptop). Measured with the PROF lines (page.c): opening a prescription made
 * 31,265 calls in 28.4 s -- 14,110 element.id, 6,046 list.length, 5,656 list[i],
 * 2,680 getAttribute('id'), ~1,600 className/tagName.
 *
 * So when a list element is asked for, the next RC_BATCH elements come back in the same
 * request, each with its id, tagName and className, plus the list's length; the following
 * reads of those are answered here. An id/tagName/className read that misses fetches those
 * labels for the element and the RC_BATCH-1 stashed after it (rc_elem_fetch). Only these labels are cached -- values, text, HTML,
 * checked states are always read live -- and only for HTML elements.
 *
 * Everything is forgotten (rc_bump) on any call that is not a known pure read (page_call),
 * any page event or vendor event dispatch, any user input, and after RC_TTL_MS. While the
 * vendor's code runs IE runs no page script, so the page cannot change under it; holding
 * these labels for the length of one of its loops is, if anything, closer to IE than reading
 * each one live while Chromium's own scripts run between the requests. */

#include "rcache.h"
#include "dom.h"
#include "log.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RC_BATCH 256
#define RC_TTL_MS 1000
#define RC_ELEMS 4096       /* power of two */
#define RC_COLS 32

typedef struct {
    Page *pg;
    long h;
    LONG gen;
    DWORD t;
    char *v[3];             /* id, tagName, className (NULL = null) */
} RcElem;

typedef struct {
    Page *pg;
    long h;
    LONG gen;
    DWORD t;
    long length;
    long start, count;
    long items[RC_BATCH];
} RcCol;

static RcElem g_el[RC_ELEMS];
static RcCol g_col[RC_COLS];
static int g_col_next;
static volatile LONG g_gen = 1;
static int g_pure;          /* UI thread only, like every dom call */

void rc_bump(void)
{
    InterlockedIncrement(&g_gen);
}

void rc_pure_enter(void)
{
    g_pure++;
}

void rc_pure_leave(void)
{
    if (g_pure > 0)
        g_pure--;
}

int rc_pure(void)
{
    return g_pure > 0;
}

static int fresh(LONG gen, DWORD t)
{
    return gen == g_gen && GetTickCount() - t < RC_TTL_MS;
}

static int prop_index(const char *prop)
{
    return !strcmp(prop, "id") ? 0 : !strcmp(prop, "tagName") ? 1 : !strcmp(prop, "className") ? 2 : -1;
}

static void elem_store(Page *pg, long h, LONG gen, DWORD t, const JVal *id, const JVal *tag, const JVal *cls);

static RcElem *elem_slot(Page *pg, long h)
{
    unsigned long k = ((unsigned long)h * 2654435761UL) ^ (unsigned long)(ULONG_PTR)pg;
    return &g_el[k & (RC_ELEMS - 1)];
}

int rc_elem_prop(Page *pg, long h, const char *prop, char **out)
{
    int i = prop_index(prop);
    RcElem *e = elem_slot(pg, h);

    *out = NULL;
    if (i < 0 || e->pg != pg || e->h != h || !fresh(e->gen, e->t))
        return 0;
    if (e->v[i] && !(*out = dup_utf8(e->v[i])))
        return 0;
    return 1;
}

/* An element's labels were asked for and are not cached: fetch them for it and the elements
 * stashed right after it in one request. Elements are stashed in the order the vendor met
 * them, so these are usually the rest of the same list (one materialised earlier with
 * ToList(), whose ids TEBEOS then compares a dozen times each). 1 if h's labels came back. */
int rc_elem_fetch(Page *pg, long h)
{
    char *x = sfmt("(function(h0,n){var E=window.__shim_els,r=[];if(!E)return r;"
                   "var f=function(v){return v===null||v===undefined?null:String(v);};"
                   "for(var h=h0;h<h0+n;h++){var e=E[h];"
                   "if(e===undefined||!(e instanceof HTMLElement))continue;"
                   "r.push(h,f(e.id),f(e.tagName),f(e.className));}"
                   "return r;})(%ld,%d)", h, RC_BATCH);
    JVal *reply = NULL, *ro, *a;
    LONG gen;
    DWORD t;
    size_t k;
    int got = 0;

    if (!x)
        return 0;
    gen = g_gen;
    rc_pure_enter();
    ro = dom_eval(pg, x, &reply);
    rc_pure_leave();
    free(x);
    a = j_get(ro, "value");
    if (a && a->t == J_ARR && a->n % 4 == 0 && gen == g_gen) {
        t = GetTickCount();
        for (k = 0; k < a->n; k += 4) {
            long eh = (long)j_num(&a->items[k]);
            elem_store(pg, eh, gen, t, &a->items[k + 1], &a->items[k + 2], &a->items[k + 3]);
            got |= eh == h;
        }
    }
    j_free(reply);
    return got;
}

static RcCol *col_find(Page *pg, long col)
{
    int i;
    for (i = 0; i < RC_COLS; i++)
        if (g_col[i].pg == pg && g_col[i].h == col && fresh(g_col[i].gen, g_col[i].t))
            return &g_col[i];
    return NULL;
}

int rc_col_length(Page *pg, long col, long *len)
{
    RcCol *c = col_find(pg, col);
    if (!c)
        return 0;
    *len = c->length;
    return 1;
}

static void elem_store(Page *pg, long h, LONG gen, DWORD t, const JVal *id, const JVal *tag, const JVal *cls)
{
    RcElem *e = elem_slot(pg, h);
    const JVal *src[3] = { id, tag, cls };
    int i;

    for (i = 0; i < 3; i++) {
        free(e->v[i]);
        e->v[i] = src[i] && src[i]->t == J_STR ? dup_utf8(src[i]->s) : NULL;
    }
    e->pg = pg;
    e->h = h;
    e->gen = gen;
    e->t = t;
}

/* [length, h, html, id, tagName, className, h, ...] for col[idx .. idx+RC_BATCH) */
static RcCol *col_fetch(Page *pg, long col, long idx)
{
    char *x = sfmt("(%s,(function(c,i,B){var S=window.__shim_stash,n=c.length>>>0,"
                   "z=Math.min(n,i+%d),r=[n];"
                   "var f=function(v){return v===null||v===undefined?null:String(v);};"
                   "for(var j=i;j<z;j++){var e=c[j];var h=S(e,B);"
                   "if(h<0||!(e instanceof HTMLElement)){r.push(h,0,null,null,null);continue;}"
                   "r.push(h,1,f(e.id),f(e.tagName),f(e.className));}"
                   "return r;})(window.__shim_els[%ld],%ld,%ld))",
                   STASH_DEF, RC_BATCH, col, idx, page_stash_base(pg));
    JVal *reply = NULL, *ro, *a;
    RcCol *c = NULL;
    LONG gen;
    DWORD t;
    size_t k, n;

    if (!x)
        return NULL;
    rc_pure_enter();
    ro = dom_eval(pg, x, &reply);
    rc_pure_leave();
    free(x);
    a = j_get(ro, "value");
    if (!a || a->t != J_ARR || a->n < 1 || (a->n - 1) % 5) {
        j_free(reply);
        return NULL;
    }
    gen = g_gen;
    t = GetTickCount();
    for (k = 0; k < RC_COLS && !c; k++)     /* a list keeps one window: the latest */
        if (g_col[k].pg == pg && g_col[k].h == col)
            c = &g_col[k];
    if (!c) {
        c = &g_col[g_col_next];
        g_col_next = (g_col_next + 1) % RC_COLS;
    }
    c->pg = pg;
    c->h = col;
    c->gen = gen;
    c->t = t;
    c->length = (long)j_num(&a->items[0]);
    c->start = idx;
    n = (a->n - 1) / 5;
    c->count = (long)n;
    for (k = 0; k < n; k++) {
        JVal *r = &a->items[1 + k * 5];
        long h = (long)j_num(&r[0]);
        c->items[k] = h;
        if (h >= 0)
            page_stash_used(pg, h);
        if (h >= 0 && j_num(&r[1]) != 0)
            elem_store(pg, h, gen, t, &r[2], &r[3], &r[4]);
    }
    j_free(reply);
    return c;
}

int rc_col_item(Page *pg, long col, long idx, long *h)
{
    RcCol *c = col_find(pg, col);

    *h = -1;
    if (c && (idx < 0 || idx >= c->length))
        return 1;
    if (!c || idx < c->start || idx >= c->start + c->count) {
        if (idx < 0)
            return 0;
        if (!(c = col_fetch(pg, col, idx)))
            return 0;
        if (idx >= c->length)
            return 1;
    }
    if (idx < c->start || idx >= c->start + c->count)
        return 0;
    *h = c->items[idx - c->start];
    return 1;
}

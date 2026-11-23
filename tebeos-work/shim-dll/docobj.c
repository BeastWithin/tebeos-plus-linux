/* docobj.c -- DocObject document-side implementation for the Path-A mshtml shim.
 *
 * Milestone 3b: the OLE DocObject hosting surface that Wine's ieframe/urlmon drive when
 * embedding the document as an Active Document. Mirrors Wine's own mshtml (oleobj.c/view.c)
 * but reduced to the minimum a well-behaved DocObject container needs:
 *
 *   IOleObject::{SetClientSite,GetClientSite,GetUserClassID,DoVerb,Close}
 *   IOleDocument::CreateView
 *   IOleDocumentView::{SetInPlaceSite,GetInPlaceSite,GetDocument,SetRect,GetRect,Show,UIActivate,CloseView}
 *   IOleInPlaceObject::*                 (window + deactivation)
 *   IOleInPlaceActiveObject::*           (frame integration)
 *
 * The render target is a placeholder child window (class "ShimHtmlServer") painted with the
 * system window color; it will be swapped for a CEF view in a later milestone. The window is
 * what makes the document a real in-place object that ieframe can size and show.
 *
 * All method signatures match the Wine headers exactly (the generator's REAL_IMPLS wires the
 * vtables to these symbols; see stubs.h).
 */

#include "html_doc.h"
#include <exdisp.h>
#include <mshtmhst.h>
#include "stubs.h"
#include "render.h"
#include "input.h"
#include "page.h"
#include "dom.h"
#include "rcache.h"
#include "log.h"
#include "util.h"
#include "cdp.h"
#include <commdlg.h>
#include <winspool.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ window plumbing */

/* Recover the HTMLDocument from a child render window (set at creation, GWLP_USERDATA). */
static HTMLDocument *doc_from_hwnd(HWND hwnd)
{
    return (HTMLDocument *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

#define EMBED_TIMER 0x7e3b   /* embed mode: report the window's place (page_embed_tick) */

/* Screencast the document's page into its window once both exist (the page's pump blits
 * every frame). Idempotent: called from activate/Show/SetRect and after Load. */
static void doc_ensure_render(HTMLDocument *d)
{
    Page *pg = doc_page(d);
    if (!d->hwnd || !pg)
        return;
    page_attach_window(pg, d->hwnd);
    if (page_embed_mode())
        SetTimer(d->hwnd, EMBED_TIMER, 500, NULL);   /* see page_embed_tick */
    InvalidateRect(d->hwnd, NULL, FALSE);
}

void html_doc_render_stop(HTMLDocument *d)
{
    Page *pg = doc_page(d);
    if (d->hwnd)
        KillTimer(d->hwnd, EMBED_TIMER);
    if (pg)
        page_attach_window(pg, NULL);
}

void html_doc_window_ready(HTMLDocument *d)
{
    doc_ensure_render(d);
}

/* ------------------------------------------------------------------ clipboard, context menu */
/* Headless Chromium has its own clipboard, cut off from the desktop, and no context menu. IE
 * did both against the Windows clipboard (which Wine shares with the Linux desktop), so the
 * shim does: Ctrl+C/X/V/A (and Ctrl/Shift+Insert) and the right-click menu read the selection
 * from the page and paste through Input.insertText, the same path as typed text. */

/* the selected text: in the focused field if it has a selection, else in the page (the caret
 * may stay in a field while text elsewhere is selected); null for a password field.
 * With cut, an editable field's selection is then deleted (the browser's delete command, as
 * for Backspace, so SGK's input masks keep step). */
static const char CLIP_SEL_JS[] =
    "(function(cut){var e=document.activeElement;"
    "if(e&&(e.tagName==='INPUT'||e.tagName==='TEXTAREA')){var s,f;"
    "try{s=e.selectionStart;f=e.selectionEnd;}catch(z){}"
    "if(typeof s==='number'&&f>s){if(e.type==='password')return null;var t=e.value.substring(s,f);"
    "if(cut&&!e.readOnly&&!e.disabled)document.execCommand('delete');return t;}}"
    "return window.getSelection?String(window.getSelection()):'';})(%d)";

static void clip_put(HWND hwnd, const char *utf8)
{
    WCHAR *w0 = utf8_to_wstr(utf8), *w, *a, *b;
    size_t n;
    HGLOBAL g;
    void *p;

    if (!w0)
        return;
    /* LF -> CRLF, as Windows programs expect */
    if (!(w = malloc((wcslen(w0) * 2 + 1) * sizeof(WCHAR)))) {
        free(w0);
        return;
    }
    for (a = w0, b = w; *a; a++) {
        if (*a == '\n' && (a == w0 || a[-1] != '\r'))
            *b++ = '\r';
        *b++ = *a;
    }
    *b = 0;
    free(w0);
    n = (wcslen(w) + 1) * sizeof(WCHAR);
    g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (g && (p = GlobalLock(g))) {
        memcpy(p, w, n);
        GlobalUnlock(g);
        if (OpenClipboard(hwnd)) {
            EmptyClipboard();
            if (SetClipboardData(CF_UNICODETEXT, g))
                g = NULL;
            CloseClipboard();
        }
    }
    if (g)
        GlobalFree(g);
    free(w);
}

/* the clipboard's text as UTF-8, CRLF made LF; NULL if none */
static char *clip_get(HWND hwnd)
{
    char *r = NULL, *a, *b;
    HANDLE h;
    WCHAR *p;

    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(hwnd))
        return NULL;
    if ((h = GetClipboardData(CF_UNICODETEXT)) && (p = GlobalLock(h))) {
        r = wstr_to_utf8(p);
        GlobalUnlock(h);
    }
    CloseClipboard();
    for (a = b = r; r && *a; a++)
        if (!(a[0] == '\r' && a[1] == '\n'))
            *b++ = *a;
    if (r)
        *b = '\0';
    return r;
}

static void doc_copy(HTMLDocument *d, HWND hwnd, int cut)
{
    Page *pg = doc_page(d);
    char *x, *t;

    if (!pg)
        return;
    page_evt_drain(pg, 300);
    x = sfmt(CLIP_SEL_JS, cut);
    t = x ? dom_eval_str(pg, x) : NULL;
    if (t && *t)
        clip_put(hwnd, t);
    shim_log("CLIP %s %lu chars", cut ? "cut" : "copy", (unsigned long)(t ? strlen(t) : 0));
    free(t);
    free(x);
}

static void doc_paste(HTMLDocument *d, HWND hwnd)
{
    Page *pg = doc_page(d);
    CdpConn *rc = page_evt(pg);
    char *t = clip_get(hwnd);

    if (pg && rc && t && *t) {
        page_evt_drain(pg, 300);
        input_insert_text(rc, t);
    }
    shim_log("CLIP paste %lu chars", (unsigned long)(t ? strlen(t) : 0));
    free(t);
}

static void doc_select_all(HTMLDocument *d)
{
    Page *pg = doc_page(d);
    if (!pg)
        return;
    page_evt_drain(pg, 300);
    dom_exec(pg, "document.execCommand('selectAll')");
}

/* ------------------------------------------------------------------ printing
 *
 * Headless Chromium has no print dialog and ignores window.print(), so IE's print commands
 * (right-click "Yazdir...", OLECMDID_PRINT, window.print, Ctrl+P) are done here: Chromium lays
 * the page out for print as a PDF (Page.printToPDF: its own pagination, with the date, title,
 * address and page numbers in the margins as IE prints them), the Windows print dialog picks
 * the printer, and the PDF goes to it as a RAW job. Wine hands a RAW job to CUPS with the
 * format left to auto-detection, so CUPS recognises the PDF and renders it for the printer.
 * "Baski Onizleme" saves the PDF and opens it in the desktop's PDF viewer. */

/* Print the selection (IE's "Secim" in the print dialog, "As selected on screen" in its
 * preview): Chromium's PDF has no such option, so the selected part is copied into a box at the
 * end of <body> and a print-only style hides everything else. The copy is the selection's
 * common ancestor, cloned deep with the typed values of its fields, pruned to what the
 * selection covers (partly selected text is cut at the selection's ends), and wrapped in
 * shallow copies of its ancestors so the page's CSS (table borders, classes) still applies.
 * PRINT_SEL_UNDO removes both after Page.printToPDF. Returns 1 when there was a selection. */
#define PRINT_SEL_HAS \
    "(function(){var s=getSelection();return s&&s.rangeCount&&!s.isCollapsed&&String(s).trim()?1:0;})()"
#define PRINT_SEL_JS \
    "(function(){var s=getSelection();if(!s||!s.rangeCount||s.isCollapsed)return 0;" \
    "var r=s.getRangeAt(0),a=r.commonAncestorContainer;if(a.nodeType!==1)a=a.parentNode;" \
    "if(a===document.documentElement)a=document.body;var c=a.cloneNode(true),i,j;" \
    "var o=a.querySelectorAll('input,textarea,select'),k=c.querySelectorAll('input,textarea,select');" \
    "for(i=0;i<o.length&&i<k.length;i++){var e=o[i],f=k[i];" \
    "if(e.tagName==='SELECT'){for(j=0;j<e.options.length&&j<f.options.length;j++)" \
    "if(e.options[j].selected)f.options[j].setAttribute('selected','selected');else f.options[j].removeAttribute('selected');}" \
    "else if(e.type==='checkbox'||e.type==='radio'){if(e.checked)f.setAttribute('checked','checked');else f.removeAttribute('checked');}" \
    "else if(e.tagName==='TEXTAREA')f.textContent=e.value;else f.setAttribute('value',e.value);}" \
    "function prune(on,cn){var oc=on.childNodes,cc=cn.childNodes;for(var i=oc.length-1;i>=0;i--){var x=oc[i],y=cc[i];if(!y)continue;" \
    "if(!r.intersectsNode(x)){cn.removeChild(y);continue;}" \
    "if(x.nodeType===3){var b=0,en=x.data.length;if(x===r.startContainer)b=r.startOffset;if(x===r.endContainer)en=r.endOffset;y.data=x.data.substring(b,en);}" \
    "else if(x.nodeType===1)prune(x,y);}}" \
    "prune(a,c);c.querySelectorAll('script').forEach(function(x){x.remove();});" \
    "function flat(q){if(q.style){q.style.position='static';q.style.height='auto';q.style.overflow='visible';}}" \
    "var box=document.createElement('div');box.id='__shim_ps';" \
    "if(a===document.body){while(c.firstChild)box.appendChild(c.firstChild);}else{" \
    "var w=c,p=a.parentNode;flat(c);while(p&&p.nodeType===1&&p!==document.body){var q=p.cloneNode(false);" \
    "q.removeAttribute('id');flat(q);q.appendChild(w);w=q;p=p.parentNode;}box.appendChild(w);}" \
    "var st=document.createElement('style');st.id='__shim_pss';" \
    "st.textContent='@media print{body>*:not(#__shim_ps){display:none!important}#__shim_ps{display:block!important}}';" \
    "(document.head||document.documentElement).appendChild(st);document.body.appendChild(box);return 1;})()"
#define PRINT_SEL_UNDO \
    "(function(){var e=document.getElementById('__shim_ps');if(e)e.remove();" \
    "e=document.getElementById('__shim_pss');if(e)e.remove();})()"

static unsigned char *print_pdf(HTMLDocument *d, int landscape, double pw, double ph,
                                const char *ranges, int sel, size_t *n, char **err)
{
    Page *pg = doc_page(d);
    JVal *reply = NULL;
    const char *data;
    unsigned char *out = NULL;
    char *params, *head, *foot, *qh, *qf;
    SYSTEMTIME now;
    int k;

    *n = 0;
    *err = NULL;
    /* IE prints at 100% whatever the optical zoom (ReceteEventBtn sets 70 on small screens) */
    if (d->zoom && d->zoom != 100)
        dom_exec(pg, "document.documentElement&&(document.documentElement.style.zoom='')");
    if (sel) {
        char *ok = dom_eval_str(pg, PRINT_SEL_JS);
        if (!ok || strcmp(ok, "1") != 0)
            sel = 0;    /* the selection went away: the whole page, as IE */
        free(ok);
    }
    /* IE's margins: title and "page x of y" at the top, address and date below */
    GetLocalTime(&now);
    head = sfmt("<div style=\"font:8px sans-serif;width:100%%;padding:0 0.4in;box-sizing:border-box;display:flex;"
                "justify-content:space-between\"><span class=title></span>"
                "<span>Sayfa <span class=pageNumber></span> / <span class=totalPages></span></span></div>");
    foot = sfmt("<div style=\"font:8px sans-serif;width:100%%;padding:0 0.4in;box-sizing:border-box;display:flex;"
                "justify-content:space-between\"><span class=url style=\"overflow:hidden;"
                "white-space:nowrap;max-width:75%%\"></span><span>%02u.%02u.%04u %02u:%02u</span></div>",
                now.wDay, now.wMonth, now.wYear, now.wHour, now.wMinute);
    qh = head ? j_quote(head) : NULL;
    qf = foot ? j_quote(foot) : NULL;
    params = qh && qf ? sfmt("{\"landscape\":%s,\"displayHeaderFooter\":true,\"printBackground\":false,"
                  "\"headerTemplate\":%s,\"footerTemplate\":%s,"
                  "\"paperWidth\":%.2f,\"paperHeight\":%.2f,\"marginTop\":0.5,\"marginBottom\":0.5,"
                  "\"marginLeft\":0.4,\"marginRight\":0.4,\"pageRanges\":\"%s\"}",
                  landscape ? "true" : "false", qh, qf, pw, ph, ranges ? ranges : "") : NULL;
    free(head); free(foot); free(qh); free(qf);
    if (params && page_call(pg, "Page.printToPDF", params, &reply) == 0) {
        data = j_str(j_get(j_get(reply, "result"), "data"));
        if (data && (out = (unsigned char *)malloc(strlen(data) / 4 * 3 + 4))) {
            k = b64_decode(data, out, strlen(data) / 4 * 3 + 4);
            if (k > 0)
                *n = (size_t)k;
        } else if (j_str(j_get(j_get(reply, "error"), "message"))) {
            *err = _strdup(j_str(j_get(j_get(reply, "error"), "message")));
        }
    }
    if (sel)
        dom_exec(pg, PRINT_SEL_UNDO);
    if (d->zoom && d->zoom != 100) {
        char *x = sfmt("document.documentElement&&(document.documentElement.style.zoom='%ld%%')",
                       (long)d->zoom);
        if (x)
            dom_exec(pg, x);
        free(x);
    }
    free(params);
    j_free(reply);
    if (!*n) {
        free(out);
        out = NULL;
    }
    return out;
}

static int print_send(const WCHAR *printer, const WCHAR *title, const unsigned char *pdf, size_t n)
{
    HANDLE h = NULL;
    DOC_INFO_1W di;
    size_t off = 0;
    DWORD w;
    int ok = 0;

    if (!OpenPrinterW((WCHAR *)printer, &h, NULL))
        return 0;
    memset(&di, 0, sizeof(di));
    di.pDocName = (WCHAR *)(title && *title ? title : L"Medula");
    di.pDatatype = (WCHAR *)L"RAW";
    if (StartDocPrinterW(h, 1, (BYTE *)&di)) {
        ok = 1;
        while (off < n) {
            DWORD c = n - off > 65536 ? 65536 : (DWORD)(n - off);
            if (!WritePrinter(h, (void *)(pdf + off), c, &w) || !w) {
                ok = 0;
                break;
            }
            off += w;
        }
        EndDocPrinter(h);
    }
    ClosePrinter(h);
    return ok;
}

/* The page's paper from the printer's DEVMODE, in inches (A4 when it names none). */
static void print_paper(const DEVMODEW *dm, double *pw, double *ph, int *landscape)
{
    *pw = 8.27;
    *ph = 11.69;
    if (!dm)
        return;
    if ((dm->dmFields & DM_ORIENTATION) && dm->dmOrientation == DMORIENT_LANDSCAPE)
        *landscape = 1;
    if (dm->dmFields & DM_PAPERSIZE) {
        switch (dm->dmPaperSize) {
        case DMPAPER_LETTER: *pw = 8.5;  *ph = 11.0;  return;
        case DMPAPER_LEGAL:  *pw = 8.5;  *ph = 14.0;  return;
        case DMPAPER_A5:     *pw = 5.83; *ph = 8.27;  return;
        case DMPAPER_A3:     *pw = 11.69; *ph = 16.54; return;
        case DMPAPER_A4:     return;
        default: break;
        }
    }
    if ((dm->dmFields & (DM_PAPERWIDTH | DM_PAPERLENGTH)) == (DM_PAPERWIDTH | DM_PAPERLENGTH) &&
        dm->dmPaperWidth > 0 && dm->dmPaperLength > 0) {
        *pw = dm->dmPaperWidth / 254.0;     /* tenths of a millimetre */
        *ph = dm->dmPaperLength / 254.0;
    }
}

/* sel: 1 = the selection only (preselected in the dialog), 0 = the whole page; the dialog's
 * "Secim" choice is offered whenever the page has a selection, as IE does. */
static void doc_print(HTMLDocument *d, int prompt, int preview, int sel)
{
    Page *pg = doc_page(d);
    HWND owner = d->hwnd ? GetAncestor(d->hwnd, GA_ROOT) : NULL;
    WCHAR printer[256] = L"", *title = NULL;
    double pw = 8.27, ph = 11.69;
    int landscape = 0, copies = 1, i, sent = 0, has_sel;
    char ranges[48] = "", *t, *err = NULL;
    unsigned char *pdf;
    size_t n;

    if (!pg)
        return;
    page_evt_drain(pg, 300);
    t = dom_eval_str(pg, PRINT_SEL_HAS);
    has_sel = t && !strcmp(t, "1");
    free(t);
    if (!has_sel)
        sel = 0;
    if (preview) {
        /* the viewer's own print dialog picks printer and paper */
    } else if (prompt) {
        PRINTDLGW pd;
        memset(&pd, 0, sizeof(pd));
        pd.lStructSize = sizeof(pd);
        pd.hwndOwner = owner;
        pd.Flags = PD_USEDEVMODECOPIESANDCOLLATE | (has_sel ? (sel ? PD_SELECTION : 0) : PD_NOSELECTION);
        pd.nFromPage = pd.nToPage = pd.nMinPage = 1;
        pd.nMaxPage = 9999;
        pd.nCopies = 1;
        if (!PrintDlgW(&pd)) {
            shim_log("PRINT dialog closed (err 0x%lx)", (unsigned long)CommDlgExtendedError());
            return;
        }
        if (pd.hDevNames) {
            DEVNAMES *dn = (DEVNAMES *)GlobalLock(pd.hDevNames);
            if (dn) {
                lstrcpynW(printer, (WCHAR *)dn + dn->wDeviceOffset, 256);
                GlobalUnlock(pd.hDevNames);
            }
            GlobalFree(pd.hDevNames);
        }
        copies = pd.nCopies;
        if (pd.hDevMode) {
            DEVMODEW *dm = (DEVMODEW *)GlobalLock(pd.hDevMode);
            if (dm) {
                print_paper(dm, &pw, &ph, &landscape);
                if ((dm->dmFields & DM_COPIES) && dm->dmCopies > copies)
                    copies = dm->dmCopies;
                GlobalUnlock(pd.hDevMode);
            }
            GlobalFree(pd.hDevMode);
        }
        if (pd.Flags & PD_PAGENUMS)
            snprintf(ranges, sizeof(ranges), "%u-%u", pd.nFromPage, pd.nToPage);
        sel = (pd.Flags & PD_SELECTION) ? 1 : 0;
    } else {
        DWORD cch = 256;
        if (!GetDefaultPrinterW(printer, &cch))
            printer[0] = 0;
    }
    if (!preview && !printer[0]) {
        MessageBoxW(owner, L"Yaz\x0131c\x0131 bulunamad\x0131.", L"Yazd\x0131r", MB_OK | MB_ICONWARNING);
        return;
    }
    if (copies < 1)
        copies = 1;
    if (copies > 99)
        copies = 99;

    t = dom_eval_str(pg, "document.title");
    title = utf8_to_wstr(t && *t ? t : "Medula");
    free(t);
    pdf = print_pdf(d, landscape, pw, ph, ranges, sel, &n, &err);
    if (!pdf) {
        WCHAR *we = utf8_to_wstr(err ? err : "");
        WCHAR msg[512];
        _snwprintf(msg, 511, L"Sayfa yazd\x0131rmaya haz\x0131rlanamad\x0131.\n%ls", we ? we : L"");
        msg[511] = 0;
        MessageBoxW(owner, msg, L"Yazd\x0131r", MB_OK | MB_ICONWARNING);
        shim_log("PRINT no PDF: %s", err ? err : "?");
        free(we);
        free(err);
        free(title);
        return;
    }
    if (preview) {
        WCHAR dir[MAX_PATH], path[MAX_PATH], url[MAX_PATH + 16];
        HANDLE f;
        DWORD w = 0;
        GetTempPathW(MAX_PATH, dir);
        _snwprintf(path, MAX_PATH - 1, L"%lsmedula-onizleme-%lu.pdf", dir, (unsigned long)GetTickCount());
        path[MAX_PATH - 1] = 0;
        f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            WriteFile(f, pdf, (DWORD)n, &w, NULL);
            CloseHandle(f);
        }
        /* winebrowser hands a file: URL to the desktop (xdg-open) */
        _snwprintf(url, MAX_PATH + 15, L"file:///%ls", path);
        url[MAX_PATH + 15] = 0;
        for (i = 0; url[i]; i++)
            if (url[i] == L'\\')
                url[i] = L'/';
        sent = w == n && (INT_PTR)ShellExecuteW(owner, NULL, L"winebrowser.exe", url, NULL, SW_SHOWNORMAL) > 32;
        shim_log("PRINT preview%s %lu bytes %s", sel ? " (selection)" : "", (unsigned long)n, sent ? "opened" : "FAILED");
    } else {
        for (i = 0; i < copies; i++)
            if (print_send(printer, title, pdf, n))
                sent++;
        shim_log("PRINT%s %lu bytes to %ls, %d of %d cop%s, %s%s%s", sel ? " (selection)" : "", (unsigned long)n, printer, sent,
                 copies, copies == 1 ? "y" : "ies", landscape ? "landscape" : "portrait",
                 ranges[0] ? " pages " : "", ranges);
    }
    if (!sent)
        MessageBoxW(owner, preview ? L"\x00D6nizleme a\x00E7\x0131lamad\x0131." : L"Yaz\x0131c\x0131ya g\x00F6nderilemedi.",
                    L"Yazd\x0131r", MB_OK | MB_ICONWARNING);
    free(pdf);
    free(title);
}

/* window.print() arrives on the page's event binding, inside the pump's dispatch: show the
 * dialog from the window's own message instead. */
void html_doc_print_later(HTMLDocument *d)
{
    if (d && d->hwnd)
        PostMessageW(d->hwnd, WM_DOC_PRINT, 0, 0);
}

/* After a right click, as IE: unless the page cancelled its contextmenu event, the host may
 * show its own menu (IDocHostUIHandler::ShowContextMenu; WinForms does when
 * IsWebBrowserContextMenuEnabled is off), else IE's edit menu. */
static void doc_context_menu(HTMLDocument *d, HWND hwnd, int x, int y)
{
    enum { M_CUT = 1, M_COPY, M_PASTE, M_ALL, M_PRINT, M_PREVIEW, M_PRINT_SEL, M_PREVIEW_SEL };
    Page *pg = doc_page(d);
    IDocHostUIHandler *ui = NULL;
    POINT pt = { x, y };
    int ctx = 0, edit = 0, sel = 0, pw = 0, cmd;
    char *st;
    HMENU m;

    if (!pg)
        return;
    page_evt_drain(pg, 300);
    st = dom_eval_str(pg,
        "(function(){var c=window.__shim_ctx|0,e=document.activeElement,ed=0,s=0,pw=0;"
        "if(e&&(e.tagName==='TEXTAREA'||(e.tagName==='INPUT'&&"
        "/^(text|password|search|tel|url|email|number|)$/i.test(e.getAttribute('type')||'')))){"
        "ed=(e.readOnly||e.disabled)?0:1;pw=e.type==='password'?1:0;"
        "try{s=e.selectionEnd>e.selectionStart?1:0;}catch(z){}}"
        "else if(window.getSelection&&String(window.getSelection()).length)s=1;"
        "return c+','+ed+','+s+','+pw;})()");
    if (st)
        sscanf(st, "%d,%d,%d,%d", &ctx, &edit, &sel, &pw);
    free(st);
    if (ctx == 2)
        return;     /* the page's oncontextmenu returned false */
    ClientToScreen(hwnd, &pt);
    if (d->client && SUCCEEDED(IOleClientSite_QueryInterface(d->client, &IID_IDocHostUIHandler, (void **)&ui)) && ui) {
        HRESULT hr = ui->lpVtbl->ShowContextMenu(ui, edit ? CONTEXT_MENU_CONTROL : sel ? CONTEXT_MENU_TEXTSELECT : CONTEXT_MENU_DEFAULT,
                                                 &pt, (IUnknown *)&d->IOleCommandTarget_iface, NULL);
        ui->lpVtbl->Release(ui);
        shim_log("CONTEXT host ShowContextMenu hr=0x%08lx", (unsigned long)hr);
        if (hr == S_OK)
            return;
    }
    if (page_embed_mode())
        return;     /* the visible Chromium already shows its own menu for the forwarded click */
    if (!(m = CreatePopupMenu()))
        return;
    AppendMenuW(m, MF_STRING | ((edit && sel && !pw) ? 0 : MF_GRAYED), M_CUT, L"Kes");
    AppendMenuW(m, MF_STRING | ((sel && !pw) ? 0 : MF_GRAYED), M_COPY, L"Kopyala");
    AppendMenuW(m, MF_STRING | ((edit && IsClipboardFormatAvailable(CF_UNICODETEXT)) ? 0 : MF_GRAYED), M_PASTE,
                L"Yap\x0131\x015Ft\x0131r");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, M_ALL, L"T\x00FCm\x00FCn\x00FC Se\x00E7");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, M_PRINT, L"Yazd\x0131r...");
    AppendMenuW(m, MF_STRING, M_PREVIEW, L"Bask\x0131 \x00D6nizleme...");
    if (sel && !edit) {     /* a selection on the page (not inside a field) */
        AppendMenuW(m, MF_STRING, M_PRINT_SEL, L"Se\x00E7ili Alan\x0131 Yazd\x0131r...");
        AppendMenuW(m, MF_STRING, M_PREVIEW_SEL, L"Se\x00E7ili Alan\x0131 \x00D6nizle...");
    }
    cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
    switch (cmd) {
    case M_CUT:   doc_copy(d, hwnd, 1); break;
    case M_COPY:  doc_copy(d, hwnd, 0); break;
    case M_PASTE: doc_paste(d, hwnd); break;
    case M_ALL:   doc_select_all(d); break;
    case M_PRINT:   doc_print(d, 1, 0, 0); break;
    case M_PREVIEW: doc_print(d, 1, 1, 0); break;
    case M_PRINT_SEL:   doc_print(d, 1, 0, 1); break;
    case M_PREVIEW_SEL: doc_print(d, 1, 1, 1); break;
    default: break;
    }
}

static LRESULT CALLBACK doc_window_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    HTMLDocument *d = doc_from_hwnd(hwnd);
    CdpConn *rc = d ? page_evt(doc_page(d)) : NULL;

    if ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST))
        rc_bump();  /* the user may change the page (rcache.c) */
    switch (msg) {
    case WM_DOC_PRINT:      /* window.print() from the page (html_doc_print_later) */
        if (d)
            doc_print(d, 1, 0, 0);
        return 0;

    case WM_TIMER:
        if (w == EMBED_TIMER) {
            if (d)
                page_embed_tick(doc_page(d));
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        return 1; /* the back-buffer paints the whole client; avoid flicker */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (d && page_render(doc_page(d)))
            render_repaint(page_render(doc_page(d)), hwnd);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_MOUSEMOVE: {
        /* Chromium needs the held-buttons mask on every event: a move with the left button
         * down drags (scrollbar thumb, text selection), and a press on the scrollbar track
         * auto-repeats until it sees the release. The window holds the capture while a button
         * is down so the release (and a drag past the edge) still arrives here. */
        static DWORD last_time;
        static int last_x, last_y, clicks;
        int x = (int)(short)LOWORD(l), y = (int)(short)HIWORD(l);
        int buttons = ((w & MK_LBUTTON) ? 1 : 0) | ((w & MK_RBUTTON) ? 2 : 0) | ((w & MK_MBUTTON) ? 4 : 0);
        int mods = ((GetKeyState(VK_MENU) < 0) ? 1 : 0) | ((w & MK_CONTROL) ? 2 : 0) | ((w & MK_SHIFT) ? 8 : 0);
        const char *btn = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? "left" :
                          (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? "right" :
                          (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) ? "middle" :
                          (buttons & 1) ? "left" : (buttons & 2) ? "right" : (buttons & 4) ? "middle" : "none";
        if (msg == WM_MOUSEMOVE) {
            if (rc) input_mouse_ex(rc, "mouseMoved", x, y, btn, 0, buttons, mods);
            return 0;
        }
        if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN) {
            /* clickCount 2, 3 for a double/triple click (dblclick, word/line selection) */
            DWORD now = (DWORD)GetMessageTime();
            if (now - last_time <= GetDoubleClickTime() &&
                abs(x - last_x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                abs(y - last_y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2)
                clicks = clicks < 3 ? clicks + 1 : 1;
            else
                clicks = 1;
            last_time = now; last_x = x; last_y = y;
            SetCapture(hwnd);
            if (msg == WM_RBUTTONDOWN && d && doc_page(d))
                dom_exec(doc_page(d), "window.__shim_ctx=0");   /* see doc_context_menu */
            if (rc) input_mouse_ex(rc, "mousePressed", x, y, btn, clicks, buttons, mods);
            if (msg == WM_LBUTTONDOWN)
                SetFocus(hwnd);
            return 0;
        }
        if (rc) input_mouse_ex(rc, "mouseReleased", x, y, btn, clicks ? clicks : 1, buttons, mods);
        if (!buttons && GetCapture() == hwnd)
            ReleaseCapture();
        if (msg == WM_RBUTTONUP && d)
            doc_context_menu(d, hwnd, x, y);
        return 0;
    }

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        /* one notch (120) = 100 CSS px, as Chromium scrolls on Windows; the point is in
         * screen coordinates */
        POINT pt;
        int delta = (int)(short)HIWORD(w);
        int mods = ((GetKeyState(VK_MENU) < 0) ? 1 : 0) | ((LOWORD(w) & MK_CONTROL) ? 2 : 0) |
                   ((LOWORD(w) & MK_SHIFT) ? 8 : 0);
        pt.x = (int)(short)LOWORD(l);
        pt.y = (int)(short)HIWORD(l);
        ScreenToClient(hwnd, &pt);
        if (rc) {
            if (msg == WM_MOUSEWHEEL)
                input_wheel(rc, pt.x, pt.y, 0, -delta * 100 / 120, mods);
            else
                input_wheel(rc, pt.x, pt.y, delta * 100 / 120, 0, mods);
        }
        return 0;
    }

    case WM_CHAR: {
        /* A typed character is inserted as text, with no keydown/keypress. SGK's hxclient input
         * assists (the T.C. field's number mask) take keys over -- they cancel them and edit the
         * value in their own prompt format -- but TEBEOS writes that field from .NET (it sets
         * "" over hxclient's "__________0" while the field has the focus), which IE tolerated
         * and Chromium's hxclient path does not: fed real keystrokes, the field took nothing.
         * Inserted as text, the field edits like a plain input, as TEBEOS expects (it strips
         * any "_" itself). TranslateMessage's control characters (Backspace's \b, Tab, Enter,
         * Escape, Ctrl+letters) are not text; those keys go through WM_KEYDOWN. */
        WCHAR ch = (WCHAR)w;
        char utf8[8];
        int n;
        if (ch < 0x20 || ch == 0x7f)
            return 0;
        n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, utf8, sizeof(utf8) - 1, NULL, NULL);
        if (n > 0) {
            utf8[n] = '\0';
            if (rc) input_insert_text(rc, utf8);
        }
        return 0;
    }

    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP: {
        /* Named (non-character) keys go to the page as real keydown/keyup, so the vendor's
         * attachEvent("onkeydown"/"onkeyup") handlers see IE's keyCode -- e.g. F3 (114) on
         * E-Recete Sorgula opens Hasta Arama, F2/F4 drive their buttons. Characters go in
         * through WM_CHAR. Not forwarded: F1 (Chromium opens a help tab) and F5 (Chromium
         * reloads by itself, outside ieframe's navigation). Alt+key stays with the frame. */
        static const char *const fkeys[] = { "F1", "F2", "F3", "F4", "F5", "F6",
                                             "F7", "F8", "F9", "F10", "F11", "F12" };
        const char *key = NULL;
        int up = (msg == WM_KEYUP || msg == WM_SYSKEYUP), mods = 0;
        switch (w) {
        case VK_RETURN:  key = "Enter"; break;
        case VK_BACK:    key = "Backspace"; break;
        case VK_TAB:     key = "Tab"; break;
        case VK_DELETE:  key = "Delete"; break;
        case VK_LEFT:    key = "ArrowLeft"; break;
        case VK_RIGHT:   key = "ArrowRight"; break;
        case VK_UP:      key = "ArrowUp"; break;
        case VK_DOWN:    key = "ArrowDown"; break;
        case VK_HOME:    key = "Home"; break;
        case VK_END:     key = "End"; break;
        case VK_ESCAPE:  key = "Escape"; break;
        case VK_PRIOR:   key = "PageUp"; break;
        case VK_NEXT:    key = "PageDown"; break;
        case VK_INSERT:  key = "Insert"; break;
        default:
            if (w >= VK_F2 && w <= VK_F12 && w != VK_F5)
                key = fkeys[w - VK_F1];
            break;
        }
        /* Alt combos (WM_SYSKEY*) belong to the frame's menus; only F10 arrives there alone */
        if ((msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && w != VK_F10)
            key = NULL;
        if (GetKeyState(VK_MENU) < 0)    mods |= 1;
        if (GetKeyState(VK_CONTROL) < 0) mods |= 2;
        if (GetKeyState(VK_SHIFT) < 0)   mods |= 8;
        /* clipboard keys, against the Windows clipboard (see doc_copy); not with Alt (AltGr) */
        if (msg == WM_KEYDOWN && d && !(mods & 1)) {
            int ctrl = (mods & 2) != 0, shift = (mods & 8) != 0;
            if (ctrl && !shift && (w == 'C' || w == VK_INSERT)) { doc_copy(d, hwnd, 0); return 0; }
            if (ctrl && !shift && w == 'X')                     { doc_copy(d, hwnd, 1); return 0; }
            if ((ctrl && !shift && w == 'V') || (shift && !ctrl && w == VK_INSERT)) { doc_paste(d, hwnd); return 0; }
            if (ctrl && !shift && w == 'A')                     { doc_select_all(d); return 0; }
            if (ctrl && !shift && w == 'P')                     { doc_print(d, 1, 0, 0); return 0; }
        }
        if (key && (w == VK_BACK || w == VK_DELETE) && !up) {
            /* Backspace/Delete edit through the browser's own delete command, not a keydown:
             * hxclient cancels those keydowns and redoes the edit on its own copy of the value,
             * which TEBEOS's writes leave out of step (see WM_CHAR), so in the T.C. field every
             * deletion was undone. The keyup still goes out (TEBEOS's patient search runs on it). */
            Page *pg = d ? doc_page(d) : NULL;
            page_evt_drain(pg, 300);
            if (pg)
                dom_exec(pg, w == VK_BACK ? "document.execCommand('delete')"
                                          : "document.execCommand('forwardDelete')");
            return 0;
        }
        if (key && (w == VK_LEFT || w == VK_RIGHT) && !up) {
            /* Left/Right in a text field move the caret through the selection API, not a
             * keydown: hxclient cancels those keydowns too (its own caret bookkeeping, out of
             * step for the same reason), so in the T.C. field the caret never moved. Shift
             * extends the selection, Ctrl moves by word, as the keys do natively. Anywhere
             * else (no text field focused) the keydown goes out as usual. */
            Page *pg = d ? doc_page(d) : NULL;
            char js[1024];
            int moved = 0;
            /* setSelectionRange, not Selection.modify: modify follows the document selection,
             * which is not the field's caret when the page lacks focus. Ctrl steps over a word
             * and the spaces after it (Windows), Shift moves the selection's focus end. */
            snprintf(js, sizeof(js),
                     "(function(b,x,w){var e=document.activeElement;"
                     "if(!e||!(e.tagName==='TEXTAREA'||e.tagName==='INPUT'))return false;"
                     "var s,t;try{s=e.selectionStart;t=e.selectionEnd;}catch(z){return false;}"
                     "if(typeof s!=='number')return false;"
                     "var v=e.value,L=v.length,bk=e.selectionDirection==='backward',a=bk?t:s,f=bk?s:t;"
                     "function st(p){if(!w)return b?Math.max(0,p-1):Math.min(L,p+1);"
                     "var S=/\\s/;if(b){while(p>0&&S.test(v.charAt(p-1)))p--;while(p>0&&!S.test(v.charAt(p-1)))p--;}"
                     "else{while(p<L&&!S.test(v.charAt(p)))p++;while(p<L&&S.test(v.charAt(p)))p++;}return p;}"
                     "if(x){var n=st(f);e.setSelectionRange(Math.min(a,n),Math.max(a,n),n<a?'backward':'forward');return true;}"
                     "var p=(s!==t)?(b?s:t):f;if(s===t||w)p=st(p);e.setSelectionRange(p,p);return true;})(%d,%d,%d)",
                     w == VK_LEFT, (mods & 8) != 0, (mods & 2) != 0);
            page_evt_drain(pg, 300);
            if (pg && dom_eval_bool(pg, js, &moved) == 0 && moved)
                return 0;
        }
        if (key && w == VK_RETURN && !up) {
            /* Enter is a keydown plus a keypress (keyCode 13), as in IE: SGK's medula.js moves
             * the focus to the next field from onkeypress (and cancels it), TEBEOS's handlers
             * look for it too. Uncancelled, it submits the form as IE does. */
            if (rc) {
                input_key_mod(rc, "rawKeyDown", key, (int)w, mods);
                input_key(rc, "char", key, "\r", (int)w);
            }
            return 0;
        }
        if (key) {
            if (rc) input_key_mod(rc, up ? "keyUp" : "rawKeyDown", key, (int)w, mods);
            return 0;
        }
        break;
    }

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

static ATOM doc_window_class(void)
{
    static ATOM cls = 0;

    if (!cls) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = doc_window_proc;
        wc.hInstance     = shim_hinst;
        wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = NULL; /* the renderer paints everything; no default erase */
        wc.lpszClassName = L"ShimHtmlServer";
        cls = RegisterClassExW(&wc);
    }
    return cls;
}

/* In-place activate: ask the in-place site for the frame/window context and create (or
 * re-parent) our render window into it, then fire OnInPlaceActivate. Mirror of Wine's
 * mshtml/view.c activate_window(). */
static HRESULT doc_activate_window(HTMLDocument *d)
{
    IOleInPlaceSite *site = d->ipsite;
    IOleInPlaceFrame *frame = NULL;
    IOleInPlaceUIWindow *uiwindow = NULL;
    OLEINPLACEFRAMEINFO frameinfo;
    IOleInPlaceSiteEx *siteex = NULL;
    RECT posrect, cliprect;
    HWND parent = NULL;
    HRESULT hr;

    if (!site)
        return E_UNEXPECTED;

    hr = site->lpVtbl->CanInPlaceActivate(site);
    if (FAILED(hr))
        return hr;

    frameinfo.cb = sizeof(frameinfo);
    hr = site->lpVtbl->GetWindowContext(site, &frame, &uiwindow,
                                        &posrect, &cliprect, &frameinfo);
    if (FAILED(hr))
        return hr;

    hr = site->lpVtbl->GetWindow(site, &parent);
    if (FAILED(hr)) {
        if (frame) frame->lpVtbl->Release(frame);
        if (uiwindow) uiwindow->lpVtbl->Release(uiwindow);
        return hr;
    }

    if (!doc_window_class()) {
        if (frame) frame->lpVtbl->Release(frame);
        if (uiwindow) uiwindow->lpVtbl->Release(uiwindow);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    if (!d->hwnd) {
        d->hwnd = CreateWindowExW(0, L"ShimHtmlServer", NULL,
                WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                posrect.left, posrect.top,
                posrect.right - posrect.left, posrect.bottom - posrect.top,
                parent, NULL, shim_hinst, NULL);
        if (!d->hwnd) {
            if (frame) frame->lpVtbl->Release(frame);
            if (uiwindow) uiwindow->lpVtbl->Release(uiwindow);
            return HRESULT_FROM_WIN32(GetLastError());
        }
    } else {
        if (GetParent(d->hwnd) != parent)
            SetParent(d->hwnd, parent);
        SetWindowPos(d->hwnd, NULL, posrect.left, posrect.top,
                     posrect.right - posrect.left, posrect.bottom - posrect.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    /* Attach the document to its render window so the wndproc can reach render/input. */
    SetWindowLongPtrW(d->hwnd, GWLP_USERDATA, (LONG_PTR)d);

    d->view_rect = posrect;
    d->in_place_active = TRUE;

    hr = site->lpVtbl->QueryInterface(site, &IID_IOleInPlaceSiteEx, (void **)&siteex);
    if (SUCCEEDED(hr)) {
        BOOL redraw = FALSE;
        hr = siteex->lpVtbl->OnInPlaceActivateEx(siteex, &redraw, 0);
        siteex->lpVtbl->Release(siteex);
    } else {
        hr = site->lpVtbl->OnInPlaceActivate(site);
    }

    if (d->inplace_frame)
        d->inplace_frame->lpVtbl->Release(d->inplace_frame);
    d->inplace_frame = frame;
    d->inplace_ui = uiwindow;

    d->window_active = TRUE;
    doc_ensure_render(d);
    return hr;
}

/* ------------------------------------------------------------------ IOleObject */

/* Drop what SetClientSite took from the previous site. */
void html_doc_release_site(HTMLDocument *d)
{
    if (d->doc_object_service) {
        d->doc_object_service->lpVtbl->Release(d->doc_object_service);
        d->doc_object_service = NULL;
    }
    if (d->client_cmdtrg) {
        IOleCommandTarget_Release(d->client_cmdtrg);
        d->client_cmdtrg = NULL;
    }
    d->webbrowser = FALSE;
    if (d->client) {
        IOleClientSite_Release(d->client);
        d->client = NULL;
    }
}

/* The navigation seam, as Wine's mshtml (oleobj.c) sets it up with ieframe:
 *   - IDocObjectService from the site's shell browser: the document reports its own
 *     navigations through it (and ieframe then stops reporting them itself);
 *   - Exec(CGID_DocHostCmdPriv, DOCHOST_DOCCANNAVIGATE, window): ieframe sends
 *     WebBrowser.Navigate to this window's IHTMLPrivateWindow::SuperNavigate;
 *   - the IWebBrowserApp service marks the host as a WebBrowser control. */
static void doc_hook_site(HTMLDocument *d, IOleClientSite *site)
{
    IServiceProvider *sp = NULL;
    IUnknown *unk = NULL;
    HRESULT hr;

    if (SUCCEEDED(IOleClientSite_QueryInterface(site, &IID_IServiceProvider, (void **)&sp))) {
        hr = IServiceProvider_QueryService(sp, &SHIM_SID_SShellBrowser, &SHIM_IID_IBrowserService, (void **)&unk);
        if (SUCCEEDED(hr) && unk) {
            IUnknown_QueryInterface(unk, &IID_IDocObjectService, (void **)&d->doc_object_service);
            IUnknown_Release(unk);
            unk = NULL;
        }
        if (!d->doc_object_service)
            IServiceProvider_QueryService(sp, &SHIM_SID_SShellBrowser, &IID_IDocObjectService,
                                          (void **)&d->doc_object_service);
        if (SUCCEEDED(IServiceProvider_QueryService(sp, &SHIM_SID_SWebBrowserApp, &IID_IWebBrowser2, (void **)&unk)) && unk) {
            d->webbrowser = TRUE;
            IUnknown_Release(unk);
        }
        IServiceProvider_Release(sp);
    }
    if (SUCCEEDED(IOleClientSite_QueryInterface(site, &IID_IOleCommandTarget, (void **)&d->client_cmdtrg))) {
        VARIANT v;
        V_VT(&v) = VT_UNKNOWN;
        V_UNKNOWN(&v) = (IUnknown *)&d->window.IHTMLWindow2_iface;
        hr = IOleCommandTarget_Exec(d->client_cmdtrg, &SHIM_CGID_DocHostCmdPriv, SHIM_DOCHOST_DOCCANNAVIGATE,
                                    0, &v, NULL);
        shim_log("SITE DOCCANNAVIGATE hr=0x%08lx", (unsigned long)hr);
    }
    shim_log("SITE doc %p: IDocObjectService %s, WebBrowser %s", (void *)d,
             d->doc_object_service ? "yes" : "no", d->webbrowser ? "yes" : "no");
    if (d->doc_object_service)
        page_catch_up(doc_page(d));
}

HRESULT STDMETHODCALLTYPE html_doc_ole_SetClientSite(IOleObject *This, IOleClientSite *pClientSite)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IOleObject_iface);

    if (pClientSite == d->client)
        return S_OK;
    html_doc_release_site(d);
    if (!pClientSite)
        return S_OK;
    IOleClientSite_AddRef(pClientSite);
    d->client = pClientSite;
    doc_hook_site(d, pClientSite);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_ole_GetClientSite(IOleObject *This, IOleClientSite **ppClientSite)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IOleObject_iface);

    if (!ppClientSite)
        return E_INVALIDARG;
    if (d->client)
        d->client->lpVtbl->AddRef(d->client);
    *ppClientSite = d->client;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_ole_GetUserClassID(IOleObject *This, CLSID *pClsid)
{
    (void)This;
    if (!pClsid)
        return E_INVALIDARG;
    *pClsid = CLSID_HTMLDocument;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_ole_DoVerb(IOleObject *This, LONG iVerb, LPMSG lpmsg,
                                              IOleClientSite *pActiveSite, LONG lindex,
                                              HWND hwndParent, LPCRECT lprcPosRect)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IOleObject_iface);
    IOleDocumentSite *docsite = NULL;
    HRESULT hr;

    (void)lpmsg; (void)lindex; (void)hwndParent;

    if (iVerb != OLEIVERB_SHOW && iVerb != OLEIVERB_UIACTIVATE && iVerb != OLEIVERB_INPLACEACTIVATE)
        return E_NOTIMPL;

    if (!pActiveSite)
        pActiveSite = d->client;
    if (!pActiveSite)
        return E_FAIL;

    /* DocObject path: hand the view back to the container's IOleDocumentSite::ActivateMe,
     * which in turn calls IOleDocument::CreateView + SetRect + Show (see ieframe/client.c). */
    hr = pActiveSite->lpVtbl->QueryInterface(pActiveSite, &IID_IOleDocumentSite, (void **)&docsite);
    if (SUCCEEDED(hr)) {
        hr = docsite->lpVtbl->ActivateMe(docsite, &d->IOleDocumentView_iface);
        docsite->lpVtbl->Release(docsite);
    } else {
        /* Non-DocObject host: activate directly. */
        hr = html_doc_view_UIActivate(&d->IOleDocumentView_iface, TRUE);
        if (SUCCEEDED(hr)) {
            if (lprcPosRect)
                html_doc_view_SetRect(&d->IOleDocumentView_iface, (LPRECT)lprcPosRect);
            html_doc_view_Show(&d->IOleDocumentView_iface, TRUE);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE html_doc_ole_Close(IOleObject *This, DWORD dwSaveOption)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IOleObject_iface);

    (void)dwSaveOption;
    if (d->in_place_active)
        html_doc_ipo_InPlaceDeactivate(&d->IOleInPlaceObject_iface);
    html_doc_release_site(d);
    return S_OK;
}

/* ------------------------------------------------------------------ IOleDocument */

HRESULT STDMETHODCALLTYPE html_doc_doc_CreateView(IOleDocument *This, IOleInPlaceSite *pIPSite,
                                                  IStream *pstm, DWORD dwReserved,
                                                  IOleDocumentView **ppView)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IOleDocument_iface);
    HRESULT hr;

    if (!ppView)
        return E_INVALIDARG;

    /* The document is its own view (same object); just record the in-place site. */
    if (pIPSite) {
        hr = html_doc_view_SetInPlaceSite(&d->IOleDocumentView_iface, pIPSite);
        if (FAILED(hr))
            return hr;
    }
    (void)pstm; (void)dwReserved;

    d->IOleDocumentView_iface.lpVtbl->AddRef(&d->IOleDocumentView_iface);
    *ppView = &d->IOleDocumentView_iface;
    return S_OK;
}

/* ------------------------------------------------------------------ IOleDocumentView */

HRESULT STDMETHODCALLTYPE html_doc_view_SetInPlaceSite(IOleDocumentView *This, IOleInPlaceSite *pIPSite)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IOleDocumentView_iface);

    if (pIPSite)
        pIPSite->lpVtbl->AddRef(pIPSite);
    if (d->ipsite)
        d->ipsite->lpVtbl->Release(d->ipsite);
    d->ipsite = pIPSite;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_view_GetInPlaceSite(IOleDocumentView *This, IOleInPlaceSite **ppIPSite)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IOleDocumentView_iface);

    if (!ppIPSite)
        return E_INVALIDARG;
    if (d->ipsite)
        d->ipsite->lpVtbl->AddRef(d->ipsite);
    *ppIPSite = d->ipsite;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_view_GetDocument(IOleDocumentView *This, IUnknown **ppunk)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IOleDocumentView_iface);

    if (!ppunk)
        return E_INVALIDARG;
    *ppunk = (IUnknown *)&d->IHTMLDocument2_iface;
    (*ppunk)->lpVtbl->AddRef(*ppunk);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_view_SetRect(IOleDocumentView *This, LPRECT prcView)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IOleDocumentView_iface);

    if (!prcView)
        return E_INVALIDARG;
    if (shim_tracing())
        shim_log("VIEW SetRect %ld,%ld-%ld,%ld hwnd %p", prcView->left, prcView->top,
                 prcView->right, prcView->bottom, (void *)d->hwnd);
    d->view_rect = *prcView;
    if (d->hwnd) {
        MoveWindow(d->hwnd, prcView->left, prcView->top,
                   prcView->right - prcView->left, prcView->bottom - prcView->top, TRUE);
        page_resize(doc_page(d), prcView->right - prcView->left, prcView->bottom - prcView->top);
        doc_ensure_render(d);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_view_GetRect(IOleDocumentView *This, LPRECT prcView)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IOleDocumentView_iface);

    if (!prcView)
        return E_INVALIDARG;
    *prcView = d->view_rect;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_view_Show(IOleDocumentView *This, BOOL fShow)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IOleDocumentView_iface);

    if (shim_tracing())
        shim_log("VIEW Show(%d) hwnd %p", fShow, (void *)d->hwnd);
    if (fShow) {
        if (!d->ui_active) {
            HRESULT hr = doc_activate_window(d);
            if (FAILED(hr))
                return hr;
        }
        if (d->hwnd) {
            ShowWindow(d->hwnd, SW_SHOW);
            doc_ensure_render(d);
        }
    } else {
        if (d->hwnd)
            ShowWindow(d->hwnd, SW_HIDE);
        if (d->in_place_active)
            html_doc_ipo_InPlaceDeactivate(&d->IOleInPlaceObject_iface);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_view_UIActivate(IOleDocumentView *This, BOOL fUIActivate)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IOleDocumentView_iface);

    if (fUIActivate) {
        if (!d->window_active) {
            HRESULT hr = doc_activate_window(d);
            if (FAILED(hr))
                return hr;
        }
        d->ui_active = TRUE;
        if (d->ipsite) {
            HRESULT hr = d->ipsite->lpVtbl->OnUIActivate(d->ipsite);
            if (FAILED(hr))
                return hr;
        }
        /* SetActiveObject goes on the in-place frame's IOleInPlaceUIWindow part (ieframe
         * returns a NULL *ppDoc from GetWindowContext, so frame == active-object registrar). */
        if (d->inplace_frame)
            ((IOleInPlaceUIWindow *)d->inplace_frame)->lpVtbl->SetActiveObject(
                    (IOleInPlaceUIWindow *)d->inplace_frame,
                    (IOleInPlaceActiveObject *)&d->IOleInPlaceActiveObject_iface, NULL);
    } else {
        if (d->ipsite)
            d->ipsite->lpVtbl->OnUIDeactivate(d->ipsite, FALSE);
        if (d->inplace_frame)
            ((IOleInPlaceUIWindow *)d->inplace_frame)->lpVtbl->SetActiveObject(
                    (IOleInPlaceUIWindow *)d->inplace_frame, NULL, NULL);
        d->ui_active = FALSE;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_view_CloseView(IOleDocumentView *This, DWORD dwReserved)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IOleDocumentView_iface);
    (void)dwReserved;
    return html_doc_ole_Close(&d->IOleObject_iface, OLECLOSE_NOSAVE);
}

/* ------------------------------------------------------------------ IOleInPlaceObject */

HRESULT STDMETHODCALLTYPE html_doc_ipo_GetWindow(IOleInPlaceObject *This, HWND *phwnd)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IOleInPlaceObject_iface);

    if (!phwnd)
        return E_INVALIDARG;
    *phwnd = d->hwnd;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_ipo_ContextSensitiveHelp(IOleInPlaceObject *This, BOOL fEnterMode)
{
    (void)This; (void)fEnterMode;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_ipo_InPlaceDeactivate(IOleInPlaceObject *This)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IOleInPlaceObject_iface);

    if (shim_tracing())
        shim_log("VIEW InPlaceDeactivate hwnd %p", (void *)d->hwnd);
    if (d->in_place_active && d->ipsite)
        d->ipsite->lpVtbl->OnInPlaceDeactivate(d->ipsite);
    html_doc_render_stop(d);
    if (d->hwnd) {
        DestroyWindow(d->hwnd);
        d->hwnd = NULL;
    }
    d->in_place_active = FALSE;
    d->window_active = FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_ipo_UIDeactivate(IOleInPlaceObject *This)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IOleInPlaceObject_iface);

    if (d->ipsite)
        d->ipsite->lpVtbl->OnUIDeactivate(d->ipsite, FALSE);
    if (d->inplace_frame)
        ((IOleInPlaceUIWindow *)d->inplace_frame)->lpVtbl->SetActiveObject(
                (IOleInPlaceUIWindow *)d->inplace_frame, NULL, NULL);
    d->ui_active = FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_ipo_SetObjectRects(IOleInPlaceObject *This, LPCRECT lprcPosRect,
                                                      LPCRECT lprcClipRect)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IOleInPlaceObject_iface);

    (void)lprcClipRect;
    if (!d->hwnd)
        return S_OK;
    MoveWindow(d->hwnd, lprcPosRect->left, lprcPosRect->top,
               lprcPosRect->right - lprcPosRect->left, lprcPosRect->bottom - lprcPosRect->top,
               TRUE);
    page_resize(doc_page(d), lprcPosRect->right - lprcPosRect->left, lprcPosRect->bottom - lprcPosRect->top);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_ipo_ReactivateAndUndo(IOleInPlaceObject *This)
{
    (void)This;
    return S_OK;
}

/* ------------------------------------------------------------------ IOleInPlaceActiveObject */

HRESULT STDMETHODCALLTYPE html_doc_ipao_GetWindow(IOleInPlaceActiveObject *This, HWND *phwnd)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IOleInPlaceActiveObject_iface);

    if (!phwnd)
        return E_INVALIDARG;
    *phwnd = d->hwnd;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_ipao_ContextSensitiveHelp(IOleInPlaceActiveObject *This, BOOL fEnterMode)
{
    (void)This; (void)fEnterMode;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_ipao_TranslateAccelerator(IOleInPlaceActiveObject *This, LPMSG lpmsg)
{
    (void)This; (void)lpmsg;
    return S_FALSE;
}

HRESULT STDMETHODCALLTYPE html_doc_ipao_OnFrameWindowActivate(IOleInPlaceActiveObject *This, BOOL fActivate)
{
    (void)This; (void)fActivate;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_ipao_OnDocWindowActivate(IOleInPlaceActiveObject *This, BOOL fActivate)
{
    (void)This; (void)fActivate;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_ipao_ResizeBorder(IOleInPlaceActiveObject *This, LPCRECT prcBorder,
                                                     IOleInPlaceUIWindow *pUIWindow, BOOL fFrameWindow)
{
    (void)This; (void)prcBorder; (void)pUIWindow; (void)fFrameWindow;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_ipao_EnableModeless(IOleInPlaceActiveObject *This, BOOL fEnable)
{
    (void)This; (void)fEnable;
    return S_OK;
}

/* Exported diagnostic: report the render pump's latest frame dims + seq (0 if a frame has
 * arrived). Used by hostrender.c to assert the Milestone-6 integration end-to-end. */
int shim_doc_render_frame_info(IHTMLDocument2 *This, int *w, int *h, long *seq)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IHTMLDocument2_iface);
    struct RenderCtx *r = d ? page_render(doc_page(d)) : NULL;
    if (!r)
        return -1;
    return render_frame_info(r, w, h, seq);
}

/* ------------------------------------------------------------------ IOleCommandTarget */
/* What ieframe asks of the document: Refresh and Stop. Anything else is reported as not
 * supported (and logged once), as Wine's mshtml does for commands it lacks. */

HRESULT STDMETHODCALLTYPE html_doc_cmd_QueryStatus(IOleCommandTarget *This, const GUID *pguidCmdGroup,
                                                   ULONG cCmds, OLECMD prgCmds[], OLECMDTEXT *pCmdText)
{
    ULONG i;
    if (pguidCmdGroup)
        return OLECMDERR_E_UNKNOWNGROUP;
    for (i = 0; i < cCmds; i++)
        prgCmds[i].cmdf = (prgCmds[i].cmdID == OLECMDID_REFRESH || prgCmds[i].cmdID == OLECMDID_STOP ||
                           prgCmds[i].cmdID == OLECMDID_OPTICAL_ZOOM ||
                           prgCmds[i].cmdID == OLECMDID_OPTICAL_GETZOOMRANGE ||
                           prgCmds[i].cmdID == OLECMDID_PRINT || prgCmds[i].cmdID == OLECMDID_PRINTPREVIEW)
                          ? OLECMDF_SUPPORTED | OLECMDF_ENABLED : 0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE html_doc_cmd_Exec(IOleCommandTarget *This, const GUID *pguidCmdGroup, DWORD nCmdID,
                                            DWORD nCmdexecopt, VARIANT *pvaIn, VARIANT *pvaOut)
{
    HTMLDocument *d = CONTAINING_RECORD(This, HTMLDocument, IOleCommandTarget_iface);
    Page *pg = doc_page(d);

    if (!pguidCmdGroup && nCmdID == OLECMDID_REFRESH) {
        dom_exec(pg, "location.reload()");
        return S_OK;
    }
    if (!pguidCmdGroup && nCmdID == OLECMDID_STOP) {
        JVal *reply = NULL;
        if (pg && page_call(pg, "Page.stopLoading", "{}", &reply) == 0)
            j_free(reply);
        return S_OK;
    }
    /* WebBrowser.Print() asks without a dialog (default printer), ShowPrintDialog with one */
    if (!pguidCmdGroup && (nCmdID == OLECMDID_PRINT || nCmdID == OLECMDID_PRINTPREVIEW)) {
        doc_print(d, nCmdexecopt != OLECMDEXECOPT_DONTPROMPTUSER, nCmdID == OLECMDID_PRINTPREVIEW, 0);
        return S_OK;
    }
    /* IE's page zoom, in percent. TEBEOS's ReceteEventBtn sets it on every prescription page
     * when the screen is at most 1366 px wide (100, or 70 with the warning panel), and has no
     * try/catch: a refusal throws in .NET and ends its whole DocumentCompleted handler, so
     * everything after it on that page was skipped (e.g. the Karekod page's drug list).
     * CSS zoom on the root reflows the page, as IE's optical zoom does. */
    if (!pguidCmdGroup && nCmdID == OLECMDID_OPTICAL_ZOOM) {
        VARIANT v;
        VariantInit(&v);
        if (pvaIn && V_VT(pvaIn) != VT_EMPTY && V_VT(pvaIn) != VT_NULL) {
            if (FAILED(VariantChangeType(&v, pvaIn, 0, VT_I4)) || V_I4(&v) < 10 || V_I4(&v) > 1000)
                return E_INVALIDARG;
            if (pg) {
                char *x = V_I4(&v) == 100
                    ? sfmt("document.documentElement&&(document.documentElement.style.zoom='')")
                    : sfmt("document.documentElement&&(document.documentElement.style.zoom='%ld%%')",
                           (long)V_I4(&v));
                if (x)
                    dom_exec(pg, x);
                free(x);
            }
            d->zoom = V_I4(&v);
        }
        if (pvaOut) {
            VariantClear(pvaOut);
            V_VT(pvaOut) = VT_I4;
            V_I4(pvaOut) = d->zoom ? d->zoom : 100;
        }
        return S_OK;
    }
    if (!pguidCmdGroup && nCmdID == OLECMDID_OPTICAL_GETZOOMRANGE) {
        if (pvaOut) {
            VariantClear(pvaOut);
            V_VT(pvaOut) = VT_I4;
            V_I4(pvaOut) = MAKELONG(10, 1000);
        }
        return S_OK;
    }
    {
        WCHAR g[40] = L"(null)";
        if (pguidCmdGroup)
            StringFromGUID2(pguidCmdGroup, g, 40);
        shim_log("CMD Exec %ls %lu not supported", g, (unsigned long)nCmdID);
    }
    return pguidCmdGroup ? OLECMDERR_E_UNKNOWNGROUP : OLECMDERR_E_NOTSUPPORTED;
}

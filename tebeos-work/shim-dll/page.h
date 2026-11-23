/* page.h -- one Chromium tab behind one HTML document.
 *
 * The process shares one headless Chromium (TEBEOS_CDP_PORT, default 9333; launched on
 * first use if nothing answers) and one browser context, so its cookies are its own and its
 * tabs close when the process exits. Each document gets a tab and two CDP connections:
 *
 *   dom  synchronous calls from the UI thread (Runtime.evaluate, getResponseBody, ...);
 *   evt  read by the page's pump thread (screencast frames, Fetch pauses, navigation and
 *        DOM-event notifications); anyone may send fire-and-forget commands on it.
 *
 * Whatever needs the app (BeforeNavigate2, NavigateComplete2, DocumentComplete, attachEvent
 * sinks) is posted to a message-only window on the UI thread, so COM sinks are only ever
 * called on the thread that owns them.
 */

#ifndef SHIM_PAGE_H
#define SHIM_PAGE_H

#include <windows.h>
#include <objidl.h>
#include "cdp.h"
#include "json.h"

struct HTMLDocument;
struct RenderCtx;
struct HtmlEventObj;
typedef struct Page Page;

/* A new tab for `doc` (UI thread). NULL if Chromium cannot be reached or started. */
Page *page_create(struct HTMLDocument *doc);

/* The test harnesses' tab: the first page target on a port they started themselves. */
Page *page_legacy(void);
int page_legacy_connect_dom(int port);
int page_legacy_connect_evt(int port);
void page_legacy_disconnect(void);

void page_addref(Page *pg);
void page_release(Page *pg);

/* The document is going away: stop the pump, close the tab, forget the document. */
void page_close(Page *pg);

CdpConn *page_dom(Page *pg);
CdpConn *page_evt(Page *pg);

/* Wait (up to timeout_ms) until Chromium has answered every command sent on the event
 * connection -- its Input.* replies come once the renderer has handled them -- so that a
 * Runtime.evaluate on the DOM connection lands after the keystrokes sent before it. */
void page_evt_drain(Page *pg, DWORD timeout_ms);
struct HTMLDocument *page_doc(Page *pg);
void page_set_doc(Page *pg, struct HTMLDocument *doc);

/* Synchronous command on the dom connection. *reply must be j_free'd (may be NULL). */
int page_call(Page *pg, const char *method, const char *params, JVal **reply);

/* Navigate the tab. ieframe has already fired BeforeNavigate2 for it, so the request it
 * makes is not reported again; a POST body / extra headers are applied to that request. */
int page_navigate(Page *pg, const char *url, const BYTE *post, DWORD postlen, const char *headers);

/* Replace the document with `html` (IPersistStreamInit::Load / DocumentText); reports
 * DocumentComplete for it afterwards, from the message loop, as IE does. */
int page_set_content(Page *pg, const char *html);

/* The site's IDocObjectService has just arrived: report the navigation events the document
 * reached before it (from the message loop). */
void page_catch_up(Page *pg);

/* IHTMLElement2::focus/blur held the page's focus events back: fire them from the message
 * loop, as IE does. */
void page_replay_focus(Page *pg);

/* Stash ids (dom.h): the first id for a document that has stashed nothing yet, and the
 * notice that `id` came back (the base was taken, so the next document gets a new block). */
long page_stash_base(Page *pg);
void page_stash_used(Page *pg, long id);

/* IPersistStreamInit::Save: the response body as the server sent it, in the page's charset
 * (falls back to the serialised DOM). */
HRESULT page_save(Page *pg, IStream *stm);

/* Screencast into `hwnd` (NULL stops), and size the viewport. */
void page_attach_window(Page *pg, HWND hwnd);
void page_resize(Page *pg, int w, int h);
/* TEBEOS_EMBED=1: a visible Chromium draws the page (and shows its own context menu) */
int page_embed_mode(void);
/* embed mode: report the shim window again if it moved or changed size (shim window timer) */
void page_embed_tick(Page *pg);
struct RenderCtx *page_render(Page *pg);

/* window.event while a DOM event is being dispatched (borrowed; NULL otherwise). */
struct HtmlEventObj *page_current_event(Page *pg);
void page_set_current_event(Page *pg, struct HtmlEventObj *evo);

#endif /* SHIM_PAGE_H */

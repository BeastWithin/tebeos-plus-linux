/* privif.h -- the two private interfaces between Wine's ieframe and its mshtml.
 *
 * Neither is in the llvm-mingw headers. Both are declared from Wine 11.18:
 *   IHTMLPrivateWindow  include/mshtml.idl            {3050f6dc-98b5-11cf-bb82-00aa00bdce0b}
 *   IDocObjectService   include/docobjectservice.idl  {3050f801-98b5-11cf-bb82-00aa00bdce0b}
 *
 * The protocol (mirrors Wine mshtml oleobj.c SetClientSite):
 *   - the document hands its window to the client site with
 *     IOleCommandTarget::Exec(&CGID_DocHostCmdPriv, DOCHOST_DOCCANNAVIGATE, 0, VT_UNKNOWN window);
 *     from then on ieframe sends WebBrowser.Navigate to IHTMLPrivateWindow::SuperNavigate;
 *   - the document asks QueryService(SID_SShellBrowser, IID_IDocObjectService) and reports its
 *     own navigations through it (BeforeNavigate2 / NavigateComplete2 / DocumentComplete).
 *     Once that service exists, ieframe stops firing NavigateComplete2/DocumentComplete itself.
 */

#ifndef SHIM_PRIVIF_H
#define SHIM_PRIVIF_H

#include <windows.h>
#include <ole2.h>
#include <mshtml.h>
#include <docobj.h>

DEFINE_GUID(IID_IHTMLPrivateWindow, 0x3050f6dc, 0x98b5, 0x11cf, 0xbb, 0x82, 0x00, 0xaa, 0x00, 0xbd, 0xce, 0x0b);
DEFINE_GUID(IID_IDocObjectService,  0x3050f801, 0x98b5, 0x11cf, 0xbb, 0x82, 0x00, 0xaa, 0x00, 0xbd, 0xce, 0x0b);
DEFINE_GUID(SHIM_CGID_DocHostCmdPriv, 0x000214d4, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46);
DEFINE_GUID(SHIM_SID_SShellBrowser,  0x000214e2, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46);  /* = IID_IShellBrowser */
DEFINE_GUID(SHIM_SID_SWebBrowserApp, 0x0002df05, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46);  /* = IID_IWebBrowserApp */
DEFINE_GUID(SHIM_IID_IBrowserService, 0x02ba3b52, 0x0547, 0x11d1, 0xb8, 0x33, 0x00, 0xc0, 0x4f, 0xc9, 0xb3, 0x1f);
DEFINE_GUID(SHIM_DIID_HTMLWindowEvents2, 0x3050f625, 0x98b5, 0x11cf, 0xbb, 0x82, 0x00, 0xaa, 0x00, 0xbd, 0xce, 0x0b);

#define SHIM_DOCHOST_DOCCANNAVIGATE 0

typedef struct IHTMLPrivateWindow IHTMLPrivateWindow;
typedef struct IHTMLPrivateWindowVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IHTMLPrivateWindow *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IHTMLPrivateWindow *This);
    ULONG   (STDMETHODCALLTYPE *Release)(IHTMLPrivateWindow *This);
    HRESULT (STDMETHODCALLTYPE *SuperNavigate)(IHTMLPrivateWindow *This, BSTR url, BSTR arg2, BSTR arg3,
                                               BSTR arg4, VARIANT *post_data, VARIANT *headers, ULONG flags);
    HRESULT (STDMETHODCALLTYPE *GetPendingUrl)(IHTMLPrivateWindow *This, BSTR *url);
    HRESULT (STDMETHODCALLTYPE *SetPICSTarget)(IHTMLPrivateWindow *This, IOleCommandTarget *cmdtrg);
    HRESULT (STDMETHODCALLTYPE *PICSComplete)(IHTMLPrivateWindow *This, int arg);
    HRESULT (STDMETHODCALLTYPE *FindWindowByName)(IHTMLPrivateWindow *This, LPCWSTR name, IHTMLWindow2 **ret);
    HRESULT (STDMETHODCALLTYPE *GetAddressBarUrl)(IHTMLPrivateWindow *This, BSTR *url);
} IHTMLPrivateWindowVtbl;
struct IHTMLPrivateWindow { IHTMLPrivateWindowVtbl *lpVtbl; };

typedef struct IDocObjectService IDocObjectService;
typedef struct IDocObjectServiceVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDocObjectService *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDocObjectService *This);
    ULONG   (STDMETHODCALLTYPE *Release)(IDocObjectService *This);
    HRESULT (STDMETHODCALLTYPE *FireBeforeNavigate2)(IDocObjectService *This, IDispatch *pDispatch,
                                                     LPCWSTR lpszUrl, DWORD dwFlags, LPCWSTR lpszFrameName,
                                                     BYTE *pPostData, DWORD cbPostData, LPCWSTR lpszHeaders,
                                                     BOOL fPlayNavSound, BOOL *pfCancel);
    HRESULT (STDMETHODCALLTYPE *FireNavigateComplete2)(IDocObjectService *This, IHTMLWindow2 *pHTMLWindow2, DWORD dwFlags);
    HRESULT (STDMETHODCALLTYPE *FireDownloadBegin)(IDocObjectService *This);
    HRESULT (STDMETHODCALLTYPE *FireDownloadComplete)(IDocObjectService *This);
    HRESULT (STDMETHODCALLTYPE *FireDocumentComplete)(IDocObjectService *This, IHTMLWindow2 *pHTMLWindow, DWORD dwFlags);
    HRESULT (STDMETHODCALLTYPE *UpdateDesktopComponent)(IDocObjectService *This, IHTMLWindow2 *pHTMLWindow);
    HRESULT (STDMETHODCALLTYPE *GetPendingUrl)(IDocObjectService *This, BSTR *pbstrPendingUrl);
    HRESULT (STDMETHODCALLTYPE *ActiveElementChanged)(IDocObjectService *This, IHTMLElement *pHTMLElement);
    HRESULT (STDMETHODCALLTYPE *GetUrlSearchComponent)(IDocObjectService *This, BSTR *pbstrSearch);
    HRESULT (STDMETHODCALLTYPE *IsErrorUrl)(IDocObjectService *This, LPCWSTR lpszUrl, BOOL *pfIsError);
} IDocObjectServiceVtbl;
struct IDocObjectService { IDocObjectServiceVtbl *lpVtbl; };

#endif /* SHIM_PRIVIF_H */

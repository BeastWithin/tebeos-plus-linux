/* wintest.c: create a child HWND and print the X11 window id Wine keeps for it */
#include <windows.h>
#include <stdio.h>
static LRESULT CALLBACK wp(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmd, int show) {
    WNDCLASSW wc = {0}; HWND top, child; char buf[256]; FILE *f;
    wc.lpfnWndProc = wp; wc.hInstance = hi; wc.lpszClassName = L"SpikeWin"; RegisterClassW(&wc);
    top = CreateWindowW(L"SpikeWin", L"spike-top", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 800, 600, NULL, NULL, hi, NULL);
    child = CreateWindowW(L"SpikeWin", L"spike-child", WS_CHILD | WS_VISIBLE, 0, 0, 800, 600, top, NULL, hi, NULL);
    ShowWindow(top, SW_SHOW);
    snprintf(buf, sizeof buf, "top_x11=%p child_x11=%p child_hwnd=%p\n",
             (void *)GetPropW(top, L"__wine_x11_whole_window"),
             (void *)GetPropW(child, L"__wine_x11_whole_window"), (void *)child);
    f = fopen("C:\\spike-result.txt", "w"); if (f) { fputs(buf, f); fclose(f); }
    for (int i = 0; i < 600; i++) { MSG m; while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } Sleep(100); }
    return 0;
}

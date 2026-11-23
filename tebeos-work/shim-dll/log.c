/* log.c -- see log.h. */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "log.h"

#define LOG_CAP (20L * 1024 * 1024)

static CRITICAL_SECTION g_log_lock;
static INIT_ONCE g_log_once = INIT_ONCE_STATIC_INIT;
static HANDLE g_log_file = INVALID_HANDLE_VALUE;
static int g_log_off = 0;
static long g_log_bytes = 0;

static BOOL CALLBACK log_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    char path[MAX_PATH];
    DWORD n;

    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_log_lock);
    n = GetEnvironmentVariableA("SHIM_LOG", path, sizeof(path));
    if (n > 0 && n < sizeof(path) && _stricmp(path, "off") == 0) {
        g_log_off = 1;
        return TRUE;
    }
    if (n == 0 || n >= sizeof(path)) {
        n = GetTempPathA(sizeof(path), path);
        if (n == 0 || n + 16 >= sizeof(path))
            strcpy(path, "C:\\");
        strcat(path, "mshtml-cdp.log");
    }
    g_log_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log_file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz;
        if (GetFileSizeEx(g_log_file, &sz))
            g_log_bytes = sz.QuadPart > LOG_CAP ? LOG_CAP : (long)sz.QuadPart;
    }
    return TRUE;
}

int shim_tracing(void)
{
    static int trace = -1;
    if (trace < 0) {
        char buf[8];
        char path[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("SHIM_TRACE", buf, sizeof(buf)), t;
        trace = n > 0 && n < sizeof(buf) && buf[0] == '1';
        /* or a file %TEMP%\mshtml-cdp.trace when the process starts: for a process whose
         * environment is hard to set, such as Medula started from TEBEOS */
        t = GetTempPathA(sizeof(path), path);
        if (!trace && t > 0 && t + 20 < sizeof(path)) {
            strcat(path, "mshtml-cdp.trace");
            trace = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
        }
    }
    return trace;
}

void shim_log(const char *fmt, ...)
{
    char buf[2048];
    int n, m;
    va_list ap;
    DWORD w;

    InitOnceExecuteOnce(&g_log_once, log_init, NULL, NULL);
    if (g_log_off || g_log_file == INVALID_HANDLE_VALUE || g_log_bytes >= LOG_CAP)
        return;

    n = snprintf(buf, sizeof(buf), "%10lu %04lx ", (unsigned long)GetTickCount(),
                 (unsigned long)GetCurrentThreadId());
    va_start(ap, fmt);
    m = vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0)
        m = 0;
    n += m;
    if (n > (int)sizeof(buf) - 2)
        n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';

    EnterCriticalSection(&g_log_lock);
    if (g_log_bytes < LOG_CAP && WriteFile(g_log_file, buf, (DWORD)n, &w, NULL))
        g_log_bytes += (long)w;
    LeaveCriticalSection(&g_log_lock);
}

void shim_stub_hit(volatile long *flag, const char *name)
{
    if (InterlockedExchange((LONG *)flag, 1) == 0)
        shim_log("STUB %s", name);
}

void shim_qi_miss(const char *object, const void *riid)
{
    static struct { const char *obj; GUID iid; } seen[256];
    static LONG nseen = 0;
    const GUID *g = (const GUID *)riid;
    LONG i, n = nseen;

    for (i = 0; i < n && i < 256; i++)
        if (seen[i].obj == object && IsEqualGUID(&seen[i].iid, g))
            return;
    i = InterlockedIncrement(&nseen) - 1;
    if (i < 256) {
        seen[i].obj = object;
        seen[i].iid = *g;
    }
    shim_log("QI-MISS %s {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", object,
             (unsigned long)g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2],
             g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

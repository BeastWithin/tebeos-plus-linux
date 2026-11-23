/* log.h -- bounded diagnostic log for the shim.
 *
 * Writes to %SHIM_LOG% (a Windows path) or %TEMP%\mshtml-cdp.log. SHIM_LOG=off disables it.
 * Capped at 20 MB so a chatty run can never fill the disk. Thread-safe.
 */
#ifndef SHIM_LOG_H
#define SHIM_LOG_H

void shim_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Log the first call of an unimplemented (E_NOTIMPL) generated stub. `flag` is the
 * stub's own once-flag, so each stub is reported once per process. */
void shim_stub_hit(volatile long *flag, const char *name);

/* Log a QueryInterface miss once per (object kind, IID). */
void shim_qi_miss(const char *object, const void *riid);

/* SHIM_TRACE=1: log every synchronous CDP call, UI event, Fire* into ieframe, view and
 * window change, and screencast frame. For finding where the real app stalls. */
int shim_tracing(void);

#endif

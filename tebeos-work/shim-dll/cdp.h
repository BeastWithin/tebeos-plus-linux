/* cdp.h -- minimal Chrome DevTools Protocol client over WebSocket, for the Path-A shim.
 *
 * Mirrors the semantics of shim-engine/cdp.py: connect to a Chromium-family debugging
 * port (e.g. the app's in-process CEF on 8080, or a self-hosted CEF later), send method
 * calls, and receive responses/events. Self-test in cdptest.c (mock server, no Chromium).
 */

#ifndef SHIM_CDP_H
#define SHIM_CDP_H

#include <stddef.h>
#include "json.h"

typedef struct CdpConn CdpConn;

/* Connect to host:port and perform the WebSocket HTTP upgrade. Returns NULL on failure. */
CdpConn *cdp_connect(const char *host, int port);

/* Connect to a specific ws://host:port/path (the path is the CDP target). */
CdpConn *cdp_connect_wsurl(const char *ws_url);

/* Plain HTTP GET (used for /json discovery). Returns a malloc'd body (free()), or NULL. */
char *cdp_http_get(const char *host, int port, const char *path);

/* Fetch /json and copy the first page target's webSocketDebuggerUrl into out (cap bytes).
 * Returns 0 on success, -1 if no page target. */
int cdp_first_page_wsurl(const char *host, int port, char *out, size_t cap);

void cdp_close(CdpConn *c);

/* Send a method call. params_json is the already-serialized JSON object text (or NULL/"{}").
 * Blocks until the matching reply arrives, returning it in *reply (a JVal*, which the
 * caller must j_free). The `id` argument is IGNORED: ids are auto-assigned from the
 * shared monotonic counter also used by cdp_command(), so call and fire-and-forget ids
 * cannot collide. Returns 0 on success, -1 on transport failure. */
int cdp_call(CdpConn *c, long id, const char *method, const char *params_json,
             JVal **reply);

/* Block until the next message (a response whose id has no waiter, or an event) arrives.
 * *msg is the parsed message (caller j_free). Returns 0, -2 if the receive timeout expired
 * with nothing read (poll again), -1 on transport failure/close. */
int cdp_recv(CdpConn *c, JVal **msg);

/* Fire-and-forget: send a method call with a fresh id and DO NOT wait for the reply.
 * The reply is later consumed (and ignored) by whichever cdp_recv/cdp_call loop is
 * running. Used for screencast acks and Input.* forwarding. Returns 0 on send success. */
int cdp_command(CdpConn *c, const char *method, const char *params_json);

/* The id of the last request sent on c (0 if none). */
long cdp_last_id(CdpConn *c);

/* Receive timeout in ms for cdp_recv/cdp_call (default 3000; 0 = wait forever). */
void cdp_set_timeout(CdpConn *c, unsigned ms);

/* Wake a thread blocked in cdp_recv on `c` (it returns -1); the caller still cdp_close()s. */
void cdp_shutdown(CdpConn *c);

/* base64 helpers (used by the transport and by cdptest's mock server). */
char *b64_encode(const unsigned char *in, size_t inlen);   /* malloc'd, free() */
int   b64_decode(const char *in, unsigned char *out, size_t outcap); /* -> length, -1 err */

#endif /* SHIM_CDP_H */

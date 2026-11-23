/* cdp.c -- minimal Chrome DevTools Protocol client over WebSocket (RFC 6455).
 *
 * Self-contained native transport for the Path-A shim: TCP connect + HTTP upgrade +
 * client-masked text frames + server frame reassembly, with a thin CDP request/reply
 * layer on top. Mirrors shim-engine/cdp.py. See cdptest.c for the mock-server self-test.
 */

#include "cdp.h"
#include "json.h"
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ base64 */

static const char b64_a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *b64_encode(const unsigned char *in, size_t inlen)
{
    size_t olen = ((inlen + 2) / 3) * 4;
    char *out = (char *)malloc(olen + 1);
    size_t i, j = 0;
    if (!out) return NULL;
    for (i = 0; i < inlen; i += 3) {
        unsigned a = in[i];
        unsigned b = (i + 1 < inlen) ? in[i + 1] : 0;
        unsigned c = (i + 2 < inlen) ? in[i + 2] : 0;
        out[j++] = b64_a[a >> 2];
        out[j++] = b64_a[((a & 3) << 4) | (b >> 4)];
        out[j++] = (i + 1 < inlen) ? b64_a[((b & 15) << 2) | (c >> 6)] : '=';
        out[j++] = (i + 2 < inlen) ? b64_a[c & 63] : '=';
    }
    out[j] = '\0';
    return out;
}

static int b64_val(char ch)
{
    const char *p = strchr(b64_a, ch);
    if (p) return (int)(p - b64_a);
    if (ch == '=') return 0;
    return -1;
}

int b64_decode(const char *in, unsigned char *out, size_t outcap)
{
    size_t len = strlen(in), n = 0;
    size_t i;
    unsigned long acc = 0;
    unsigned bits = 0;
    for (i = 0; i < len; i++) {
        int v;
        if (in[i] == '=') break; /* padding: stop */
        v = b64_val(in[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= outcap) return (int)n; /* output buffer full: partial decode is not an error */
            out[n++] = (unsigned char)((acc >> bits) & 0xFF);
            acc &= (1UL << bits) - 1; /* drop emitted bits so acc never overflows */
        }
    }
    return (int)n;
}

/* ------------------------------------------------------------------ WebSocket */

struct CdpConn {
    SOCKET s;
    long next_id;
    CRITICAL_SECTION send_lock; /* serializes ws_send_text + next_id (cross-thread sends) */
    unsigned char *rbuf;        /* text reassembly buffer */
    size_t rlen, rcap;
};

static int ws_started = 0;

static int ensure_winsock(void)
{
    if (!ws_started) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0)
            return -1;
        ws_started = 1;
    }
    return 0;
}

/* read exactly n bytes from the socket (blocking). Returns 0 on success, -1 on error.
 * A receive timeout (SO_RCVTIMEO) before the first byte returns -2 when `may_time_out`,
 * so the caller can poll; once part of a frame has arrived, timeouts are retried, since
 * giving up mid-frame would leave the stream desynchronised. */
static int read_exact_t(SOCKET s, void *buf, int n, int may_time_out)
{
    char *p = (char *)buf;
    int got = 0, stalls = 0;
    while (n > 0) {
        int r = recv(s, p, n, 0);
        if (r < 0 && WSAGetLastError() == WSAETIMEDOUT) {
            if (!got && may_time_out)
                return -2;
            if (++stalls > 20)
                return -1;
            continue;
        }
        if (r <= 0) return -1;
        p += r;
        n -= r;
        got += r;
    }
    return 0;
}

static int read_exact(SOCKET s, void *buf, int n)
{
    return read_exact_t(s, buf, n, 0);
}

static int write_exact(SOCKET s, const void *buf, int n)
{
    const char *p = (const char *)buf;
    while (n > 0) {
        int w = send(s, p, n, 0);
        if (w <= 0) return -1;
        p += w;
        n -= w;
    }
    return 0;
}

static CdpConn *cdp_connect_path(const char *host, int port, const char *path)
{
    struct hostent *he;
    struct sockaddr_in addr;
    CdpConn *c;
    unsigned char keybytes[16];
    char *key, *req;
    char resp[4096];
    int i, n;

    if (ensure_winsock() != 0) return NULL;

    he = gethostbyname(host);
    if (!he) return NULL;

    c = (CdpConn *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->next_id = 1;
    c->s = INVALID_SOCKET;
    InitializeCriticalSection(&c->send_lock);

    c->s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c->s == INVALID_SOCKET) { cdp_close(c); return NULL; }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect(c->s, (struct sockaddr *)&addr, sizeof(addr)) != 0) { cdp_close(c); return NULL; }
    {
        BOOL on = TRUE;
        setsockopt(c->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
    }

    /* Receive timeout (WinSock: DWORD milliseconds) so a silently-dead target can't
     * block cdp_recv/cdp_call forever (e.g. the screencast stream going quiet). */
    {
        DWORD tv = 3000;
        setsockopt(c->s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    }

    /* random-ish 16-byte key (not cryptographically strong; fine for the handshake) */
    srand((unsigned)GetTickCount());
    for (i = 0; i < 16; i++) keybytes[i] = (unsigned char)(rand() & 0xFF);
    key = b64_encode(keybytes, 16);

    n = snprintf(NULL, 0,
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n", path, host, port, key);
    req = (char *)malloc((size_t)n + 1);
    sprintf(req,
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n", path, host, port, key);
    free(key);

    if (write_exact(c->s, req, n) != 0) { free(req); cdp_close(c); return NULL; }
    free(req);

    /* read response headers */
    n = 0;
    while (n < (int)sizeof(resp) - 1) {
        int r = recv(c->s, resp + n, 1, 0);
        if (r <= 0) { cdp_close(c); return NULL; }
        n += r;
        resp[n] = '\0';
        if (strstr(resp, "\r\n\r\n") != NULL) break;
    }
    if (c->s == INVALID_SOCKET) return NULL;
    if (strstr(resp, "101") == NULL) { cdp_close(c); return NULL; }

    return c;
}

void cdp_close(CdpConn *c)
{
    if (!c) return;
    if (c->s != INVALID_SOCKET) {
        closesocket(c->s);
        c->s = INVALID_SOCKET;
    }
    DeleteCriticalSection(&c->send_lock);
    free(c->rbuf);
    free(c);
}

CdpConn *cdp_connect(const char *host, int port)
{
    /* fixed path: used by cdptest.c's mock server, which ignores the path. */
    return cdp_connect_path(host, port, "/devtools/page/tebeos");
}

CdpConn *cdp_connect_wsurl(const char *ws_url)
{
    char host[128];
    char path[512];
    int port = 9222, i;
    const char *p = ws_url;

    if (strncmp(p, "ws://", 5) == 0) p += 5;
    else if (strncmp(p, "wss://", 6) == 0) return NULL; /* TLS not supported */
    else return NULL;

    i = 0;
    while (*p && *p != ':' && *p != '/' && i < 127) host[i++] = *p++;
    host[i] = '\0';
    if (*p == ':') { p++; port = atoi(p); while (*p && *p != '/') p++; }
    if (*p == '/') snprintf(path, sizeof(path), "%s", p);
    else strcpy(path, "/");

    return cdp_connect_path(host, port, path);
}

char *cdp_http_get(const char *host, int port, const char *path)
{
    struct hostent *he;
    struct sockaddr_in addr;
    SOCKET s;
    char req[1024], *resp = NULL;
    size_t cap = 0, n = 0;

    if (ensure_winsock() != 0) return NULL;
    he = gethostbyname(host);
    if (!he) return NULL;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return NULL;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) { closesocket(s); return NULL; }

    snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", path, host, port);
    if (write_exact(s, req, (int)strlen(req)) != 0) { closesocket(s); return NULL; }

    /* read headers (byte-wise so we don't over-read into the body) */
    cap = 4096; n = 0;
    resp = (char *)malloc(cap);
    for (;;) {
        if (read_exact(s, resp + n, 1) != 0) { closesocket(s); free(resp); return NULL; }
        n++;
        if (n + 1 >= cap) { cap *= 2; resp = (char *)realloc(resp, cap); }
        resp[n] = '\0';
        if (n >= 4 && strstr(resp, "\r\n\r\n") != NULL) break;
    }
    {
        char *cl = strstr(resp, "Content-Length:");
        long bodylen = 0;
        char *body;
        if (!cl) {
            char *lcl = strstr(resp, "content-length:");
            cl = lcl;
        }
        if (cl) {
            bodylen = atol(cl + 15);
        }
        body = (char *)malloc((size_t)bodylen + 1);
        if (bodylen > 0 && read_exact(s, body, (int)bodylen) != 0) {
            closesocket(s); free(resp); free(body); return NULL;
        }
        body[bodylen] = '\0';
        closesocket(s);
        free(resp);
        return body;
    }
}

int cdp_first_page_wsurl(const char *host, int port, char *out, size_t cap)
{
    char *body = cdp_http_get(host, port, "/json");
    JVal *arr;
    size_t k, off = 0;
    int ret = -1;

    if (!body) return -1;
    arr = j_parse(body, &off);
    free(body);
    if (!arr || arr->t != J_ARR) { if (arr) j_free(arr); return -1; }
    for (k = 0; k < arr->n; k++) {
        JVal *t = j_get(&arr->items[k], "type");
        JVal *u = j_get(&arr->items[k], "webSocketDebuggerUrl");
        if (t && u && j_str(t) && strcmp(j_str(t), "page") == 0 && j_str(u)) {
            snprintf(out, cap, "%s", j_str(u));
            ret = 0;
            break;
        }
    }
    j_free(arr);
    return ret;
}

/* send one masked text frame */
static int ws_send_text(CdpConn *c, const char *text)
{
    size_t len = strlen(text);
    unsigned char hdr[14];
    unsigned char mask[4] = { 0x11, 0x22, 0x33, 0x44 };
    size_t hlen = 2;
    size_t i;
    unsigned char *payload;
    int ok;

    hdr[0] = 0x81; /* FIN | text */
    if (len < 126) {
        hdr[1] = 0x80 | (unsigned char)len;
    } else if (len < 65536) {
        hdr[1] = 0x80 | 126;
        hdr[2] = (unsigned char)(len >> 8);
        hdr[3] = (unsigned char)(len & 0xFF);
        hlen = 4;
    } else {
        unsigned long long ll = len;
        int k;
        hdr[1] = 0x80 | 127;
        for (k = 0; k < 8; k++) hdr[9 - k] = (unsigned char)((ll >> (8 * k)) & 0xFF);
        hlen = 10;
    }

    /* One send per frame. Header, key and payload as three sends made Nagle hold the payload
     * for Chromium's delayed ACK: every synchronous call took 42-44 ms (2026-10-22). */
    payload = (unsigned char *)malloc(hlen + 4 + len);
    if (!payload) return -1;
    memcpy(payload, hdr, hlen);
    memcpy(payload + hlen, mask, 4);
    for (i = 0; i < len; i++) payload[hlen + 4 + i] = (unsigned char)text[i] ^ mask[i & 3];

    ok = write_exact(c->s, payload, (int)(hlen + 4 + len));

    free(payload);
    return ok;
}

/* read one message (text), reassembling continuations and handling ping/pong. */
static int ws_recv_text(CdpConn *c, char **out, size_t *outlen)
{
    size_t buf_n = 0, buf_cap = 0;
    char *buf = NULL;
    int fin;

    *out = NULL;
    *outlen = 0;

    for (;;) {
        unsigned char h[2];
        unsigned char op, masked;
        unsigned long long len;
        unsigned char mkey[4];
        int i;

        {
            int r = read_exact_t(c->s, h, 2, buf_n == 0);
            if (r != 0) { free(buf); return r; }
        }
        fin = (h[0] & 0x80) != 0;
        op = h[0] & 0x0F;
        masked = (h[1] & 0x80) != 0;
        len = h[1] & 0x7F;

        if (len == 126) {
            unsigned char e[2];
            if (read_exact(c->s, e, 2) != 0) { free(buf); return -1; }
            len = ((unsigned long long)e[0] << 8) | e[1];
        } else if (len == 127) {
            unsigned char e[8];
            len = 0;
            if (read_exact(c->s, e, 8) != 0) { free(buf); return -1; }
            for (i = 0; i < 8; i++) len = (len << 8) | e[i];
        }

        if (masked && read_exact(c->s, mkey, 4) != 0) { free(buf); return -1; }

        if (op == 0x9) { /* ping -> pong */
            unsigned char *pp = (unsigned char *)malloc((size_t)(len + 1));
            unsigned char phdr[2] = { 0x8A, 0x80 | (unsigned char)len };
            if (read_exact(c->s, pp, (int)len) != 0) { free(pp); free(buf); return -1; }
            write_exact(c->s, phdr, 2);
            write_exact(c->s, mkey, 4); /* echo the pong with the same mask key? we re-mask below */
            /* simpler: send own masked pong of the same payload */
            for (i = 0; i < (int)len; i++) pp[i] ^= mkey[i & 3];
            write_exact(c->s, pp, (int)len);
            free(pp);
            continue;
        }
        if (op == 0xA) { /* pong */
            unsigned char *ign = (unsigned char *)malloc((size_t)(len + 1));
            if (read_exact(c->s, ign, (int)len) != 0) { free(ign); free(buf); return -1; }
            free(ign);
            continue;
        }
        if (op == 0x8) { /* close */
            free(buf);
            return -1;
        }
        if (op != 0x1 && op != 0x0) { /* binary/other: skip payload */
            while (len > 0) {
                unsigned char tmp[512];
                unsigned long long chunk = len > sizeof(tmp) ? sizeof(tmp) : len;
                if (read_exact(c->s, tmp, (int)chunk) != 0) { free(buf); return -1; }
                if (masked) for (i = 0; i < (int)chunk; i++) tmp[i] ^= mkey[i & 3];
                len -= chunk;
            }
            continue;
        }

        /* text or continuation */
        {
            unsigned char *pl = (unsigned char *)malloc((size_t)(len + 1));
            if (read_exact(c->s, pl, (int)len) != 0) { free(pl); free(buf); return -1; }
            if (masked) for (i = 0; i < (int)len; i++) pl[i] ^= mkey[i & 3];
            if (buf_n + (size_t)len + 1 > buf_cap) {
                while (buf_n + (size_t)len + 1 > buf_cap) buf_cap = buf_cap ? buf_cap * 2 : 4096;
                buf = (char *)realloc(buf, buf_cap);
            }
            memcpy(buf + buf_n, pl, (size_t)len);
            buf_n += (size_t)len;
            free(pl);
            if (fin) {
                buf[buf_n] = '\0';
                *out = buf;
                *outlen = buf_n;
                return 0;
            }
        }
    }
}

/* ------------------------------------------------------------------ CDP layer */

static int cdp_send_request(CdpConn *c, long id, const char *method, const char *params_json)
{
    const char *p = params_json && *params_json ? params_json : "{}";
    size_t n = strlen(method) + strlen(p) + 64;
    char *msg = (char *)malloc(n);
    int r;
    if (!msg)
        return -1;
    snprintf(msg, n, "{\"id\":%ld,\"method\":\"%s\",\"params\":%s}", id, method, p);
    EnterCriticalSection(&c->send_lock);
    r = c->s == INVALID_SOCKET ? -1 : ws_send_text(c, msg);
    LeaveCriticalSection(&c->send_lock);
    free(msg);
    return r;
}

int cdp_call(CdpConn *c, long id, const char *method, const char *params_json, JVal **reply)
{
    char *text;
    size_t len;
    long i, my_id;

    (void)id; /* id is auto-assigned from the shared counter -- see note below */
    *reply = NULL;

    /* Use the same monotonic c->next_id counter as cdp_command() so a fire-and-forget
     * command (e.g. a screencast ack, or an Input.* event) and this synchronous call
     * can never be issued with the same id -- otherwise cdp_call would match the wrong
     * reply. next_id is protected by send_lock. */
    EnterCriticalSection(&c->send_lock);
    my_id = c->next_id++;
    LeaveCriticalSection(&c->send_lock);

    if (cdp_send_request(c, my_id, method, params_json) != 0)
        return -1;

    for (i = 0; i < 100000; i++) {
        JVal *m;
        size_t off = 0;
        if (ws_recv_text(c, &text, &len) != 0)
            return -1;   /* timeout or closed; a late reply is dropped by the next call */
        m = j_parse(text, &off);
        free(text);
        if (!m)
            return -1;
        {
            JVal *idv = j_get(m, "id");
            if (idv && (long)j_num(idv) == my_id) {
                *reply = m;
                return 0;
            }
        }
        j_free(m); /* event or unrelated response: drop for now */
    }
    return -1;
}

int cdp_recv(CdpConn *c, JVal **msg)
{
    char *text;
    size_t len, off = 0;
    int r;
    *msg = NULL;
    r = ws_recv_text(c, &text, &len);
    if (r != 0)
        return r;
    *msg = j_parse(text, &off);
    free(text);
    return *msg ? 0 : -1;
}

long cdp_last_id(CdpConn *c)
{
    long id;
    EnterCriticalSection(&c->send_lock);
    id = c->next_id - 1;
    LeaveCriticalSection(&c->send_lock);
    return id;
}

int cdp_command(CdpConn *c, const char *method, const char *params_json)
{
    long id;
    EnterCriticalSection(&c->send_lock);
    id = c->next_id++;
    LeaveCriticalSection(&c->send_lock);
    return cdp_send_request(c, id, method, params_json);
}

void cdp_set_timeout(CdpConn *c, unsigned ms)
{
    DWORD tv = ms;
    if (c && c->s != INVALID_SOCKET)
        setsockopt(c->s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
}

void cdp_shutdown(CdpConn *c)
{
    if (c && c->s != INVALID_SOCKET)
        shutdown(c->s, SD_BOTH);
}

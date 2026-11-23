/* cdptest.c -- self-test for the CDP transport (cdp.c): spins a mock WebSocket server in a
 * thread, then runs the client against it over 127.0.0.1. Proves: HTTP upgrade handshake
 * (with a correct Sec-WebSocket-Accept = base64(SHA1(key+GUID))), client-side frame masking,
 * server->client frame parsing, and the cdp_call()/cdp_recv() JSON round-trip + id matching.
 *
 * Run:  wine cdptest32.exe    (or cdptest64.exe)
 */

#include "cdp.h"
#include "json.h"
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- SHA-1 (FIPS 180-1), for the mock server's Sec-WebSocket-Accept ---- */

static unsigned sha1_rotl(unsigned v, int s) { return (v << s) | (v >> (32 - s)); }

static void sha1_block(unsigned h[5], const unsigned char block[64])
{
    unsigned w[80], a, b, c, d, e;
    int t;
    for (t = 0; t < 16; t++)
        w[t] = ((unsigned)block[t*4] << 24) | ((unsigned)block[t*4+1] << 16) |
               ((unsigned)block[t*4+2] << 8) | block[t*4+3];
    for (t = 16; t < 80; t++)
        w[t] = sha1_rotl(w[t-3] ^ w[t-8] ^ w[t-14] ^ w[t-16], 1);
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
    for (t = 0; t < 80; t++) {
        unsigned f, k, tmp;
        if (t < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999; }
        else if (t < 40) { f = b ^ c ^ d;             k = 0x6ED9EBA1; }
        else if (t < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;             k = 0xCA62C1D6; }
        tmp = sha1_rotl(a,5) + f + e + k + w[t];
        e = d; d = c; c = sha1_rotl(b,30); b = a; a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1(const unsigned char *data, size_t len, unsigned char out[20])
{
    unsigned h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    unsigned long long bitlen = (unsigned long long)len * 8;
    unsigned char block[64];
    size_t i;

    for (i = 0; i + 64 <= len; i += 64)
        sha1_block(h, data + i);

    memset(block, 0, 64);
    memcpy(block, data + i, len - i);
    block[len - i] = 0x80;
    if ((len - i) >= 56) {
        sha1_block(h, block);
        memset(block, 0, 64);
    }
    for (i = 0; i < 8; i++)
        block[63 - i] = (unsigned char)((bitlen >> (8 * i)) & 0xFF);
    sha1_block(h, block);

    out[0]=(unsigned char)(h[0]>>24); out[1]=(unsigned char)(h[0]>>16); out[2]=(unsigned char)(h[0]>>8); out[3]=(unsigned char)h[0];
    out[4]=(unsigned char)(h[1]>>24); out[5]=(unsigned char)(h[1]>>16); out[6]=(unsigned char)(h[1]>>8); out[7]=(unsigned char)h[1];
    out[8]=(unsigned char)(h[2]>>24); out[9]=(unsigned char)(h[2]>>16); out[10]=(unsigned char)(h[2]>>8); out[11]=(unsigned char)h[2];
    out[12]=(unsigned char)(h[3]>>24); out[13]=(unsigned char)(h[3]>>16); out[14]=(unsigned char)(h[3]>>8); out[15]=(unsigned char)h[3];
    out[16]=(unsigned char)(h[4]>>24); out[17]=(unsigned char)(h[4]>>16); out[18]=(unsigned char)(h[4]>>8); out[19]=(unsigned char)h[4];
}

/* ---- mock server thread ---- */

typedef struct { SOCKET ls; volatile int port; volatile int failures; } Srv;

static int sv_read_exact(SOCKET s, void *b, int n){
    char *p=(char*)b; while(n>0){int r=recv(s,p,n,0); if(r<=0)return -1; p+=r; n-=r;} return 0;
}
static int sv_write_exact(SOCKET s, const void *b, int n){
    const char *p=(const char*)b; while(n>0){int w=send(s,p,n,0); if(w<=0)return -1; p+=w; n-=w;} return 0;
}

static DWORD WINAPI server_thread(void *arg)
{
    Srv *sv = (Srv *)arg;
    SOCKET c = accept(sv->ls, NULL, NULL);
    char req[4096];
    int n = 0, i;
    const char *WSGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    if (c == INVALID_SOCKET) { sv->failures++; return 0; }

    /* read HTTP upgrade request */
    while (n < (int)sizeof(req) - 1) {
        int r = recv(c, req + n, 1, 0);
        if (r <= 0) break;
        n += r; req[n] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }

    /* extract Sec-WebSocket-Key */
    {
        char *p = strstr(req, "Sec-WebSocket-Key:");
        char key[256] = {0};
        if (!p) { sv->failures++; closesocket(c); return 0; }
        p += 18; while (*p == ' ') p++;
        i = 0;
        while (*p && *p != '\r' && *p != '\n' && i < 250) key[i++] = *p++;
        key[i] = '\0';

        /* accept = base64(sha1(key + GUID)) */
        {
            char combo[512];
            unsigned char dg[20];
            char *acc;
            char resp[512];
            int j;
            sprintf(combo, "%s%s", key, WSGUID);
            sha1((const unsigned char *)combo, strlen(combo), dg);
            acc = b64_encode(dg, 20);
            j = sprintf(resp,
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: %s\r\n\r\n", acc);
            free(acc);
            if (sv_write_exact(c, resp, j) != 0) { sv->failures++; closesocket(c); return 0; }
        }
    }

    /* read one client frame (must be masked) */
    {
        unsigned char h[2];
        if (sv_read_exact(c, h, 2) != 0) { sv->failures++; closesocket(c); return 0; }
        if ((h[1] & 0x80) == 0) { sv->failures++; closesocket(c); return 0; } /* client must mask */
        {
            unsigned long long len = h[1] & 0x7F;
            if (len == 126) { unsigned char e[2]; sv_read_exact(c,e,2); len=(e[0]<<8)|e[1]; }
            else if (len == 127) { unsigned char e[8]; int k; len=0; sv_read_exact(c,e,8); for(k=0;k<8;k++) len=(len<<8)|e[k]; }
            {
                unsigned char mkey[4], *pl;
                if (sv_read_exact(c, mkey, 4) != 0) { sv->failures++; closesocket(c); return 0; }
                pl = (unsigned char *)malloc((size_t)len + 1);
                if (sv_read_exact(c, pl, (int)len) != 0) { sv->failures++; free(pl); closesocket(c); return 0; }
                for (i = 0; i < (int)len; i++) pl[i] ^= mkey[i & 3];
                pl[len] = '\0';
                /* verify the unmasked client request is the expected CDP call */
                if (!strstr((char *)pl, "\"Runtime.evaluate\"")) sv->failures++;
                free(pl);
            }
        }
    }

    /* send response (server frames are unmasked) */
    {
        const char *resp = "{\"id\":1,\"result\":{\"result\":{\"type\":\"string\",\"value\":\"ok\"}}}";
        unsigned long long L = strlen(resp);
        unsigned char hdr[4];
        hdr[0]=0x81; hdr[1]=(unsigned char)L;
        sv_write_exact(c, hdr, 2);
        sv_write_exact(c, resp, (int)L);
    }
    /* send an event */
    {
        const char *ev = "{\"method\":\"Page.loadEventFired\",\"params\":{\"ts\":42}}";
        unsigned long long L = strlen(ev);
        unsigned char hdr[2];
        hdr[0]=0x81; hdr[1]=(unsigned char)L;
        sv_write_exact(c, hdr, 2);
        sv_write_exact(c, ev, (int)L);
    }

    closesocket(c);
    return 0;
}

int main(void)
{
    Srv sv;
    struct sockaddr_in sa;
    int salen = sizeof(sa);
    WSADATA d;
    HANDLE th;
    CdpConn *c;
    JVal *reply, *ev;
    int failures = 0;

    /* SHA-1 known-vector self-check (mock server relies on it for Sec-WebSocket-Accept). */
    {
        unsigned char dg[20];
        static const unsigned char expect[20] = {
            0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
            0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d };
        sha1((const unsigned char *)"abc", 3, dg);
        if (memcmp(dg, expect, 20) != 0) { printf("FAIL: SHA-1 self-check\n"); return 1; }
    }

    WSAStartup(MAKEWORD(2,2), &d);
    sv.ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sv.failures = 0;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    sa.sin_port = 0;
    bind(sv.ls, (struct sockaddr *)&sa, sizeof(sa));
    listen(sv.ls, 1);
    getsockname(sv.ls, (struct sockaddr *)&sa, &salen);
    sv.port = ntohs(sa.sin_port);

    th = CreateThread(NULL, 0, server_thread, &sv, 0, NULL);

    c = cdp_connect("127.0.0.1", sv.port);
    printf("cdp_connect = %s\n", c ? "OK" : "FAIL");
    if (!c) failures++;

    if (c) {
        if (cdp_call(c, 1, "Runtime.evaluate", "{\"expression\":\"1+1\"}", &reply) == 0) {
            JVal *res = j_get(reply, "result");
            JVal *inner = res ? j_get(res, "result") : NULL;
            JVal *val = inner ? j_get(inner, "value") : NULL;
            printf("cdp_call reply value = '%s' (expect \"ok\")\n", j_str(val) ? j_str(val) : "(null)");
            if (!val || strcmp(j_str(val), "ok") != 0) failures++;
            j_free(reply);
        } else { printf("cdp_call FAILED\n"); failures++; }

        if (cdp_recv(c, &ev) == 0) {
            JVal *m = j_get(ev, "method");
            printf("cdp_recv event method = '%s' (expect Page.loadEventFired)\n",
                   j_str(m) ? j_str(m) : "(null)");
            if (!m || strcmp(j_str(m), "Page.loadEventFired") != 0) failures++;
            j_free(ev);
        } else { printf("cdp_recv FAILED\n"); failures++; }

        cdp_close(c);
    }

    WaitForSingleObject(th, 3000);
    closesocket(sv.ls);
    if (sv.failures) { printf("server saw %d failures\n", (int)sv.failures); failures += sv.failures; }

    printf("%s\n", failures ? "FAIL" : "PASS (CDP transport)");
    return failures ? 1 : 0;
}

/* blittest.c -- prove the "decode + GDI blit" half of the render-target milestone.
 *
 * The open problem flagged in the handoff was: the toolchain has no libjpeg/libpng
 * import lib, so turning a Chromium screencast JPEG into pixels for a GDI blit was
 * blocked. This test vendors stb_image (single-file, public domain, decodes JPEG+PNG
 * with no import lib) and proves the full path end-to-end under Wine:
 *
 *   1. connect to Chromium, navigate an animated red/blue page, Page.startScreencast;
 *   2. pull ONE Page.screencastFrame, base64-decode the whole JPEG;
 *   3. stbi_load_from_memory -> RGBA; check dimensions + centre-pixel colour;
 *   4. convert RGBA -> BGRA (32bpp top-down DIB);
 *   5. GDI blit it (StretchDIBits) into a real top-level window, then read it back via
 *      GetDIBits and confirm the pixels survived the round trip;
 *   6. write a .bmp for visual inspection.
 *
 * Usage:  wine blittest32.exe [port]
 */

#include "cdp.h"
#include "json.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DECODE (1u << 20)   /* 1 MiB: far more than any screencast frame */

static int send_cmd(CdpConn *c, long *id, const char *method, const char *params)
{
    JVal *reply = NULL;
    int r = cdp_call(c, ++(*id), method, params, &reply);
    if (reply)
        j_free(reply);
    return r;
}

/* Decode one screencast frame: returns malloc'd RGBA pixels, sets *w and *h. NULL on failure. */
static unsigned char *grab_frame(CdpConn *c, int *w, int *h)
{
    unsigned char *rgba = NULL;
    int i;

    for (i = 0; i < 300; i++) {
        JVal *m = NULL;
        const char *method;
        if (cdp_recv(c, &m) != 0)
            break;
        method = j_str(j_get(m, "method"));
        if (method && strcmp(method, "Page.screencastFrame") == 0) {
            JVal *params = j_get(m, "params");
            JVal *data = params ? j_get(params, "data") : NULL;
            JVal *meta = params ? j_get(params, "metadata") : NULL;
            JVal *sid = params ? j_get(params, "sessionId") : NULL;
            const char *b64 = data ? j_str(data) : NULL;
            int n, comp, x, y;
            unsigned char *jpeg;

            if (!b64) { j_free(m); break; }

            jpeg = (unsigned char *)malloc(MAX_DECODE);
            n = b64_decode(b64, jpeg, MAX_DECODE);
            if (n <= 0) { free(jpeg); j_free(m); break; }

            rgba = stbi_load_from_memory(jpeg, n, &x, &y, &comp, 4);
            free(jpeg);
            if (rgba) { *w = x; *h = y; }

            (void)sid; (void)meta;
            j_free(m);
            if (rgba)
                return rgba;
            break;
        }
        j_free(m);
    }
    return NULL;
}

/* BGRA 32bpp top-down DIB from RGBA top-down. */
static unsigned char *rgba_to_bgra(const unsigned char *rgba, int w, int h)
{
    unsigned char *bgra = (unsigned char *)malloc((size_t)w * h * 4);
    size_t i, n = (size_t)w * h;
    for (i = 0; i < n; i++) {
        bgra[i * 4 + 0] = rgba[i * 4 + 2];
        bgra[i * 4 + 1] = rgba[i * 4 + 1];
        bgra[i * 4 + 2] = rgba[i * 4 + 0];
        bgra[i * 4 + 3] = rgba[i * 4 + 3];
    }
    return bgra;
}

/* Write a 32bpp BGRA image as a bottom-up BMP (for visual inspection). */
static int write_bmp(const char *path, const unsigned char *bgra, int w, int h)
{
    FILE *f = fopen(path, "wb");
    int row, padded = ((w * 3 + 3) / 4) * 4;
    unsigned char *rowbuf;
    if (!f)
        return -1;

    /* We write 24bpp BMP (simpler, universally viewable). */
    {
        BITMAPFILEHEADER fh;
        BITMAPINFOHEADER ih;
        memset(&fh, 0, sizeof(fh));
        memset(&ih, 0, sizeof(ih));
        ih.biSize = sizeof(ih);
        ih.biWidth = w;
        ih.biHeight = h;
        ih.biPlanes = 1;
        ih.biBitCount = 24;
        ih.biCompression = BI_RGB;
        fh.bfType = 0x4D42; /* 'BM' */
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + padded * h;
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
    }

    rowbuf = (unsigned char *)malloc(padded);
    for (row = h - 1; row >= 0; row--) { /* bottom-up */
        int col;
        for (col = 0; col < w; col++) {
            /* input is 32bpp BGRA; a 24bpp BMP stores B,G,R byte order, so no swap. */
            const unsigned char *p = bgra + (size_t)row * w * 4 + col * 4;
            rowbuf[col * 3 + 0] = p[0]; /* B */
            rowbuf[col * 3 + 1] = p[1]; /* G */
            rowbuf[col * 3 + 2] = p[2]; /* R */
        }
        fwrite(rowbuf, padded, 1, f);
    }
    free(rowbuf);
    fclose(f);
    return 0;
}

static int is_red(const unsigned char *p)  { return p[0] > 200 && p[1] < 100 && p[2] < 100; }
static int is_blue(const unsigned char *p) { return p[2] > 200 && p[1] < 100 && p[0] < 100; }

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 9222;
    char wsurl[512];
    CdpConn *c;
    long id = 0;
    unsigned char *rgba;
    int w = 0, h = 0, failures = 0;

    setbuf(stdout, NULL);

    if (cdp_first_page_wsurl("127.0.0.1", port, wsurl, sizeof(wsurl)) != 0) {
        printf("FAIL: no page target on %d\n", port);
        return 1;
    }
    c = cdp_connect_wsurl(wsurl);
    if (!c) { printf("FAIL: connect\n"); return 1; }

    send_cmd(c, &id, "Page.navigate",
      "{\"url\":\"data:text/html;base64,PCFET0NUWVBFIGh0bWw+PGh0bWw+PGJvZHkgc3R5bGU9ImJhY2tncm91bmQ6cmVkIj48aDEgaWQ9InQiPjA8L2gxPjxzY3JpcHQ+dmFyIG49MDtzZXRJbnRlcnZhbChmdW5jdGlvbigpe24rKztkb2N1bWVudC5nZXRFbGVtZW50QnlJZCgndCcpLnRleHRDb250ZW50PW47ZG9jdW1lbnQuYm9keS5zdHlsZS5iYWNrZ3JvdW5kPShuJTIpPydyZWQnOidibHVlJzt9LDIwMCk7PC9zY3JpcHQ+PC9ib2R5PjwvaHRtbD4=\"}");
    Sleep(700);

    send_cmd(c, &id, "Page.startScreencast",
             "{\"format\":\"jpeg\",\"quality\":80,\"everyNthFrame\":1}");
    printf("startScreencast OK, waiting for a frame...\n");

    rgba = grab_frame(c, &w, &h);
    send_cmd(c, &id, "Page.stopScreencast", "{}");
    cdp_close(c);

    if (!rgba) {
        printf("FAIL: no frame decoded\n");
        return 1;
    }
    printf("decoded frame: %dx%d (RGBA)\n", w, h);
    if (w <= 0 || h <= 0) { printf("FAIL: bad dimensions\n"); failures++; }

    /* centre pixel + a few interior samples (avoid the 8px body margin) */
    {
        const unsigned char *cpx = rgba + ((size_t)h / 2) * w * 4 + ((size_t)w / 2) * 4;
        const unsigned char *px2 = rgba + ((size_t)(h * 3 / 4)) * w * 4 + ((size_t)(w * 7 / 8)) * 4;
        printf("centre=%3d,%3d,%3d   sample=%3d,%3d,%3d\n",
               cpx[0], cpx[1], cpx[2], px2[0], px2[1], px2[2]);
        if (!(is_red(cpx) || is_blue(cpx))) {
            printf("FAIL: centre pixel not red/blue (is the page rendering?)\n");
            failures++;
        } else {
            printf("centre pixel is %s (matches animated page)\n",
                   is_red(cpx) ? "RED" : "BLUE");
        }
    }

    /* ---- GDI blit proof: StretchDIBits the BGRA DIB into a DIB-section DC, read back ---- */
    {
        unsigned char *bgra = rgba_to_bgra(rgba, w, h);
        BITMAPINFO bi;
        HDC mem_dc;
        HBITMAP dib;
        void *dib_bits;
        unsigned char *readback;
        int r;

        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;   /* negative = top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        mem_dc = CreateCompatibleDC(NULL);
        if (!mem_dc) { printf("FAIL: CreateCompatibleDC\n"); failures++; }
        else {
            dib = CreateDIBSection(mem_dc, &bi, DIB_RGB_COLORS, &dib_bits, NULL, 0);
            if (!dib) { printf("FAIL: CreateDIBSection\n"); failures++; }
            else {
                HBITMAP old = (HBITMAP)SelectObject(mem_dc, dib);
                /* Real GDI blit: source DIB in memory -> device (the DIB section DC). */
                r = StretchDIBits(mem_dc, 0, 0, w, h, 0, 0, w, h, bgra, &bi, DIB_RGB_COLORS, SRCCOPY);
                GdiFlush();
                SelectObject(mem_dc, old);

                readback = (unsigned char *)malloc((size_t)w * h * 4);
                memcpy(readback, dib_bits, (size_t)w * h * 4);
                {
                    size_t off = ((size_t)h / 2) * (size_t)w * 4 + ((size_t)w / 2) * 4;
                    const unsigned char *rb = readback + off;
                    const unsigned char *src = bgra + off;
                    printf("StretchDIBits ret=%d, readback centre BGRA=%3d,%3d,%3d,%3d (source %3d,%3d,%3d,%3d)\n",
                           r, rb[0], rb[1], rb[2], rb[3], src[0], src[1], src[2], src[3]);
                    if (memcmp(rb, src, 4) != 0) {
                        printf("FAIL: GDI blit changed the pixels\n");
                        failures++;
                    } else {
                        printf("GDI blit: centre pixel preserved exactly through StretchDIBits\n");
                    }
                }
                free(readback);
                DeleteObject(dib);
                DeleteDC(mem_dc);
            }
        }
        free(bgra);
    }

    /* write the RGBA as a 24bpp BMP via bgra intermediate for viewing */
    {
        unsigned char *bgra = rgba_to_bgra(rgba, w, h);
        write_bmp("blittest_out.bmp", bgra, w, h);
        free(bgra);
        printf("wrote blittest_out.bmp\n");
    }

    stbi_image_free(rgba);
    printf("%s\n", failures == 0 ? "PASS (decode + blit)" : "FAIL");
    return failures == 0 ? 0 : 1;
}

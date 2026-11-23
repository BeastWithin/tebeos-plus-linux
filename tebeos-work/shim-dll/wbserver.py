#!/usr/bin/env python3
"""wbserver.py -- the pages wbtest.exe navigates the WebBrowser control through.

    python3 wbserver.py PORT

/index     windows-1254 page (Turkish text, a link, a form, a JS function)
/second    plain page
/post      echoes the method, Content-Type, body, Referer, Origin and Sec-Fetch-* of the request
/redirect  302 to /second
/cancelme  must never load (wbtest cancels it in BeforeNavigate2)
/prompt    a form whose #t4 onfocus writes "___" and onblur strips it (Medula's T.C. field)
/captcha   a page with an <img> from /SayiUretenImageYeniServlet, as on the Medula login
/SayiUretenImageYeniServlet  80x30 PNG: left half 1px red/green columns, right half blue
/hits      how many times each path was requested (text/plain)
"""
import struct
import sys
import zlib
from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

hits = Counter()

INDEX = ("<!DOCTYPE html><html><head><title>Index</title></head><body>"
         "<p id=t>Sistem hatası: çğış</p>"
         "<a id=l href=\"/second\">second</a> <a id=c href=\"/cancelme\">cancel</a>"
         "<form id=f method=post action=\"/post\"><input name=q value=\"çğış\"></form>"
         "<script>function add(a,b){return a+b}</script>"
         "</body></html>")


PROMPT = ("<!DOCTYPE html><html><head><title>Prompt</title></head><body>"
          "<form id=pf method=post action=\"/post\">"
          "<input id=t4 name=t4 value=\"\" onfocus=\"if(this.value==='')this.value='___'\" "
          "onblur=\"this.value=this.value.replace(/_/g,'')\">"
          "<input id=t2 name=t2 value=\"\"><input type=submit id=ps name=ps value=Sorgula>"
          "</form></body></html>")


def png(w, h, pixel):
    def chunk(kind, data):
        c = kind + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    raw = b"".join(b"\0" + b"".join(bytes(pixel(x, y)) for x in range(w)) for y in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


# left half 1px red/green columns, right half blue: any resampling turns the columns into mud
CAPTCHA = png(80, 30, lambda x, y: ((255, 0, 0) if x % 2 == 0 else (0, 255, 0)) if x < 40 else (0, 0, 255))


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body, ctype="text/html; charset=utf-8", extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def page(self, title, body):
        return ("<!DOCTYPE html><html><head><title>%s</title></head><body>%s</body></html>"
                % (title, body)).encode()

    def route(self):
        path = self.path.split("?")[0]
        hits[path] += 1
        if path == "/index":
            self.send(200, INDEX.encode("cp1254"), "text/html; charset=windows-1254")
        elif path == "/second":
            self.send(200, self.page("Second", "<p id=s>second page</p>"))
        elif path == "/cancelme":
            self.send(200, self.page("Cancelled", "<p>should not load</p>"))
        elif path == "/redirect":
            self.send(302, b"", extra={"Location": "/second"})
        elif path == "/post":
            n = int(self.headers.get("Content-Length") or 0)
            data = self.rfile.read(n).decode("latin-1") if n else ""
            ctype = self.headers.get("Content-Type", "")
            self.send(200, self.page("Post", "<pre id=m>%s</pre><pre id=ct>%s</pre><pre id=b>%s</pre>"
                                     "<pre id=r>%s</pre><pre id=o>%s</pre><pre id=sf>%s</pre>"
                                     % (self.command, ctype, data, self.headers.get("Referer", ""),
                                        self.headers.get("Origin", ""),
                                        " ".join("%s=%s" % (k, v) for k, v in self.headers.items()
                                                 if k.lower().startswith("sec-fetch")))))
        elif path == "/prompt":
            self.send(200, PROMPT.encode())
        elif path == "/captcha":
            self.send(200, self.page("Captcha", "<p>kod</p><p style=\"margin-left:12.5px\">"
                                             "<img id=k src=\"/SayiUretenImageYeniServlet?x=1\"></p>"))
        elif path == "/SayiUretenImageYeniServlet":
            self.send(200, CAPTCHA, "image/png")
        elif path == "/hits":
            self.send(200, "".join("%s %d\n" % kv for kv in sorted(hits.items())).encode(),
                      "text/plain")
        else:
            self.send(404, self.page("404", "not found"))

    do_GET = route
    do_POST = route


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()

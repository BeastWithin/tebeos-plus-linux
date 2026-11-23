"""netwatch.py DIR SECONDS -- print the SGK tab's document requests (method, url, post field
names with value lengths; submit-button values shown) and responses, for SECONDS."""
import json, sys, time, urllib.parse, urllib.request
sys.path.insert(0, sys.argv[1])
from cdpprobe_ws import WS
tl = json.load(urllib.request.urlopen("http://127.0.0.1:9333/json/list"))
import os
pg = [t for t in tl if t["id"] == os.environ["APP"]][0]
w = WS(pg["webSocketDebuggerUrl"])
w.send("Network.enable", {"maxPostDataSize": 1000000})
end = time.time() + float(sys.argv[2])
docs = set()
def fields(body):
    out = []
    for k, v in urllib.parse.parse_qsl(body or "", keep_blank_values=True, encoding="latin-1"):
        out.append("%s=%s" % (k, v if ("button" in k.lower() or k.endswith("_SUBMIT")) else "<%d>" % len(v)))
    return " ".join(out)
while time.time() < end:
    m = w.recv(1)
    if not m or "method" not in m:
        continue
    p = m["params"]
    if m["method"] == "Network.requestWillBeSent" and p.get("type") == "Document":
        docs.add(p["requestId"]); r = p["request"]
        if p.get("redirectResponse"):
            print("  redirect", p["redirectResponse"]["status"], "->", r["url"])
        print("REQ", r["method"], r["url"], "hasPost=%s" % r.get("hasPostData"), fields(r.get("postData")))
    elif m["method"] == "Network.requestWillBeSentExtraInfo" and p["requestId"] in docs:
        h = p.get("headers", {})
        print("  SENT", {k: (v if k.lower() != "cookie" else "<%d>" % len(v)) for k, v in h.items()
                         if k.lower() in ("origin", "referer", "content-type", "cookie", "content-length") or k.lower().startswith("sec-fetch") or k.startswith(":")})
    elif m["method"] == "Network.responseReceived" and p["requestId"] in docs:
        print("RESP", p["response"]["status"], p["response"]["url"])
    elif m["method"] == "Network.loadingFailed" and p["requestId"] in docs:
        print("FAIL", p.get("errorText"), "canceled=%s" % p.get("canceled"))

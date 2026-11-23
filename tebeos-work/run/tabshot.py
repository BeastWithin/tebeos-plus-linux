"""tabshot.py DIR OUT.png -- Chromium's own screenshot of the shim's SGK tab on 9333."""
import base64, json, sys, urllib.request, time
sys.path.insert(0, sys.argv[1])
from cdpprobe_ws import WS
tl = json.load(urllib.request.urlopen("http://127.0.0.1:9333/json/list"))
pg = [t for t in tl if t["type"] == "page" and "sgk" in t["url"]][0]
w = WS(pg["webSocketDebuggerUrl"])
i = w.send("Page.captureScreenshot", {"format": "png"})
t = time.time()
while time.time() - t < 10:
    m = w.recv(1)
    if m and m.get("id") == i:
        open(sys.argv[2], "wb").write(base64.b64decode(m["result"]["data"])); print("saved"); break
else:
    print("timed out")

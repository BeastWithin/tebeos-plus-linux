import json, sys, urllib.request, time
sys.path.insert(0, sys.argv[1])
from cdpprobe_ws import WS
tl = json.load(urllib.request.urlopen("http://127.0.0.1:9333/json/list"))
pg = [t for t in tl if t["type"] == "page" and "sgk" in t["url"]][0]
print("tab", pg["url"], pg["title"])
w = WS(pg["webSocketDebuggerUrl"])
i = w.send("Runtime.evaluate", {"expression": "document.readyState + ' ' + location.href + ' forms=' + document.forms.length", "returnByValue": True})
t = time.time()
while time.time() - t < 5:
    m = w.recv(1)
    if m and m.get("id") == i:
        print("eval", m.get("result", {}).get("result", {}).get("value"), m.get("error")); break
else:
    print("eval timed out (dialog open or renderer busy?)")

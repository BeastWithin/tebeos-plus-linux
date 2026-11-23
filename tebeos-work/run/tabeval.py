"""tabeval.py DIR EXPR -- evaluate EXPR (read-only, please) in the shim's SGK tab on 9333."""
import json, sys, urllib.request, time
sys.path.insert(0, sys.argv[1])
from cdpprobe_ws import WS
tl = json.load(urllib.request.urlopen("http://127.0.0.1:9333/json/list"))
import os
pg = [t for t in tl if t["id"] == os.environ["APP"]][0] if os.environ.get("APP") else [t for t in tl if t["type"] == "page" and "sgk" in t["url"]][0]
print("tab", pg["url"])
w = WS(pg["webSocketDebuggerUrl"])
i = w.send("Runtime.evaluate", {"expression": sys.argv[2], "returnByValue": True})
t = time.time()
while time.time() - t < 5:
    m = w.recv(1)
    if m and m.get("id") == i:
        r = m.get("result", {})
        print(json.dumps(r.get("result", {}).get("value"), ensure_ascii=False, indent=1) if "exceptionDetails" not in r else r["exceptionDetails"])
        break
else:
    print("eval timed out")

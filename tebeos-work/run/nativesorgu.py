"""nativesorgu.py DIR CONTEXT [reissue] -- in a second tab of the shim's browser context (same SGK
session, no Fetch interception), open ReceteSorgu.jsp, query the prescription number the app's
tab shows, and print the request headers and an outline of the result. Closes the tab."""
import json, sys, time, urllib.request
sys.path.insert(0, sys.argv[1])
from cdpprobe_ws import WS
base = "http://127.0.0.1:9333"
import os
app = [t for t in json.load(urllib.request.urlopen(base + "/json/list")) if t["id"] == os.environ["APP"]][0]
a = WS(app["webSocketDebuggerUrl"])
num = a.call("Runtime.evaluate", {"expression": "document.getElementById('form1:text2').value", "returnByValue": True})["result"]["result"]["value"]
b = WS(json.load(urllib.request.urlopen(base + "/json/version"))["webSocketDebuggerUrl"])
tid = b.call("Target.createTarget", {"url": "about:blank", "browserContextId": sys.argv[2], "background": True})["result"]["targetId"]
try:
    time.sleep(1)
    t = [x for x in json.load(urllib.request.urlopen(base + "/json/list")) if x["id"] == tid][0]
    w = WS(t["webSocketDebuggerUrl"])
    w.call("Page.enable"); w.call("Network.enable")
    REISSUE = len(sys.argv) > 3
    import base64
    state = {}
    if REISSUE:
        w.call("Fetch.enable", {"patterns": [{"urlPattern": "*", "resourceType": "Document", "requestStage": "Request"}]})
    def go(expr_or_url, nav):
        ev = []
        if nav: w.send("Page.navigate", {"url": expr_or_url})
        else: w.send("Runtime.evaluate", {"expression": expr_or_url})
        end = time.time() + 20
        while time.time() < end and not any(m.get("method") == "Page.loadEventFired" for m in ev):
            m = w.recv(1)
            if m: ev.append(m)
            if m and m.get("method") == "Fetch.requestPaused":
                rq = m["params"]["request"]; rid = m["params"]["requestId"]
                if rq["method"] == "POST" and "orig" not in state:
                    state["orig"] = rq
                    w.send("Fetch.failRequest", {"requestId": rid, "errorReason": "Aborted"})
                    w.send("Page.navigate", {"url": rq["url"], "referrer": rq["headers"].get("Referer", "")})
                elif "orig" in state and "done" not in state:
                    state["done"] = 1; o = state["orig"]
                    body = b"".join(base64.b64decode(e["bytes"]) for e in o.get("postDataEntries", []))
                    keep = {"Content-Type": o["headers"]["Content-Type"], "Origin": o["headers"]["Origin"]}
                    h = [{"name": k, "value": v} for k, v in rq["headers"].items() if k not in keep] + [{"name": k, "value": v} for k, v in keep.items()]
                    w.send("Fetch.continueRequest", {"requestId": rid, "method": "POST", "postData": base64.b64encode(body).decode(), "headers": h})
                    print("reissued with Origin", keep["Origin"])
                else:
                    w.send("Fetch.continueRequest", {"requestId": rid})
        for m in ev:
            if m.get("method") == "Network.requestWillBeSent" and m["params"].get("type") == "Document":
                r = m["params"]["request"]; h = r["headers"]
                print("REQ", r["method"], r["url"], {k: v for k, v in h.items() if k.lower() in ("origin", "referer", "content-type") or k.lower().startswith("sec-fetch")})
            if m.get("method") == "Network.responseReceived" and m["params"].get("type") == "Document":
                print("RESP", m["params"]["response"]["status"], m["params"]["response"]["url"])
    go("https://medeczane.sgk.gov.tr/eczane/faces/pages/recete/ReceteSorgu.jsp", True)
    w.call("Runtime.evaluate", {"expression": "document.getElementById('form1:text2').value=%s" % json.dumps(num)})
    go("document.getElementById('form1:buttonReceteNoSorgula').click()", False)
    time.sleep(1)
    out = w.call("Runtime.evaluate", {"expression": "location.href+' | action='+(document.forms[0]&&document.forms[0].getAttribute('action'))+' | '+document.body.innerText.split('\\n').map(function(l){return l.trim().slice(0,40)}).filter(function(l){return l}).slice(25,45).join(' / ')", "returnByValue": True})
    print("RESULT", out["result"]["result"]["value"])
finally:
    b.call("Target.closeTarget", {"targetId": tid})

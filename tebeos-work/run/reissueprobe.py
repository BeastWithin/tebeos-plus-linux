#!/usr/bin/env python3
"""reissueprobe.py -- what headers reach the server for a form POST that is (a) left alone,
(b) aborted in Fetch and re-issued with Page.navigate + continueRequest overrides, as the shim
does, with different header overrides."""
import base64, json, os, subprocess, sys, tempfile, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cdpprobe_ws import WS
CP, SP = 9447, 9448
srv = subprocess.Popen([sys.executable, os.path.expanduser("~/Projects/TebeosOnLinux/tebeos-work/shim-dll/wbserver.py"), str(SP)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
prof = tempfile.mkdtemp(prefix="reissueprobe-")
chrome = subprocess.Popen(["chromium", "--headless=new", "--remote-debugging-port=%d" % CP, "--user-data-dir=" + prof,
                           "--no-first-run", "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(50):
        try: tabs = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json" % CP)); break
        except Exception: time.sleep(0.2)
    url = [t for t in tabs if t["type"] == "page"][0]["webSocketDebuggerUrl"]
    dom, evt = WS(url), WS(url)
    evt.call("Page.enable")
    evt.call("Fetch.enable", {"patterns": [{"urlPattern": "*", "resourceType": "Document", "requestStage": "Request"}]})
    def pump(until, t=10, on_pause=None):
        end = time.time() + t
        while time.time() < end:
            m = evt.recv(1)
            if not m: continue
            if m.get("method") == "Fetch.requestPaused":
                if on_pause and on_pause(m): continue
                evt.send("Fetch.continueRequest", {"requestId": m["params"]["requestId"]})
            if m.get("method") == until: return m
    def load(u):
        evt.send("Page.navigate", {"url": u}); pump("Page.loadEventFired")
    def server_saw():
        return dom.call("Runtime.evaluate", {"expression": "['m','o','r','sf'].map(function(i){var e=document.getElementById(i);return i+'='+(e?e.innerText:'?')}).join(' | ')", "returnByValue": True})["result"]["result"]["value"]
    idx = "http://127.0.0.1:%d/index" % SP
    load(idx); dom.call("Runtime.evaluate", {"expression": "document.getElementById('f').submit()"}); pump("Page.loadEventFired")
    print("native     ", server_saw())
    for name, extra in [("reissue", []), ("+origin", ["Origin"]), ("+origin+secfetch", ["Origin", "Sec-Fetch-Site", "Sec-Fetch-Mode", "Sec-Fetch-Dest", "Sec-Fetch-User"])]:
        load(idx)
        held = {}
        def grab(m):
            if held: return False
            held["req"] = m["params"]["request"]
            evt.send("Fetch.failRequest", {"requestId": m["params"]["requestId"], "errorReason": "Aborted"})
            return True
        dom.call("Runtime.evaluate", {"expression": "document.getElementById('f').submit()"})
        pump("Fetch.requestPaused", 5, grab)
        time.sleep(0.3)
        req = held["req"]
        body = b"".join(base64.b64decode(e["bytes"]) for e in req.get("postDataEntries", []))
        hdr = {"Content-Type": req["headers"].get("Content-Type", "")}
        orig = dict(req["headers"])
        orig.setdefault("Sec-Fetch-Site", "same-origin"); orig.setdefault("Sec-Fetch-Mode", "navigate")
        orig.setdefault("Sec-Fetch-Dest", "document"); orig.setdefault("Sec-Fetch-User", "?1")
        for k in extra:
            if k in orig: hdr[k] = orig[k]
        def cont(m):
            h = [{"name": k, "value": v} for k, v in m["params"]["request"]["headers"].items() if k not in hdr]
            h += [{"name": k, "value": v} for k, v in hdr.items()]
            evt.send("Fetch.continueRequest", {"requestId": m["params"]["requestId"], "method": "POST",
                                               "postData": base64.b64encode(body).decode(), "headers": h})
            return True
        evt.send("Page.navigate", {"url": req["url"], "referrer": req["headers"].get("Referer", "")})
        pump("Page.loadEventFired", 10, cont)
        print("%-11s" % name, "orig-hdrs=%s" % sorted(k for k in req["headers"]), "|", server_saw())
finally:
    chrome.kill(); srv.kill()

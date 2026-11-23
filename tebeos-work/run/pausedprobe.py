#!/usr/bin/env python3
"""pausedprobe.py -- does Runtime.evaluate answer on one session while another session holds
the form-submit navigation paused in Fetch (as page.c does during BeforeNavigate2)?"""
import json, os, subprocess, sys, tempfile, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cdpprobe_ws import WS

CP, SP = 9445, 9446
srv = subprocess.Popen([sys.executable, os.path.expanduser(
    "~/Projects/TebeosOnLinux/tebeos-work/shim-dll/wbserver.py"), str(SP)],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
prof = tempfile.mkdtemp(prefix="pausedprobe-")
chrome = subprocess.Popen(["chromium", "--headless=new", "--remote-debugging-port=%d" % CP,
                           "--user-data-dir=" + prof, "--no-first-run"]
                          + os.environ.get("FLAGS", "").split() + ["about:blank"],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(50):
        try:
            tabs = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json" % CP)); break
        except Exception:
            time.sleep(0.2)
    url = [t for t in tabs if t["type"] == "page"][0]["webSocketDebuggerUrl"]
    dom, evt = WS(url), WS(url)
    dom.call("Network.enable")
    evt.call("Page.enable"); evt.call("Runtime.enable")
    evt.call("Fetch.enable", {"patterns": [{"urlPattern": "*", "resourceType": "Document", "requestStage": "Request"}]})
    dom.send("Page.navigate", {"url": "http://127.0.0.1:%d/index" % SP})
    def wait_evt(pred, t=10):
        end = time.time() + t
        while time.time() < end:
            m = evt.recv(1)
            if not m: continue
            if m.get("method") == "Fetch.requestPaused" and not pred(m):
                evt.send("Fetch.continueRequest", {"requestId": m["params"]["requestId"]})
                continue
            if pred(m): return m
        return None
    wait_evt(lambda m: m.get("method") == "Page.loadEventFired")
    print("loaded:", dom.call("Runtime.evaluate", {"expression": "document.title", "returnByValue": True})["result"]["result"]["value"])
    for how in sys.argv[1:] or ["form.submit()"]:
        expr = {"form.submit()": "document.getElementById('f').submit()",
                "click": "var b=document.createElement('input');b.type='submit';document.getElementById('f').appendChild(b);b.click()",
                "link": "document.getElementById('l').click()"}[how]
        t0 = time.time()
        r = dom.call("Runtime.evaluate", {"expression": expr, "returnByValue": True, "userGesture": True})
        print(how, "evaluate returned in %.0f ms" % ((time.time() - t0) * 1000), "ok" if r else "TIMEOUT")
        p = wait_evt(lambda m: m.get("method") == "Fetch.requestPaused")
        print("paused:", p and p["params"]["request"]["method"], p and p["params"]["request"]["url"])
        t0 = time.time()
        r = dom.call("Runtime.evaluate", {"expression": "document.title", "returnByValue": True})
        print("evaluate while paused: %.0f ms" % ((time.time() - t0) * 1000), r and r.get("result", {}).get("result", {}).get("value"))
        evt.send("Fetch.continueRequest", {"requestId": p["params"]["requestId"]})
        wait_evt(lambda m: m.get("method") == "Page.loadEventFired")
        print("after:", dom.call("Runtime.evaluate", {"expression": "document.title", "returnByValue": True})["result"]["result"]["value"])
        dom.send("Page.navigate", {"url": "http://127.0.0.1:%d/index" % SP})
        wait_evt(lambda m: m.get("method") == "Page.loadEventFired")
finally:
    chrome.kill(); srv.kill()

"""freshpage.py DIR CONTEXT -- GET ReceteSorgu.jsp in a second tab of the shim's context (no
query), then probe how hxclient's prompt-char behaviour treats form1:text4; closes the tab."""
import json, sys, time, urllib.request
sys.path.insert(0, sys.argv[1])
from cdpprobe_ws import WS
base = "http://127.0.0.1:9333"
b = WS(json.load(urllib.request.urlopen(base + "/json/version"))["webSocketDebuggerUrl"])
tid = b.call("Target.createTarget", {"url": "about:blank", "browserContextId": sys.argv[2], "background": True})["result"]["targetId"]
def ev(w, e):
    r = w.call("Runtime.evaluate", {"expression": e, "returnByValue": True, "userGesture": True})["result"]
    return r.get("result", {}).get("value", r.get("exceptionDetails", {}).get("text"))
try:
    time.sleep(1)
    t = [x for x in json.load(urllib.request.urlopen(base + "/json/list")) if x["id"] == tid][0]
    w = WS(t["webSocketDebuggerUrl"]); w.call("Page.enable")
    w.send("Page.navigate", {"url": "https://medeczane.sgk.gov.tr/eczane/faces/pages/recete/ReceteSorgu.jsp"})
    end = time.time() + 20
    while time.time() < end:
        m = w.recv(1)
        if m and m.get("method") == "Page.loadEventFired": break
    time.sleep(1)
    V = "(function(){var t=document.getElementById('form1:text4');return JSON.stringify(t.value)+' active='+(document.activeElement&&document.activeElement.id)})()"
    print("load      ", ev(w, V))
    print("form      ", ev(w, "(function(){var f=document.getElementById('form1');return 'onsubmit='+f.getAttribute('onsubmit')+' action='+f.getAttribute('action')})()"))
    print("btns      ", ev(w, "Array.from(document.querySelectorAll('input[type=submit],input[type=button]')).map(function(b){return b.id+'='+b.value+' onclick='+b.getAttribute('onclick')}).join(' | ')"))
    ev(w, "document.getElementById('form1:text4').focus()"); print("focus()   ", ev(w, V))
    ev(w, "document.getElementById('form1:text4').blur()"); print("blur()    ", ev(w, V))
    ev(w, "document.getElementById('form1:text4').dispatchEvent(new FocusEvent('focus'))"); print("evt focus ", ev(w, V))
    ev(w, "document.getElementById('form1:text4').dispatchEvent(new FocusEvent('blur'))"); print("evt blur  ", ev(w, V))
    ev(w, "document.getElementById('form1:text4').value=''"); print("set ''    ", ev(w, V))
    ev(w, "document.getElementById('form1:text4').focus()"); print("focus('') ", ev(w, V))
    ev(w, "document.getElementById('form1:text4').blur()"); print("blur('')  ", ev(w, V))
    print("hasFocus  ", ev(w, "document.hasFocus()"))
finally:
    b.call("Target.closeTarget", {"targetId": tid})

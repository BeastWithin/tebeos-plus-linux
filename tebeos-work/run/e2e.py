"""e2e.py RUNDIR [--keep] -- one login and one "Medula'da Ac" query through the real app on :5
(SHIM_TRACE=1). Prints structure only -- request paths, POST field names with value lengths,
form actions -- never page text, so no patient data. Stops the app on a refused login or a
timeout; at the end it logs the SGK session out (XHR, which the app never sees) and stops the
app, unless --keep."""
import json, os, subprocess, sys, time, urllib.parse, urllib.request
RUN = sys.argv[1]
sys.path.insert(0, RUN)
from cdpprobe_ws import WS

LOG = os.path.expandvars(os.path.expanduser("~/.wine-tebeos/drive_c/users/$USER/AppData/Local/Temp/mshtml-cdp.log"))
OCR = os.path.join(RUN, "..", "ocr-service", "service.log")
LOGIN = "NAV POST https://medeczane.sgk.gov.tr/eczane/faces/login.jsp"
KEEP = "--keep" in sys.argv
w = None
docs = set()
sorgu_t4 = []


def text(p):
    try:
        with open(p, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def reads():
    """OCR results so far: one per login attempt (the daemon also logs its own start)."""
    return sum(1 for l in text(OCR).splitlines() if " -> " in l)


def say(*a):
    print("%5.1f" % (time.time() - T0), *a, flush=True)


def handle(m):
    """Document requests of the app tab: path, method, field names with value lengths."""
    if not m or "method" not in m:
        return
    p = m["params"]
    if m["method"] == "Network.requestWillBeSent" and p.get("type") == "Document":
        docs.add(p["requestId"])
        r = p["request"]
        out = []
        for k, v in urllib.parse.parse_qsl(r.get("postData") or "", keep_blank_values=True, encoding="latin-1"):
            show = "button" in k.lower() or k.endswith("_SUBMIT")
            out.append("%s=%s" % (k, v if show else "<%d>" % len(v)))
            if k == "form1:text4" and "ReceteSorgu" in r["url"]:
                sorgu_t4.append(v.count("_"))
        say("REQ", r["method"], urllib.parse.urlparse(r["url"]).path, " ".join(out))
    elif m["method"] == "Network.responseReceived" and p["requestId"] in docs:
        say("RESP", p["response"]["status"], urllib.parse.urlparse(p["response"]["url"]).path)


def ev(expr, timeout=5):
    i = w.send("Runtime.evaluate", {"expression": expr, "returnByValue": True, "awaitPromise": True})
    end = time.time() + timeout
    while time.time() < end:
        m = w.recv(max(0.1, end - time.time()))
        if m and m.get("id") == i:
            return m.get("result", {}).get("result", {}).get("value")
        handle(m)
    return None


def pump(seconds):
    end = time.time() + seconds
    while time.time() < end:
        handle(w.recv(max(0.1, end - time.time())))


def logout():
    if w:
        r = ev("fetch('/eczane/LogoutServlet',{credentials:'include',redirect:'manual'}).then(function(r){return r.type+' '+r.status},"
               "function(e){return String(e)})", 15)
        say("logout XHR ->", r)


def stop(why, rc=1):
    say("STOP:", why)
    if rc == 0 and KEEP:
        sys.exit(0)
    if rc == 0 or "refused" not in why:
        logout()
    subprocess.run(["bash", os.path.join(RUN, "stopmedula.sh")], stdin=subprocess.DEVNULL,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    s = text(LOG)
    say("trace: %d focus holds, %d focus replays, %d login POSTs" %
        (s.count("__shim_fq||"), s.count("UI focus replay"), s.count(LOGIN)))
    sys.exit(rc)


def connect():
    tabs = [l.split(" tab ")[1].split()[0] for l in text(LOG).splitlines() if " tab " in l and " for doc " in l]
    if not tabs:
        return None
    try:
        tl = json.load(urllib.request.urlopen("http://127.0.0.1:9333/json/list"))
    except OSError:
        return None
    t = [x for x in tl if x["id"] == tabs[-1]]
    if not t:
        return None
    c = WS(t[0]["webSocketDebuggerUrl"])
    c.send("Network.enable", {"maxPostDataSize": 1000000})
    return c


T0 = time.time()
ocr0 = reads()
if os.path.exists(LOG):
    os.remove(LOG)
subprocess.Popen(["bash", os.path.join(RUN, "runmedula.sh")], env=dict(os.environ, SHIM_TRACE="1"),
                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                 start_new_session=True)

# 1. one login, then the index page's injected prescription table
while True:
    time.sleep(0.5)
    s = text(LOG)
    i = s.find(LOGIN)
    if i >= 0 and "LogoutServlet" in s[i:]:
        stop("login refused: SGK redirected the login POST to LogoutServlet")
    if s.count(LOGIN) > 1 or reads() - ocr0 > 1:
        stop("login refused: a second login attempt")
    if time.time() - T0 > 180:
        stop("no prescription table within 180 s")
    if i < 0:
        continue
    if w is None:
        w = connect()
        if w is None:
            continue
    n = ev("document.querySelectorAll('#Medularecbtton').length", 3)
    if n:
        say("logged in; %d 'Medula'da Ac' buttons" % n)
        break
pump(3)   # let DocumentCompleted finish attaching its handlers

# 2. click the first "Medula'da Ac" the way a mouse does
r = ev("(function(){var b=document.querySelector('#Medularecbtton');b.scrollIntoView({block:'center'});"
       "var r=b.getBoundingClientRect();return JSON.stringify([r.left+r.width/2,r.top+r.height/2]);})()")
x, y = json.loads(r)
for t in ("mouseMoved", "mousePressed", "mouseReleased"):
    w.send("Input.dispatchMouseEvent", {"type": t, "x": x, "y": y, "button": "left", "clickCount": 1})
say("clicked Medula'da Ac at %.0f,%.0f" % (x, y))

# 3. the menu navigation, the app's Sorgula, and the answer
end = time.time() + 60
actions = ""
while time.time() < end:
    pump(1)
    if not sorgu_t4:
        continue
    actions = ev("location.pathname+' forms: '+Array.from(document.forms).map(function(f){"
                 "return f.getAttribute('action')}).join(',')", 3) or actions
    if "ReceteIslem2" in actions:
        break
say("page:", actions)
say("Sorgula POST text4 underscores:", sorgu_t4)
if "ReceteIslem2" in actions:
    stop("PASS: the query returned the prescription (ReceteIslem2.jsp)", 0)
stop("FAIL: no ReceteIslem2.jsp within 60 s of the click")

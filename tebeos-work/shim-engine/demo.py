"""
demo.py -- live proof of the §2c CDP<->DOM mapping against a real Chromium/CEF.

Not run against the app's own CEF (port 8080) — that instance is busy rendering
the TEBEOS dashboard; navigating it would disrupt the app. Point it at a throwaway
Chromium/Edge/CEF instead, e.g.:

    chromium --headless=new --remote-debugging-port=9222 --no-sandbox about:blank
    # or a normal Chromium window:
    chromium --remote-debugging-port=9222
    python3 demo.py --port 9222

What it proves (every operation is a future C++ mshtml.dll call):
    Navigate2  -> Page.navigate
    GetElementById -> Runtime.evaluate
    SetAttribute/GetAttribute (round-trips Turkish "çığ") -> Runtime.callFunctionOn
    InvokeMember("click") -> el.click()
    InnerText  -> Runtime.evaluate
    DrawToDC (captcha) -> Page.captureScreenshot
"""

import argparse
import time

from engine import connect


PAGE = (
    "<!doctype html><html><body>"
    "<input id='uid' placeholder='user'>"
    "<button id='go' onclick=\"document.getElementById('out').innerText ="
    " 'CLICKED:' + document.getElementById('uid').value;\">go</button>"
    "<div id='out'>(nothing yet)</div>"
    "</body></html>"
)


def wait_ready(eng, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if eng.ready_state() == "complete":
            return True
        time.sleep(0.1)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9222)
    ap.add_argument("--url", default="data:text/html;charset=utf-8," + PAGE)
    args = ap.parse_args()

    eng = connect(port=args.port)
    eng.navigate(args.url)
    assert wait_ready(eng), "page did not reach readyState=complete"

    print("title        :", eng.document_title())
    print("location     :", eng.location_href())
    print("readyState   :", eng.ready_state())

    inp = eng.get_element_by_id("uid")
    assert inp is not None, "element #uid not found"
    inp.set_attribute("value", "\u00E7\u0131\u011F")  # "çığ"
    got = inp.get_attribute("value")
    assert got == "\u00E7\u0131\u011F", "Turkish round-trip failed: %r" % got
    print("setAttribute('value','çığ') -> getAttribute =", repr(got))

    btn = eng.get_element_by_id("go")
    btn.click()

    out = eng.get_element_by_id("out")
    text = out.inner_text
    print("innerText(#out) after click =", repr(text))
    assert text == "CLICKED:\u00E7\u0131\u011F", "click handler did not run: %r" % text

    png = eng.screenshot_png()
    with open("/tmp/shim_demo.png", "wb") as f:
        f.write(png)
    print("screenshot    : /tmp/shim_demo.png (%d bytes)" % len(png))

    print("PASS: navigate -> getElementById -> set/getAttribute(çığ) -> click -> innerText -> screenshot")


if __name__ == "__main__":
    main()

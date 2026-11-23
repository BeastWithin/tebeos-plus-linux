"""
engine.py -- MedulaEngine: the "engine half" of the Path-A shim.

Mirrors, 1:1, the IE COM surface the vendor app drives (HANDOFF-PATH-A.md §2c),
implemented over CDP (cdp.py). This is the exact operation set the C++ mshtml.dll
will reimplement natively; kept here in Python as (a) the reference implementation
and (b) a live test harness against any Chromium/CEF debugging port.

Mapping recap (§2c):
    Navigate2(url)                    -> Page.navigate
    getElementById / getElementsByTag -> Runtime.evaluate
    getAttribute / setAttribute       -> Runtime.evaluate
    innerText / innerHtml / outerHtml -> Runtime.evaluate
    focus / click / InvokeMember      -> el.focus() / el.click()
    AttachEventHandler                -> Runtime.addBinding + el.addEventListener
    DrawToDC (captcha)                -> Page.captureScreenshot (clipped)
"""

import base64
import json

try:
    from . import cdp as _cdp
except ImportError:
    import cdp as _cdp


class ElementHandle:
    """A live reference to a DOM element inside the page (objectId)."""

    def __init__(self, engine, object_id, description=""):
        self._engine = engine
        self._object_id = object_id
        self._description = description

    def _fn(self, function_declaration, args=None, return_by_value=True):
        params = {
            "objectId": self._object_id,
            "functionDeclaration": function_declaration,
            "returnByValue": return_by_value,
        }
        if args:
            params["arguments"] = args
        res = self._engine.call("Runtime.callFunctionOn", params)
        r = res.get("result", {})
        if r.get("exceptionDetails"):
            raise RuntimeError(json.dumps(r["exceptionDetails"]))
        return r.get("value") if return_by_value else r

    def get_attribute(self, name):
        return self._fn(
            "function(n){ return this.getAttribute(n); }",
            [{"value": name}],
        )

    def set_attribute(self, name, value):
        return self._fn(
            "function(n, v){ this.setAttribute(n, v); return true; }",
            [{"value": name}, {"value": value}],
        )

    def get_property(self, prop):
        return self._fn(
            "function(p){ var v = this[p]; return (v === null || v === undefined) ? null : String(v); }",
            [{"value": prop}],
        )

    @property
    def inner_text(self):
        return self._fn("function(){ return this.innerText; }")

    @property
    def inner_html(self):
        return self._fn("function(){ return this.innerHTML; }")

    @property
    def outer_html(self):
        return self._fn("function(){ return this.outerHTML; }")

    @property
    def tag_name(self):
        return self._fn("function(){ return this.tagName; }")

    @property
    def value(self):
        return self._fn("function(){ return this.value; }")

    def focus(self):
        self._fn("function(){ this.focus(); return true; }")

    def click(self):
        self._fn("function(){ this.click(); return true; }")

    def rect(self):
        """Bounding box dict with x,y,width,height (for screenshot clip)."""
        return self._fn(
            "function(){ var r=this.getBoundingClientRect();"
            " return {x:r.x, y:r.y, width:r.width, height:r.height}; }"
        )


class MedulaEngine:
    def __init__(self, cdp_client):
        self._c = cdp_client

    def call(self, method, params=None):
        return self._c.call(method, params)

    # -- navigation ---------------------------------------------------------
    def navigate(self, url):
        self._c.call("Page.navigate", {"url": url})

    def evaluate(self, expression, return_by_value=True):
        res = self._c.call(
            "Runtime.evaluate",
            {"expression": expression, "returnByValue": return_by_value},
        )
        r = res.get("result", {})
        if r.get("exceptionDetails"):
            raise RuntimeError(json.dumps(r["exceptionDetails"]))
        return r.get("value") if return_by_value else r

    # -- document surface ---------------------------------------------------
    def get_element_by_id(self, el_id):
        r = self.evaluate("document.getElementById(%s)" % json.dumps(el_id), return_by_value=False)
        oid = r.get("objectId")
        if oid is None:
            return None
        return ElementHandle(self, oid, description="#%s" % el_id)

    def get_elements_by_tag_name(self, tag):
        r = self.evaluate(
            "Array.from(document.getElementsByTagName(%s)).length"
            % json.dumps(tag)
        )
        return r

    def document_title(self):
        return self.evaluate("document.title")

    def location_href(self):
        return self.evaluate("location.href")

    def ready_state(self):
        return self.evaluate("document.readyState")

    # -- screenshot (captcha -> DrawToDC equivalent) ------------------------
    def screenshot_png(self, clip=None):
        params = {"format": "png"}
        if clip:
            params["clip"] = {
                "x": clip["x"],
                "y": clip["y"],
                "width": clip["width"],
                "height": clip["height"],
                "scale": 1,
            }
        res = self._c.call("Page.captureScreenshot", params)
        return base64.b64decode(res["data"])

    def screenshot_element(self, handle):
        rect = handle.rect()
        return self.screenshot_png(clip=rect)

    # -- event sinking (AttachEventHandler equivalent) ----------------------
    def attach_event(self, name, callback):
        """name e.g. 'click'. Install a CDP binding and a page-side listener that
        calls it; route Runtime.bindingCalled events to `callback(payload_dict)`."""
        binding = "medulaEvent"
        self._c.call("Runtime.addBinding", {"name": binding})

        def _dispatch(evt):
            if evt.get("method") == "Runtime.bindingCalled":
                p = evt.get("params", {})
                if p.get("name") == binding:
                    try:
                        payload = json.loads(p.get("payload", "{}"))
                    except ValueError:
                        payload = {"raw": p.get("payload")}
                    payload.setdefault("_event", name)
                    callback(payload)

        existing = self._c._event_cb
        self._c.on_event(
            dispatch_both(existing, _dispatch)
        )

        self.evaluate(
            "(() => {"
            "  window.__medulaBindings = window.__medulaBindings || {};"
            "  if (window.__medulaBindings[%(bind)r]) return;"
            "  window.__medulaBindings[%(bind)r] = true;"
            "  document.addEventListener(%(n)r, (e) => {"
            "    const t = e.target;"
            "    %(bind)s(JSON.stringify({event:%(n)r, id:t.id||null,"
            "      tag:t.tagName||null, value:t.value||null}));"
            "  }, true);"
            "})()"
            % {"bind": binding, "n": name}
        )
        return self


def dispatch_both(first, second):
    def _d(evt):
        if first is not None:
            try:
                first(evt)
            except Exception:
                pass
        second(evt)
    return _d


def connect(port=8080, host="127.0.0.1"):
    """Connect to a Chromium/CEF debugging port and wrap it in a MedulaEngine."""
    c = _cdp.CDPClient(port=port, host=host)
    c.connect()
    _cdp.enable_common(c)
    return MedulaEngine(c)

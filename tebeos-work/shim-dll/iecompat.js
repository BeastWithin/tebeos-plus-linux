/* iecompat.js -- run in every new document of a shim tab (Page.addScriptToEvaluateOnNewDocument).
 * Regenerate iecompat.h after editing:  python3 gen-iecompat.py
 *
 * 1. Untyped <button>: IE7/quirks makes it type=button, Chromium type=submit. The vendor's injected
 *    buttons ("Hasta Arama(F3)", "E-Recete Giris(F2/F4)") then submitted the SGK form on every
 *    click (once SGK answered with LogoutServlet). Give each such button IE's type as it enters
 *    the document; explicit types are left alone.
 * 2. <select> drop-downs: Chromium draws the open list in a separate popup widget that a headless
 *    tab never shows (nor puts in the screencast), so a click on a drop-down opened nothing
 *    visible. Draw the list in the page instead; picking sets the value and fires input/change
 *    as the native list does. Keyboard: Up/Down/Home/End move, Enter picks, Escape/Tab close;
 *    Alt+Down or F4 on a focused drop-down opens it.
 * 3. A checkbox/radio click after a vendor mousedown/mouseup: IE runs the vendor's handler for
 *    those before the click flips the box; the shim's run later (asynchronously), so they saw the
 *    box already flipped. Recete Giris's drug checkboxes ("f:tbl1:N:checkbox7") have a TEBEOS
 *    mouseup handler that clears every box when exactly one is ticked -- in IE the old tick,
 *    here the new one, so a box unticked itself at once. The listener for a vendor's mouse
 *    event asks __shim_hold for a token, the native click that follows is cancelled, and when
 *    the shim has run the vendor's handlers it calls __shim_ack, which replays the click.
 * 4. Right click: whether the page cancelled its contextmenu event (oncontextmenu returning
 *    false), read by the shim before it shows IE's edit menu (docobj.c doc_context_menu).
 * 5. TEXTAREA innerText: IE gives the box's current text (its value), Chromium always ''. TEBEOS
 *    reads the Karekod box (form1:textarea1) with .InnerText to save the scanned karekods into
 *    its sale; empty, it saved none, and its repair routine (Form1 status 87: Karekod Iptal ->
 *    Islemleri -> Geri Don -> Sonlandir) then repeated against SGK without end.
 * 6. window.print(): a headless tab prints nothing. It is sent to the shim (the __shim_evt
 *    binding, t "__print"), which prints the page as IE does (docobj.c doc_print).
 */
(function () {
    if (window.__shim_iecompat) return;
    window.__shim_iecompat = 1;

    function fixButtons(n) {
        if (!n || n.nodeType !== 1) return;
        if (n.tagName === 'BUTTON' && !n.hasAttribute('type')) n.setAttribute('type', 'button');
        var b = n.getElementsByTagName('button');
        for (var i = 0; i < b.length; i++)
            if (!b[i].hasAttribute('type')) b[i].setAttribute('type', 'button');
    }
    new MutationObserver(function (m) {
        for (var i = 0; i < m.length; i++)
            for (var j = 0; j < m[i].addedNodes.length; j++) fixButtons(m[i].addedNodes[j]);
    }).observe(document, { childList: true, subtree: true });

    var held = {}, nheld = 0, hel = null, hclick = false, hseq = 0, htimer = 0;
    function release() {
        var el = hel, c = hclick;
        clearTimeout(htimer);
        held = {}; nheld = 0; hel = null; hclick = false;
        if (c && el && el.isConnected && !el.disabled) el.click();
    }
    window.__shim_hold = function (e) {
        var t = e.target;
        if (!e.isTrusted || (e.type !== 'mousedown' && e.type !== 'mouseup') || !t ||
            t.tagName !== 'INPUT' || (t.type !== 'checkbox' && t.type !== 'radio')) return 0;
        if (hel && hel !== t) release();
        hel = t;
        held[++hseq] = 1;
        nheld++;
        clearTimeout(htimer);
        htimer = setTimeout(release, 5000); /* an answer that never comes */
        return hseq;
    };
    window.__shim_ack = function (q) {
        if (held[q]) { delete held[q]; if (--nheld === 0) release(); }
    };
    window.addEventListener('click', function (e) {
        if (nheld && e.isTrusted && e.target === hel) {
            e.preventDefault(); /* the box flips back */
            e.stopImmediatePropagation();
            hclick = true;
        }
    }, true);
    window.addEventListener('mousedown', function (e) { if (hel && e.isTrusted) release(); }, true);

    var cur = null; /* { s: select, box: div, items: [div], hi: index } */

    function close() {
        if (!cur) return;
        var b = cur.box;
        cur = null;
        if (b.parentNode) b.parentNode.removeChild(b);
    }
    function mark(i) {
        if (!cur || i < 0 || i >= cur.items.length) return;
        if (cur.hi >= 0 && cur.items[cur.hi]) {
            cur.items[cur.hi].style.background = '';
            cur.items[cur.hi].style.color = cur.s.options[cur.hi].disabled ? 'GrayText' : '';
        }
        cur.hi = i;
        cur.items[i].style.background = 'Highlight';
        cur.items[i].style.color = 'HighlightText';
    }
    function scrollTo(i) {
        var it = cur && cur.items[i], b = cur && cur.box;
        if (!it) return;
        if (it.offsetTop < b.scrollTop) b.scrollTop = it.offsetTop;
        else if (it.offsetTop + it.offsetHeight > b.scrollTop + b.clientHeight)
            b.scrollTop = it.offsetTop + it.offsetHeight - b.clientHeight;
    }
    function pick(i) {
        var s = cur && cur.s;
        close();
        if (!s || i < 0 || i >= s.options.length || s.options[i].disabled) return;
        if (s.selectedIndex !== i) {
            s.selectedIndex = i;
            s.dispatchEvent(new Event('input', { bubbles: true }));
            s.dispatchEvent(new Event('change', { bubbles: true }));
        }
    }
    function step(i, d) {
        var o = cur.s.options;
        for (var j = i + d; j >= 0 && j < o.length; j += d)
            if (!o[j].disabled) return j;
        return i;
    }
    function open(s) {
        close();
        var d = document, r = s.getBoundingClientRect(), cs = getComputedStyle(s);
        var box = d.createElement('div'), items = [], o = s.options;
        box.setAttribute('data-shim-select', '1');
        box.style.cssText = 'position:fixed;z-index:2147483647;background:Field;color:FieldText;' +
            'border:1px solid #767676;box-sizing:border-box;overflow-y:auto;overflow-x:hidden;' +
            'margin:0;padding:0;text-align:left;cursor:default;' +
            'font-family:' + cs.fontFamily + ';font-size:' + cs.fontSize + ';';
        for (var i = 0; i < o.length; i++) {
            var it = d.createElement('div');
            it.textContent = o[i].text || ' ';
            it.style.cssText = 'padding:1px 4px;white-space:nowrap;' + (o[i].disabled ? 'color:GrayText;' : '');
            (function (k) {
                it.addEventListener('mousemove', function () { if (!s.options[k].disabled) mark(k); });
                it.addEventListener('mouseup', function (e) { if (e.button === 0) pick(k); });
            })(i);
            box.appendChild(it);
            items.push(it);
        }
        box.addEventListener('mousedown', function (e) { e.preventDefault(); e.stopPropagation(); }, true);
        (d.body || d.documentElement).appendChild(box);
        var vh = window.innerHeight, rowh = items.length ? items[0].offsetHeight : 16;
        var want = Math.min(items.length, 20) * rowh + 2;
        var below = vh - r.bottom, above = r.top, h, top;
        if (want <= below || below >= above) { h = Math.min(want, below); top = r.bottom; }
        else { h = Math.min(want, above); top = r.top - h; }
        box.style.left = Math.max(0, r.left) + 'px';
        box.style.top = top + 'px';
        box.style.minWidth = r.width + 'px';
        box.style.height = Math.max(h, rowh + 2) + 'px';
        cur = { s: s, box: box, items: items, hi: -1 };
        mark(s.selectedIndex >= 0 ? s.selectedIndex : step(-1, 1));
        scrollTo(cur.hi);
    }
    function dropdown(t) {
        var s = t && t.closest ? t.closest('select') : null;
        return s && !s.multiple && !(s.size > 1) && !s.disabled ? s : null;
    }

    document.addEventListener('mousedown', function (e) {
        if (cur && cur.box.contains(e.target)) return;
        var s = dropdown(e.target), was = cur && cur.s;
        close();
        if (!s || e.button !== 0) return;
        e.preventDefault(); /* the invisible native popup */
        if (s === was) { s.focus(); return; } /* a second click closes it, as natively */
        s.focus();
        open(s);
    }, true);
    document.addEventListener('keydown', function (e) {
        var k = e.keyCode;
        if (!cur) {
            var s = dropdown(document.activeElement);
            if (s && ((e.altKey && (k === 38 || k === 40)) || (k === 115 && !e.altKey && !e.ctrlKey))) {
                e.preventDefault();
                open(s);
            }
            return;
        }
        if (k === 27 || k === 9) { close(); if (k === 27) e.preventDefault(); return; }
        if (k === 13) { e.preventDefault(); e.stopPropagation(); pick(cur.hi); return; }
        var n = cur.hi;
        if (k === 38) n = e.altKey ? -2 : step(n, -1);
        else if (k === 40) n = step(n, 1);
        else if (k === 36) n = step(-1, 1);
        else if (k === 35) n = step(cur.items.length, -1);
        else if (k === 33) n = Math.max(0, n - 10);
        else if (k === 34) n = Math.min(cur.items.length - 1, n + 10);
        else return;
        e.preventDefault();
        e.stopPropagation();
        if (n === -2) { pick(cur.hi); return; }
        mark(n);
        scrollTo(n);
    }, true);
    window.addEventListener('scroll', function (e) {
        if (cur && e.target !== cur.box) close();
    }, true);
    window.addEventListener('resize', close);
    window.addEventListener('blur', close);
    window.addEventListener('pagehide', close);
})();

/* 4. contextmenu: 2 if the page cancelled it, 1 if not (listening last, on the window) */
window.addEventListener('contextmenu', function (e) {
    window.__shim_ctx = e.defaultPrevented ? 2 : 1;
});

/* 5. TEXTAREA innerText is its value, as in IE (getter and setter) */
try {
    Object.defineProperty(HTMLTextAreaElement.prototype, 'innerText', {
        configurable: true,
        get: function () { return this.value; },
        set: function (v) { this.value = v === null || v === undefined ? '' : String(v); }
    });
} catch (e) { }

/* 6. window.print(): headless Chromium ignores it; the shim shows the print dialog and prints */
window.print = function () {
    try { if (window.__shim_evt) window.__shim_evt('{"t":"__print"}'); } catch (e) { }
};

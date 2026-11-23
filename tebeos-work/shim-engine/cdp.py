"""
cdp.py -- minimal Chrome DevTools Protocol client.

Dependency-free (stdlib only). Purpose: the "engine half" of the Path-A shim.
The eventual native C++ mshtml.dll will implement the same operations (see
HANDOFF-PATH-A.md section 2c/5a) using CEF's own CDP; this module is the
reference implementation and a live test harness for that mapping.

CDP is:  1) HTTP `GET /json` returns the list of debuggable targets (each with a
`webSocketDebuggerUrl`);  2) a WebSocket to that URL for the actual JSON-RPC.
Since CPython's stdlib ships no WebSocket client, we implement RFC 6455 by hand
(client frames are masked, as required).

Usage:
    c = CDPClient(port=8080)        # the in-app CEF (TEBEOS sets 8080)
    c.connect()                     # /json discovery + WebSocket handshake
    r = c.call("Runtime.evaluate", {"expression": "document.title",
                                    "returnByValue": True})
"""

import base64
import hashlib
import json
import os
import socket
import struct
import threading
import time
import urllib.parse
import urllib.request

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class WebSocketError(Exception):
    pass


class CdpError(Exception):
    def __init__(self, error_obj):
        msg = error_obj.get("message", "CDP error") if isinstance(error_obj, dict) else str(error_obj)
        super().__init__(msg)
        self.code = error_obj.get("code") if isinstance(error_obj, dict) else None
        self.data = error_obj.get("data") if isinstance(error_obj, dict) else None


class WebSocket:
    """Minimal RFC 6455 client. Text frames only are treated as messages."""

    def __init__(self, url, timeout=30.0):
        p = urllib.parse.urlsplit(url)
        if p.scheme not in ("ws", "wss"):
            raise WebSocketError("unsupported scheme: %s" % p.scheme)
        self.host = p.hostname
        self.port = p.port or (443 if p.scheme == "wss" else 80)
        self.path = p.path or "/"
        if p.query:
            self.path += "?" + p.query
        self.timeout = timeout
        self.sock = None
        self._closed = False

    # -- connection ---------------------------------------------------------
    def connect(self):
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n"
        ) % (self.path, self.host, self.port, key)

        if self.host.startswith("["):
            host = self.host
        else:
            host = self.host
        s = socket.create_connection((self.host, self.port), timeout=self.timeout)
        s.sendall(req.encode("latin-1"))

        # read response headers until blank line
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = s.recv(4096)
            if not chunk:
                raise WebSocketError("connection closed during handshake")
            resp += chunk
            if len(resp) > 65536:
                raise WebSocketError("handshake response too large")
        head, _, rest = resp.partition(b"\r\n\r\n")

        lines = head.decode("latin-1").split("\r\n")
        status = lines[0]
        headers = {}
        for ln in lines[1:]:
            if ":" in ln:
                k, _, v = ln.partition(":")
                headers[k.strip().lower()] = v.strip()

        if " 101 " not in status and not status.startswith("HTTP/1.1 101"):
            raise WebSocketError("handshake rejected: %s" % status)

        accept_expected = base64.b64encode(
            hashlib.sha1((key + GUID).encode()).digest()
        ).decode()
        if headers.get("sec-websocket-accept") != accept_expected:
            raise WebSocketError("Sec-WebSocket-Accept mismatch")

        self.sock = s
        if rest:
            self._buffer = rest
        else:
            self._buffer = b""
        self._closed = False
        return self

    # -- low-level ----------------------------------------------------------
    def _recv_exact(self, n):
        while len(self._buffer) < n:
            try:
                chunk = self.sock.recv(65536)
            except OSError:
                raise WebSocketError("connection closed")
            if not chunk:
                raise WebSocketError("connection closed")
            self._buffer += chunk
        out, self._buffer = self._buffer[:n], self._buffer[n:]
        return out

    def _send_frame(self, opcode, payload):
        b0 = 0x80 | (opcode & 0x0F)  # FIN + opcode
        mask = os.urandom(4)
        n = len(payload)
        header = bytearray([b0])
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126)
            header += struct.pack(">H", n)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", n)
        header += mask
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def _recv_frame(self):
        b0, b1 = self._recv_exact(2)
        fin = bool(b0 & 0x80)
        opcode = b0 & 0x0F
        masked = bool(b1 & 0x80)
        length = b1 & 0x7F
        if length == 126:
            length = struct.unpack(">H", self._recv_exact(2))[0]
        elif length == 127:
            length = struct.unpack(">Q", self._recv_exact(8))[0]
        mask = self._recv_exact(4) if masked else None
        payload = self._recv_exact(length)
        if mask:
            payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        return fin, opcode, payload

    def send_text(self, text):
        self._send_frame(0x1, text.encode("utf-8"))

    def recv_message(self):
        """Return the next complete text message (str), or None on close."""
        parts = []
        while True:
            fin, opcode, payload = self._recv_frame()
            if opcode == 0x9:  # ping -> pong
                self._send_frame(0xA, payload)
                continue
            if opcode == 0xA:  # pong
                continue
            if opcode == 0x8:  # close
                self._closed = True
                return None
            if opcode in (0x0, 0x1, 0x2):  # continuation/text/binary
                parts.append(payload)
                if fin:
                    data = b"".join(parts)
                    try:
                        return data.decode("utf-8")
                    except UnicodeDecodeError:
                        return None
                continue
            # unknown opcode: ignore frame, keep going
            if fin:
                return None

    def close(self):
        try:
            self._send_frame(0x8, b"")
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass
        self._closed = True


class CDPClient:
    """Tiny CDP client: /json discovery + JSON-RPC over WebSocket."""

    def __init__(self, port=8080, host="127.0.0.1", timeout=30.0):
        self.port = port
        self.host = host
        self.timeout = timeout
        self.ws = None
        self._id = 0
        self._lock = threading.Lock()
        self._pending = {}
        self._events = []
        self._event_cb = None
        self._reader = None
        self._closed = False

    # -- discovery ----------------------------------------------------------
    def _list_targets(self):
        url = "http://%s:%d/json" % (self.host, self.port)
        with urllib.request.urlopen(url, timeout=self.timeout) as r:
            return json.loads(r.read().decode())

    def pick_page_url(self):
        """Return the webSocketDebuggerUrl of a 'page' target (first available)."""
        targets = self._list_targets()
        for t in targets:
            if t.get("type") == "page":
                return t["webSocketDebuggerUrl"]
        # fall back to any target that has a ws url
        for t in targets:
            if t.get("webSocketDebuggerUrl"):
                return t["webSocketDebuggerUrl"]
        raise WebSocketError("no debuggable page target found on port %d" % self.port)

    def connect(self, ws_url=None):
        if ws_url is None:
            ws_url = self.pick_page_url()
        self.ws = WebSocket(ws_url, timeout=self.timeout).connect()
        self._reader = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader.start()
        return self

    def connect_url(self, ws_url):
        """Attach to an explicit ws URL (used by tests)."""
        return self.connect(ws_url=ws_url)

    # -- reader -------------------------------------------------------------
    def _reader_loop(self):
        while not self._closed:
            try:
                msg = self.ws.recv_message()
            except (WebSocketError, OSError):
                break
            if msg is None:
                break
            try:
                data = json.loads(msg)
            except ValueError:
                continue
            if isinstance(data, dict) and data.get("id") is not None and "method" not in data:
                self._on_response(data)
            elif isinstance(data, dict) and data.get("method"):
                self._on_event(data)

    def _on_response(self, data):
        ident = data["id"]
        with self._lock:
            entry = self._pending.pop(ident, None)
        if entry is not None:
            entry.set_result(data)

    def _on_event(self, data):
        self._events.append(data)
        cb = self._event_cb
        if cb is not None:
            try:
                cb(data)
            except Exception:
                pass

    def on_event(self, callback):
        self._event_cb = callback

    # -- RPC ------------------------------------------------------------------
    def call(self, method, params=None, timeout=None):
        if self.ws is None:
            raise WebSocketError("not connected")
        with self._lock:
            self._id += 1
            ident = self._id
        msg = {"id": ident, "method": method}
        if params:
            msg["params"] = params
        fut = _Future()
        with self._lock:
            self._pending[ident] = fut
        try:
            self.ws.send_text(json.dumps(msg))
            fut.wait(timeout or self.timeout)
            data = fut.result
            if data is None:
                raise TimeoutError("CDP call timed out: %s" % method)
            if "error" in data:
                raise CdpError(data["error"])
            return data.get("result", {})
        finally:
            with self._lock:
                self._pending.pop(ident, None)

    def close(self):
        self._closed = True
        if self.ws is not None:
            self.ws.close()


class _Future:
    def __init__(self):
        self._ev = threading.Event()
        self.result = None

    def set_result(self, value):
        self.result = value
        self._ev.set()

    def wait(self, timeout):
        return self._ev.wait(timeout)


# convenience: enable common domains on connect
def enable_common(c, page=True, runtime=True):
    if page:
        c.call("Page.enable")
    if runtime:
        c.call("Runtime.enable")
    return c

# cdp_eval.py -- evaluate one JavaScript expression in the SGK page of a debug port (read-only use).
#   cdp_eval.py <port> '<expression>'
import json, os, socket, struct, sys, base64, urllib.request
port, expr = int(sys.argv[1]), sys.argv[2]
tabs = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json"))
flt = os.environ.get('PAGE_FILTER', 'sgk')
page = [t for t in tabs if t['type'] == 'page' and flt in t['url']][0]
s = socket.create_connection(("127.0.0.1", port)); path = page['webSocketDebuggerUrl'].split(str(port), 1)[1]
key = base64.b64encode(os.urandom(16)).decode()
s.sendall((f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
buf = b""
while b"\r\n\r\n" not in buf: buf += s.recv(4096)
buf = buf.split(b"\r\n\r\n", 1)[1]
d = json.dumps({"id": 1, "method": "Runtime.evaluate", "params": {"expression": expr, "returnByValue": True}}).encode(); m = os.urandom(4)
h = bytes([0x81]) + (bytes([0x80 | len(d)]) if len(d) < 126 else bytes([0x80 | 126]) + struct.pack(">H", len(d)))
s.sendall(h + m + bytes(c ^ m[i % 4] for i, c in enumerate(d)))
while True:
    while len(buf) < 2: buf += s.recv(65536)
    n = buf[1] & 0x7f; off = 2
    if n == 126: n = struct.unpack(">H", buf[2:4])[0]; off = 4
    elif n == 127: n = struct.unpack(">Q", buf[2:10])[0]; off = 10
    while len(buf) < off + n: buf += s.recv(1 << 20)
    p = buf[off:off + n]; buf = buf[off + n:]
    r = json.loads(p)
    if r.get("id") == 1:
        print(json.dumps(r.get("result", r), ensure_ascii=False)[:600]); break

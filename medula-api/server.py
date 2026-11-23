#!/usr/bin/env python3
"""Medula engine — local HTTP/JSON API over the SOAP web services.

This is the "Medula as a clean local API" proof-of-concept. TEBEOS (or a
rewrite) can call:

    GET  /health                          -> {"status":"ok","eczaneKodu":...}
    GET  /stok?tarih=dd.MM.yyyy           -> gunlukStokBilgisiSorgulama result
    POST /stok        {"tarih": "..."}    -> same
    GET  /ilac?tur=aktif&tarih=dd.MM.yyyy -> reimbursable drug list
                                           (9108 for pharmacy creds today)

Run:

    MEDULA_ECZANE_KODU=<ECZANE_KODU> MEDULA_PAROLA=... python3 server.py
    # or: python3 server.py --config config.json

Only the stdlib is used here; ``requests`` is the sole runtime dependency.
"""

from __future__ import annotations

import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from engine import ConfigError, DrugListClient, StockClient, load_config


class Handler(BaseHTTPRequestHandler):
    client: StockClient | None = None
    drugs: DrugListClient | None = None
    eczane_kodu: str = ""

    def _send(self, code: int, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802 (http.server naming)
        parsed = urlparse(self.path)
        route = parsed.path.rstrip("/") or "/"

        if route == "/health":
            self._send(200, {"status": "ok", "eczaneKodu": self.eczane_kodu})
            return

        if route == "/stok":
            tarih = parse_qs(parsed.query).get("tarih", [None])[0]
            self._stok(tarih)
            return

        if route == "/ilac":
            qs = parse_qs(parsed.query)
            tarih = qs.get("tarih", [None])[0]
            kind = qs.get("tur", ["aktif"])[0]
            self._ilac(kind, tarih)
            return

        self._send(404, {"error": f"unknown route {route}"})

    def do_POST(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        route = parsed.path.rstrip("/") or "/"

        if route != "/stok":
            self._send(404, {"error": f"unknown route {route}"})
            return

        length = int(self.headers.get("Content-Length", 0))
        tarih = None
        if length:
            try:
                tarih = json.loads(self.rfile.read(length)).get("tarih")
            except (ValueError, AttributeError):
                self._send(400, {"error": "invalid JSON; want {\"tarih\": \"dd.MM.yyyy\"}"})
                return
        self._stok(tarih)

    def _stok(self, tarih: str | None) -> None:
        try:
            result = self.client.daily_stock(tarih)
        except Exception as exc:  # noqa: BLE001 - report any engine failure
            self._send(502, {"error": str(exc)})
            return
        self._send(200, result)

    def _ilac(self, kind: str, tarih: str | None) -> None:
        try:
            k = kind.lower()
            if k == "aktif":
                result = self.drugs.active(tarih)
            elif k == "pasif":
                result = self.drugs.passive(tarih)
            elif k == "normal":
                result = self.drugs.normal(tarih)
            elif k in ("yesil", "kirmizi", "turuncu", "mor"):
                result = self.drugs.by_colour(k, tarih)
            else:
                self._send(400, {"error": f"unknown tur={kind}"})
                return
        except Exception as exc:  # noqa: BLE001
            self._send(502, {"error": str(exc)})
            return
        self._send(200, result)

    def log_message(self, *args) -> None:  # quiet
        pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", help="path to config.json")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8090)
    args = parser.parse_args()

    try:
        config = load_config(args.config)
    except ConfigError as exc:
        print(exc)
        return 2

    Handler.client = StockClient(config)
    Handler.drugs = DrugListClient(config)
    Handler.eczane_kodu = config.eczane_kodu

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"Medula engine listening on http://{args.host}:{args.port}")
    print(f"  GET /health   GET|POST /stok   GET /ilac  (eczane {config.eczane_kodu})")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

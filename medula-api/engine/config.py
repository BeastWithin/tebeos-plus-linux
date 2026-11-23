"""Credential + endpoint configuration.

Credentials are sensitive and must never be committed. Load order:

1. Environment variables (recommended for this PoC / CI):
   ``MEDULA_ECZANE_KODU`` and ``MEDULA_PAROLA``.
2. ``config.json`` next to this package (git-ignored).

``config.example.json`` documents the shape.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from pathlib import Path

_PACKAGE_DIR = Path(__file__).resolve().parent

# SGK pharmacy Medula base host.
ENDPOINTS = {
    # Confirmed working (see NOTES.md): daily dispensed-stock query.
    "stock": "https://medeczane.sgk.gov.tr/medula/eczane/eczaneStokIslemleriWS",
    # Reimbursable drug lists (served at the pharmacy path, see NOTES.md).
    "drugs": "https://medeczane.sgk.gov.tr/medula/eczane/saglikTesisiYardimciIslemleriWS",
}

# SOAP service namespace (document/literal, SOAP 1.1).
NAMESPACE = "http://servisler.ws.eczane.gss.sgk.gov.tr"


@dataclass
class Config:
    eczane_kodu: str
    parola: str
    endpoints: dict = field(default_factory=lambda: dict(ENDPOINTS))
    namespace: str = NAMESPACE
    timeout_seconds: float = 30.0

    @property
    def masked(self) -> str:
        return f"Config(eczane_kodu={self.eczane_kodu!r}, parola=***)"


def load_config(config_path: str | Path | None = None) -> Config:
    """Load config from env vars, then from ``config.json``.

    Env vars win over the file. Raises ``ConfigError`` if no credentials found.
    """
    code = os.environ.get("MEDULA_ECZANE_KODU")
    password = os.environ.get("MEDULA_PAROLA")

    if not code or not password:
        path = Path(config_path) if config_path else _PACKAGE_DIR.parent / "config.json"
        if path.exists():
            data = json.loads(path.read_text(encoding="utf-8"))
            code = code or data.get("eczane_kodu")
            password = password or data.get("parola")

    if not code or not password:
        raise ConfigError(
            "Missing Medula credentials. Set MEDULA_ECZANE_KODU / MEDULA_PAROLA "
            "or provide a config.json (see config.example.json)."
        )

    return Config(eczane_kodu=code.strip(), parola=password.strip())


class ConfigError(RuntimeError):
    pass

"""Stock web service client — ``eczaneStokIslemleriWS``.

Operation: ``gunlukStokBilgisiSorgulama`` (daily stock/dispensed-prescriptions
query). Confirmed live (sonucKodu 0000) — see NOTES.md.
"""

from __future__ import annotations

from datetime import date
from typing import Any

from .config import Config
from .soap import SoapClient, SoapError

_OPERATION = "gunlukStokBilgisiSorgulama"


class StockClient:
    def __init__(self, config: Config):
        self._soap = SoapClient(config, config.endpoints["stock"])

    def daily_stock(self, recete_son_islem_tarihi: str | None = None) -> dict[str, Any]:
        """Query dispensed prescriptions since ``recete_son_islem_tarihi``.

        Args:
            recete_son_islem_tarihi: ``dd.MM.yyyy``. Defaults to today.

        Returns the full parsed ``gunlukStokBilgisiSorgulamaReturn`` dict keyed
        by ``sonucKodu``, ``sonucMesaji``, ``uyariMesaji`` and the
        ``receteListesi`` / ``receteIlacListesi`` / ... arrays.
        """
        tarih = recete_son_islem_tarihi or date.today().strftime("%d.%m.%Y")
        body = (
            f"<arg0>"
            f"<receteSonIslemTarihi>{tarih}</receteSonIslemTarihi>"
            f"<eczaneKodu>{self._soap.config.eczane_kodu}</eczaneKodu>"
            f"</arg0>"
        )
        return self._soap.call(_OPERATION, body)

    def check(self) -> dict[str, Any]:
        """Thin wrapper that raises on a non-success result code."""
        result = self.daily_stock()
        code = result.get("sonucKodu")
        if code != "0000":
            raise SoapError(
                f"stock query failed: sonucKodu={code!r} "
                f"sonucMesaji={result.get('sonucMesaji')!r}"
            )
        return result

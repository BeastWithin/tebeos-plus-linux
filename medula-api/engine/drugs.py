"""Drug-list web services — ``saglikTesisiYardimciIslemleriWS``.

Despite the ``saglikTesisi`` ("health facility") name, SGK serves this at the
**pharmacy** path ``/medula/eczane/saglikTesisiYardimciIslemleriWS`` and it is
exactly what a pharmacy needs: the SGK reimbursable-drug lists.

Operations (all read-only ``...ListesiSorgula``):
    aktifIlacListesiSorgula       — active (currently reimbursed) drugs  [main]
    pasifIlacListesiSorgula       — passive (removed) drugs
    normalReceteIlacListesiSorgula— normal-prescription drugs
    yesil/kirmizi/turuncu/morReceteIlacListesiSorgula — colour-coded lists
    etkinMaddeListesiSorgula      — active-ingredient list
    raporTeshisListesiSorgula     — report-diagnosis list
    ...plus ``Yurtdisi`` (overseas) variants of each.

Request (``ilacListesiSorguIstekDVO``): { tesisKodu, doktorTcKimlikNo?, islemTarihi }.
Response (``ilacListesiSorguCevapDVO``): { sonucKodu, sonucMesaji, uyariMesaji,
ilacListesi[] } where each ``ilacDVO`` = { barkod, ilacAdi, sgkIlacKodu,
ambalajMiktari, tekDozMiktari, kutuBirimMiktari, ayaktanOdenmeSarti,
yatanOdenmeSarti, etkinMaddeKodu }.
"""

from __future__ import annotations

from datetime import date
from typing import Any

from .config import Config
from .soap import SoapClient, SoapError

_ENDPOINT_KEY = "drugs"


class DrugListClient:
    def __init__(self, config: Config):
        self._soap = SoapClient(config, config.endpoints[_ENDPOINT_KEY])

    def _query(self, operation: str, islem_tarihi: str | None) -> dict[str, Any]:
        tarih = islem_tarihi or date.today().strftime("%d.%m.%Y")
        body = (
            f"<arg0>"
            f"<tesisKodu>{self._soap.config.eczane_kodu}</tesisKodu>"
            f"<doktorTcKimlikNo xsi:nil=\"true\"/>"
            f"<islemTarihi>{tarih}</islemTarihi>"
            f"</arg0>"
        )
        return self._soap.call(operation, body)

    def active(self, islem_tarihi: str | None = None) -> dict[str, Any]:
        """Active reimbursable drug list (the one used day-to-day)."""
        result = self._query("aktifIlacListesiSorgula", islem_tarihi)
        self._ensure_ok(result)
        return result

    def passive(self, islem_tarihi: str | None = None) -> dict[str, Any]:
        result = self._query("pasifIlacListesiSorgula", islem_tarihi)
        self._ensure_ok(result)
        return result

    def normal(self, islem_tarihi: str | None = None) -> dict[str, Any]:
        result = self._query("normalReceteIlacListesiSorgula", islem_tarihi)
        self._ensure_ok(result)
        return result

    def by_colour(self, colour: str, islem_tarihi: str | None = None) -> dict[str, Any]:
        """``colour`` in {yesil, kirmizi, turuncu, mor}."""
        op = f"{colour}ReceteIlacListesiSorgula"
        result = self._query(op, islem_tarihi)
        self._ensure_ok(result)
        return result

    @staticmethod
    def _ensure_ok(result: dict[str, Any]) -> None:
        code = result.get("sonucKodu")
        if code != "0000":
            raise SoapError(
                f"drug list query failed: sonucKodu={code!r} "
                f"sonucMesaji={result.get('sonucMesaji')!r}"
            )

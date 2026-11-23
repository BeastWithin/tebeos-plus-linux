# Medula engine (proof-of-concept)

An **OS-neutral** client that talks to SGK's Medula **pharmacy SOAP web services**
and exposes them as a clean local HTTP/JSON API. This is the "treat Medula as a
normal API" idea, independent of the vendor's fragile IE/wine screen-scraping
and immune to the vendor's weekly exe/DB updates (nothing here touches the
vendor binary).

## What works today (verified live, read-only)

| Capability | Status |
|---|---|
| **Daily stock query** (`eczaneStokIslemleriWS.gunlukStokBilgisiSorgulama`) | ✅ `sonucKodu=0000`, real prescription rows |
| **Reimbursable drug list** (`saglikTesisiYardimciIslemleriWS.aktifIlacListesiSorgula`) | ❌ `sonucKodu=9108` "yetkiniz yok" for pharmacy creds |
| **Prescription provizyon / dispensing** | ❌ no SOAP API (UI/JSF only — this is *why* the vendor screen-scrapes) |

Key discovery this session: the drug-list service named `saglikTesisi*WS`
("health facility") is actually **served at the pharmacy path**
`/medula/eczane/saglikTesisiYardimciIslemleriWS`, but it still rejects pharmacy
credentials with `9108` and its request requires `doktorTcKimlikNo` (a doctor's
TC id) — so it is the **doctor/health-facility** side, not the pharmacy side.

## Auth

WS-Security `UsernameToken` (`PasswordText`) in the SOAP `<Header>` — **not**
HTTP Basic auth. Username = `eczaneKodu`, password = `parola`. TLS 1.2.

Credentials are loaded from env vars (`MEDULA_ECZANE_KODU`, `MEDULA_PAROLA`) or
a git-ignored `config.json` (see `config.example.json`). Never commit them.

## Layout

```
engine/
  soap.py     # SOAP 1.1 document/literal + WS-Security UsernameToken transport
  stock.py    # eczaneStokIslemleriWS (daily stock)
  drugs.py    # saglikTesisiYardimciIslemleriWS (drug lists — see 9108 note)
  config.py   # credentials + endpoints
query.py      # one-shot CLI
server.py     # local HTTP/JSON API (stdlib only)
```

Runtime dependency: only `requests` (`pip install -r requirements.txt`).

## Run

```bash
# one-shot stock query
MEDULA_ECZANE_KODU=<ECZANE_KODU> MEDULA_PAROLA='...' python3 query.py 01.10.2026

# drug list (returns 9108 for pharmacy creds)
MEDULA_ECZANE_KODU=... MEDULA_PAROLA='...' python3 query.py --ilac aktif

# local API
MEDULA_ECZANE_KODU=... MEDULA_PAROLA='...' python3 server.py --port 8090
curl localhost:8090/health
curl 'localhost:8090/stok?tarih=01.10.2026'
```

The stock response contains `receteListesi`, `receteIlacListesi`,
`receteMajistralIlacListesi`, `receteSilinenListesi`, `receteKarekodListesi`
(receteNo, kapsamTuru, ilacKatilimPayiTutari, toplamTutar, odenecekTutar, GTIN
karekod, …). See `eczaneStokIslemleriWS.xsd` for the full types.

## Notes / gotchas

- SGK returns **HTTP 500 with a parseable SOAP body** for error results; the
  engine parses the body first so you see the real `sonucKodu`/`sonucMesaji`.
- `soapAction` is the bare operation name (document/literal).

## Next steps (not yet done — user decision needed)

1. **Provizyon** has no SOAP API. Options: reverse-engineer the JSF POST
   (ViewState + fields) to replay dispensing directly, or fall back to browser
   automation (Playwright/Chromium) for that one screen.
2. Confirm whether any *pharmacy-authorised* drug-list endpoint exists (the
   `saglikTesisi*` one is doctor-side).

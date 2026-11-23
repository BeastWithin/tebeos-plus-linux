# Medula pharmacy web-service discovery (2026-10-05)

## Confirmed: SGK exposes SOAP web services to pharmacies

This overturns the earlier assumption ("Medula does not give API access to pharmacists").

### Stock web service (CONFIRMED — live WSDL fetched + live call WORKED)
- Endpoint:  https://medeczane.sgk.gov.tr/medula/eczane/eczaneStokIslemleriWS
- WSDL:      .../eczaneStokIslemleriWS?wsdl
- Style:     SOAP 1.1, document/literal; namespace http://servisler.ws.eczane.gss.sgk.gov.tr
- **Auth: WS-Security `UsernameToken` (PasswordText) in the SOAP `<Header>`.** NOT HTTP Basic
  (Basic returns `sonucKodu=9107` "Kimlik kontrolu basarisiz"; the working form is the
  `wsse:Security/UsernameToken` header). TLS 1.2 required.
- Operation: `gunlukStokBilgisiSorgulama`
    in:  { receteSonIslemTarihi: string(dd.MM.yyyy?), eczaneKodu: int }
    out: { sonucKodu("0000"=OK), sonucMesaji, uyariMesaji,
           receteListesi[], receteIlacListesi[], receteMajistralIlacListesi[],
           receteSilinenListesi[], receteKarekodListesi[] }
- **Verified live 2026-10-05**: correct UsernameToken returned `0000 / "İşlem başarı ile sona erdi."`
  plus real prescription rows (receteNo, kapsamTuru, ilacKatilimPayiTutari, toplamTutar,
  odenecekTutar, karekodVar). Working request and a synthetic response in the same format are saved as `stock_request.example.xml`
  / `stock_response.example.xml`.
- Credentials used (user-supplied): username/eczaneKodu = `<ECZANE_KODU>`, parola = `<MEDULA_PAROLA>`.

### Drug-list service — `saglikTesisiYardimciIslemleriWS` (CHECKED 2026-10-05)
- Served at the **pharmacy path** `https://medeczane.sgk.gov.tr/medula/eczane/saglikTesisiYardimciIslemleriWS`
  (despite the `saglikTesisi` "health facility" name).
- Operations: `aktifIlacListesiSorgula`, `pasifIlacListesiSorgula`,
  `normal/yesil/kirmizi/turuncu/morReceteIlacListesiSorgula`, `etkinMaddeListesiSorgula`,
  `raporTeshisListesiSorgula`, plus `Yurtdisi` variants.
- Request `ilacListesiSorguIstekDVO` = { tesisKodu, doktorTcKimlikNo (nillable), islemTarihi };
  response `ilacDVO` = { barkod, ilacAdi, sgkIlacKodu, ambalajMiktari, tekDozMiktari,
  kutuBirimMiktari, ayaktanOdenmeSarti, yatanOdenmeSarti, etkinMaddeKodu }.
- **RESULT: pharmacy credentials get `sonucKodu=9108` "Bu servisi kullanmaya yetkiniz yoktur"**
  → this is the doctor/health-facility side (its request demands `doktorTcKimlikNo`), NOT the
  pharmacy side. WSDL/XSD saved as `saglikTesisiYardimciIslemleriWS.wsdl` / `.xsd`.
- Other guessed pharmacy names (`eczaneIlacListesiWS`, `eczaneProvizyonWS`, …) all 302 →
  `YanlisAdres` (do not exist).

### Engine PoC (this session)
`engine/` is a working SOAP client (requests + stdlib, WS-Security UsernameToken) +
`query.py` CLI + `server.py` local JSON API (`/health`, `/stok`, `/ilac`). Stock call
confirmed live again (0000 + 67 prescriptions). See `README.md`.

### Open question: prescription provizyon + reçete kaydı (the dispensing workflow)
The physician e-reçete FAQ (SGK's `erecetesss.pdf`) references a "Medula Eczane Reçete Web Servisleri
Kullanım Kılavuzu" and an MRPS ("Medula Reçete Provizyon Sistemi") guide. The old MRPS
PDF URL is now dead (redirects to sgk.gov.tr homepage). The /doktor docs index lists
"SaglikTesisiReceteVeRaporWebServisleri.pdf" (health-facility prescription/report web
services) — confirming SGK routinely publishes SOAP web services.

=> NEXT: enumerate other pharmacy WS endpoints (provizyon / reçete kaydı / hasta sorgulama)
   to see whether the interactive dispensing workflow is SOAP-available too (which would
   fully replace the IE screen-scraping). Also try WS-Security auth on any found endpoints.

## Files
- (not included) EczaneStokWebServisleri.pdf — SGK's stock WS manual (Turkish), from SGK's Medula documentation pages
- (not included) erecetesss.pdf — SGK's physician e-reçete FAQ (Turkish)
- eczaneStokIslemleriWS.wsdl       — stock service WSDL
- eczaneStokIslemleriWS.xsd        — stock service full types
- stock_request.example.xml        — WORKING request (WS-Security UsernameToken)
- stock_response.example.xml       — SYNTHETIC response in the real format (made-up prescriptions, see the file's comment)

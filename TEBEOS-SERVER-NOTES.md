# TEBEOS server notes (Linux)

> Correction applied after the first write-up: the Linux server should keep
> `[BAGLANTI] CLIENT=FALSE` ("main computer") and `<Host>127.0.0.1</Host>`.
> Setting it to TRUE makes the app treat itself as a terminal, run its
> terminal discovery and rewrite `<Host>` to the machine's LAN IP - which
> then breaks if that IP changes. Both settings are corrected on the machine.


Working setup of **TEBEOS Plus (server / "Ana Bilgisayar")** on Linux via Wine.

---

## 1. Runtime components

| Piece | What we use | Where |
|---|---|---|
| Wine | **Wine 11.2 Staging** (ships with Lutris) | `~/.local/share/lutris/runners/wine/wine-staging-11.2-x86_64/bin/wine` |
| Wine prefix | `~/.wine` (win64, wow64-capable) | `~/.wine` |
| .NET | **.NET Framework 4.8** (installed via winetricks in an earlier attempt) | prefix registry: `NDP\v4\Full` Release = `0x80eb1` |
| Winetricks | Lutris' bundled copy + cached downloads | `~/.local/share/lutris/runtime/winetricks`, `~/.cache/winetricks` |
| App | TEBEOS Plus **1.2.9.3** (DB schema 1.3.8.0) | `~/.wine/drive_c/Program Files (x86)/TebeosPlusClient` |
| Database | **PostgreSQL 12.4** (bundled Windows build) | same folder, `pgsql/` + `data/` |

No system-wide packages were installed — everything runs from the user's home directory.

---

## 2. Database contract (from the vendor's own code)

Extracted by decompiling `tebeos_plus.exe` and `tebeos_utilities.dll`:

| Setting | Value |
|---|---|
| Host | `127.0.0.1` (from `tebeos_plus.xml` → `<Host>`) |
| Port | **5454** |
| Database | `tebeos_db`, UTF8 |
| User | `tebeos` |
| Data dir | `<install>\data` |

`Ayarlar.ini` keys: `[CONNECTİON] CLIENT=127.0.0.1`, `[BAGLANTI] CLIENT=TRUE|FALSE`
(`TRUE` = this PC is the main computer / server, `FALSE` = client terminal).

---

## 3. Bugs found and fixed

### 3.1 Startup crash — Turkish `İ` in an INI section name
`Ayarlar.ini` shipped with `[CONNECTION]` (ASCII `I`), but the code reads section
**`"CONNECTİON"`** (Turkish dotted capital `İ`, U+0130). The Win32 INI API
(`GetPrivateProfileString`) therefore returned an empty IP, and then:

```
System.FormatException: An invalid IP address was specified.
   at System.Net.IPAddress.Parse → Dns.GetHostByAddress
   at Client_Model.TerminalKontrol → Ana_Form..ctor
```

**Fix:** the section must contain the Turkish `İ` (`CONNECTİON`).

### 3.2 Latent bug in the vendor's port check
`Scanner.VeritabanıPortAcikmi()` uses `BeginConnect` + `AsyncWaitHandle.WaitOne(500)`
and treats the *completion* of the connect attempt as success — so it returns `true`
even for a **refused** connection. That's what let the invalid IP reach `Dns` parsing.

### 3.3 Vendor's `postgresql.conf` has Linux-style locale names
`conf/postgresql.conf.dist` sets `lc_messages/lc_monetary/lc_numeric/lc_time = 'tr_TR.UTF-8'`.
Windows PostgreSQL rejects those (`invalid value`), and the server refuses to start.
**Fix:** set them to `'C'`.

---

## 4. How the server was set up

Following the vendor's own `server.bat` (shipped by the server installer), adapted for Wine:

```sh
# 1) cluster  (vendor parameters)
pgsql/bin/initdb.exe --encoding=UTF-8 --username=tebeos --pgdata data

# 2) vendor configs
cp conf/pg_hba.conf.dist      data/pg_hba.conf
cp conf/postgresql.conf.dist  data/postgresql.conf      # then fix lc_* -> 'C'

# 3) start (the vendor uses: pg_ctl register + net start; we start it directly)
pgsql/bin/pg_ctl.exe -D "C:\Program Files (x86)\TebeosPlusClient\data" -l pgsql.log start

# 4) database + base schema
pgsql/bin/createdb.exe --echo -p 5454 -E UTF8 -U tebeos tebeos_db
pgsql/bin/psql.exe -U tebeos -d tebeos_db --port=5454 --single-transaction \
    --echo-errors -v ON_ERROR_STOP=1 --file="sql\tebeos_db.sql"
```

`sql/tebeos_db.sql` (127 MB) is the **base schema shipped by the server installer** —
it was the missing piece. Result: **44 schemas, 317 tables, 2018 functions.**

---

## 5. Launching

```bash
~/tebeos/start-tebeos.sh              # main app  (tebeos_plus.exe)
~/tebeos/start-tebeos.sh tebeos_medula # Medulla integration
```

The script starts PostgreSQL if it isn't already running, waits for port 5454,
then launches the app. Desktop shortcuts (`TebeosPlusClient`, `Tebeos Medula`) and
menu entries in `~/.local/share/applications/` now point at it.

Old shortcuts are backed up in `~/tebeos/backup-shortcuts/`.

---

## 6. Current state / open items

- ✅ App installs, starts, shows the login window, and talks to PostgreSQL.
- ⚠️ **Login credentials**: the base schema contains one default account,
  `tebeos`. You need its password (it is normally supplied by the vendor
  during provisioning) — or restore a backup of your own pharmacy database.
- ⚠️ The app displays *"Lütfen Terminal Kurulumu Yapınız!"* — a terminal/registration
  notice. May need a "terminal" record or the vendor's activation.
- ❓ **CEF/Medulla not yet exercised** — Medulla automation can only be tested after login.
- ❓ `Wizard.exe` (the vendor's server wizard) exits immediately under Wine; its steps
  were reproduced manually instead.

---

## 7. Useful commands

```bash
# is the DB up?
ss -tln | grep 5454

# DB shell
cd ~/.wine/drive_c/"Program Files (x86)"/TebeosPlusClient/pgsql/bin
WINEPREFIX=~/.wine ~/.local/share/lutris/runners/wine/wine-staging-11.2-x86_64/bin/wine \
    psql.exe -U tebeos -d tebeos_db -p 5454

# stop everything
~/.local/share/lutris/runners/wine/wine-staging-11.2-x86_64/bin/wineserver -k
```

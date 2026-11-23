# Installing the TEBEOS client (terminal) on Linux

This installs TEBEOS Plus as a **client/terminal** — i.e. a PC that connects to the
TEBEOS **main computer** ("Ana Bilgisayar", usually a Windows PC) that runs the
database. Everything is per-user; nothing is installed system-wide except a couple
of small packages.

* Script: `install-tebeos-client.sh`
* Tested concept on: CachyOS/Arch, Wine 11.2 (x86 + wow64 support)
* Works with: **Debian/Ubuntu (apt)**, **Arch (pacman)**, **Fedora (dnf)**,
  **openSUSE (zypper)**

---

## 1. Before you start — on the Windows main computer

1. Find its LAN IP address (e.g. `192.168.1.50`) with `ipconfig`.
   Give it a **static IP or a DHCP reservation** so it never changes.
2. Open the database port in the Windows firewall:

   ```powershell
   New-NetFirewallRule -DisplayName "TEBEOS DB" -Direction Inbound `
       -Protocol TCP -LocalPort 5454 -Action Allow
   ```

   Run PowerShell **as Administrator**. If the app still can't connect, check that
   the network profile is not "Public" (`Get-NetConnectionProfile`).
3. Test from the laptop before installing:

   ```bash
   bash -c "cat < /dev/null > /dev/tcp/192.168.1.50/5454" && echo reachable
   ```
   `reachable` means the database port is open. (Nothing prints if it is not.)

---

## 2. Install on the laptop

```bash
chmod +x install-tebeos-client.sh
./install-tebeos-client.sh --server-ip 192.168.1.50
```

Replace `192.168.1.50` with the Windows PC's IP. The script will:

1. install a few tools (`cabextract`, `curl`, `unzip`, `xz`) — it will ask for your
   sudo password; use `--skip-deps` if you prefer to install them yourself;
2. get Wine — your distro's package if it can run 32-bit programs, otherwise a
   portable build in `~/.local/share/tebeos/wine` (no root, no multilib needed);
3. create the Wine prefix `~/.wine-tebeos`;
4. download winetricks;
5. install **.NET Framework 4.8** — *this is the slow part, 5–20 minutes*;
6. install real Windows fonts (`corefonts`, `tahoma`) and graft the Turkish lira
   sign `₺` into whatever font draws money — see the troubleshooting notes below;
7. install **wine-gecko** and register MSHTML for 32-bit programs, which is what
   makes the **Medula** window's embedded browser work (skip with `--skip-gecko`);
8. download and run the vendor's TEBEOS client installer (~266 MB). If the silent
   install fails (it usually does) the script re-runs it **with its window shown**:
   click through it, accept the licence, keep the default folder;
9. point TEBEOS at your server (`tebeos_plus.xml` → `<Host>`, plus `Ayarlar.ini`);
10. create `~/tebeos/start-tebeos-client.sh` and menu/desktop entries;
11. check that the server is reachable and do a 30-second test launch.

**Close TEBEOS before (re-)running it.** The font and gecko steps use winetricks,
which waits for every Wine program in the prefix to exit; if TEBEOS is running the
script says so and skips those steps instead of hanging.

Useful extras:

```bash
./install-tebeos-client.sh --help
./install-tebeos-client.sh --server-ip 192.168.1.50 --wine download   # force portable Wine
./install-tebeos-client.sh --server-ip 192.168.1.50 --installer ./tebeos_plus_client_setup_v1.exe
./install-tebeos-client.sh --skip-deps --skip-dotnet --skip-install --server-ip 192.168.1.50
        # ^ only (re)apply the configuration + launcher
./install-tebeos-client.sh --dry-run          # show what would happen
```

If it breaks halfway, just run it again — every step is skipped if already done.

---

## 3. Starting TEBEOS afterwards

```bash
~/tebeos/start-tebeos-client.sh              # normal start
~/tebeos/start-tebeos-client.sh tebeos_medula  # Medulla module
```

or use the **"TEBEOS Plus (client)"** entry in your application menu / on the desktop.

On the login screen choose **Terminal**, and use your TEBEOS user name and password.
Ticking "Beni Hatırla" remembers them.

---

## 4. Changing the server address later

Edit this file (it is the one the app really uses):

```
~/.wine-tebeos/drive_c/Program Files (x86)/TebeosPlusClient/tebeos_plus.xml
```

```xml
<Host>192.168.1.50</Host>
```

…or simply re-run the script with the new address:

```bash
./install-tebeos-client.sh --skip-deps --skip-dotnet --skip-install --server-ip 192.168.1.99
```

You can also change it inside TEBEOS (login screen → *Terminal*, or the client
manager dialog), which writes the same files.

---

## 5. Troubleshooting

**"The server is not reachable on port 5454"**
Firewall on the Windows PC, wrong IP, or the TEBEOS server is not running there.
Test with the `/dev/tcp` one-liner above.

**TEBEOS exits immediately / "An invalid IP address was specified"**
The terminal address could not be read from `Ayarlar.ini`. Re-run:

```bash
./install-tebeos-client.sh --skip-deps --skip-dotnet --skip-install --server-ip <IP>
```

**TEBEOS won't accept the password it remembered ("Beni Hatırla"), especially with Turkish letters**
`Ayarlar.ini` is written through the Windows *ANSI* API, and Wine decides that
codepage from your **host locale**. With an `en_US`/`C` locale the codepage is 1252,
which cannot represent `ğ`, `ş`, `İ` — Windows' best-fit mapping quietly stores `gü`
where you typed `çığ`. Run TEBEOS under a Turkish locale instead; the launcher already
does this (`LC_ALL=tr_TR.UTF-8`). If your system has no `tr_TR.UTF-8`, install it:

```bash
sudo localedef -i tr_TR -f UTF-8 tr_TR.UTF-8      # or: sudo locale-gen tr_TR.UTF-8
```

**Money amounts show a box instead of the ₺ sign**
Neither Wine's bundled Tahoma/Arial nor the (XP-era) fonts that `winetricks
corefonts tahoma` installs contain the Turkish lira sign. The installer grafts it in
— into Wine's own files (from `fonts/`) and, with `patch-wine-fonts.py` + fontTools,
into the real fonts inside the prefix. Re-run it and restart TEBEOS:

```bash
./install-tebeos-client.sh --skip-deps --skip-dotnet --skip-install --server-ip <IP>
```

If it warns that fontTools is unavailable, install it once and re-run:
`python3 -m pip install --user --break-system-packages fonttools`.

**The Medula window opens but stays blank**
That pane is a **Microsoft Web Browser (Internet Explorer) control**; Wine renders it
with **MSHTML + wine-gecko**, so both of these have to be right (the installer does
them automatically — re-run it):

* wine-gecko installed, in the version this Wine build asks for;
* MSHTML registered **for 32-bit programs** —
  `wine 'C:\windows\syswow64\regsvr32.exe' /s 'C:\windows\syswow64\mshtml.dll'` —
  and `mshtml` **not** disabled in `WINEDLLOVERRIDES`.

See bug #9 and §10.1 of AI-HANDOFF.md. The engine you end up with is Gecko
(Firefox's), so a page may occasionally render differently than under real IE —
compare in a normal browser if something looks odd.

**The Medula page loads, the fields fill, but I cannot type the captcha**
That was bug #11: the SGK captcha field has `onkeypress="onlyNumbers();"` and Wine's
MSHTML reported `keyCode = 0` for keypress events (Gecko fills `keyCode` only for
keydown/keyup; IE puts the character code there, and Firefox/Chrome only started
mirroring `charCode` → `keyCode` in 2018 — Wine's engine is Gecko 2.47 = Firefox 47).
The page's filter `if (event.keyCode < 48 || event.keyCode > 57) event.returnValue =
false;` therefore discarded every character.

The installer now applies a patched `mshtml.dll` from `wine-patch/` (built from Wine
11.18 with that one fix), so the captcha is typable. If a **different** Wine version
is installed the installer skips it and says so — rebuild with the recipe in
`wine-patch/README.md`, or delete `wine-patch/mshtml` to disable it deliberately.

**TEBEOS exits immediately and Wine complains about `mscoree.dll ... not found`**
The .NET loader was disabled by a bad `WINEDLLOVERRIDES` in the launcher. It must be
`mscoree=n;mshtml=d` — `mscoree,mshtml=` (empty) means *disabled*, and an .NET app
cannot start without it. Re-run the script to regenerate the launcher:

```bash
./install-tebeos-client.sh --skip-deps --skip-dotnet --skip-install --server-ip <IP>
```

**Wine says it cannot run 32-bit programs**
TEBEOS is a 32-bit program. On Debian/Ubuntu:

```bash
sudo dpkg --add-architecture i386
sudo apt-get update && sudo apt-get install wine32
```

or force the portable build: `--wine download`. The script checks this itself with
`wine 'C:\windows\syswow64\cmd.exe' /c "echo ok32"` — the right output is `ok32`.

**The login window appears but login fails**
The database may have no accounts for you yet, or the server's data has not been
provisioned. Check with the main computer's administrator / your TEBEOS vendor.

**Everything is slow / the app hangs at a splash screen**
Give it time on first start. Logs:
`~/.local/share/tebeos/install.log`, `~/.local/share/tebeos/testlaunch.log`, and
`.../TebeosPlusClient/app/log/`.

**Uninstall**

```bash
rm -rf ~/.wine-tebeos ~/tebeos ~/.local/share/tebeos
rm -f ~/.local/share/applications/tebeos-client.desktop ~/Desktop/TEBEOS-client.desktop
```

---

## 6. Privacy and downloads

* The database holds customer and prescription data — treat it as sensitive
  personal data.
* Wine and the .NET installer are downloaded from the internet; the script prints
  every URL it uses. If your pharmacy PC must stay offline, mirror those files and
  pass them with `--installer` / `--wine-url`.

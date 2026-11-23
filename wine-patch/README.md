# Patched Wine MSHTML — makes the Medula captcha typable (bug #11)

## Why this exists

Wine's MSHTML hands JavaScript the IE-style global `event` object, but returns
**`event.keyCode == 0` for `keypress` events** (Gecko fills `keyCode` only for
`keydown`/`keyup`; on `keypress` the character is in `charCode`). IE puts the
character code in `keyCode`, and Firefox/Chrome only started mirroring
`charCode` → `keyCode` in 2018 — Wine's engine is `wine-gecko 2.47.4`, i.e.
Firefox 47 (2016).

SGK's Medula login page filters its captcha ("Güvenlik Anahtarı") field with:

```html
<input ... onkeypress="onlyNumbers();" maxlength="6">
```
```js
// /eczane/scripts/onlyNumbers.js
function onlyNumbers() { if (event.keyCode < 48 || event.keyCode > 57) event.returnValue = false; }
```

With `keyCode == 0`, `0 < 48` is true, so the handler cancels **every** keystroke:
the captcha can never be filled and the Medula login can never be completed.
Everything else in TEBEOS works — the app itself fills the user name, the password
and ticks the KVKK checkbox.

Measured with a minimal page in `wine iexplore`: a field with that handler swallows
input, the identical field without it accepts input, and a handler that reports what
it sees prints `typeof event=object  keyCode=0`.

This is Wine bug material, not a TEBEOS bug.

## What is in here

`mshtml.dll` for **i386-windows** (32-bit — the one TEBEOS uses, it is an x86 app)
and **x86_64-windows**, built from the **vanilla Wine 11.18** source with a single
change in `dlls/mshtml/htmlevent.c` (`DOMKeyboardEvent_get_keyCode()`): if Gecko
reports `keyCode` 0 and the event is a keypress, use the character code.

The change is in `wine-patch/mshtml-keypress-keycode.patch`
(`patch -p1` inside the Wine source tree).

## License

Wine is **LGPL-2.1-or-later**. These are modified Wine binaries, so the LGPL applies;
the corresponding source is upstream Wine plus the patch above
(<https://dl.winehq.org/wine/source/>).

## How these were built (no root needed)

```sh
# portable cross-compiler: has i686 and x86_64 targets, unlike Arch's mingw-w64-gcc
curl -LO https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-msvcrt-ubuntu-22.04-x86_64.tar.xz
tar xf llvm-mingw-20260922-msvcrt-ubuntu-22.04-x86_64.tar.xz
export PATH="$PWD/llvm-mingw-20260922-msvcrt-ubuntu-22.04-x86_64/bin:$PATH"

curl -LO https://dl.winehq.org/wine/source/11.x/wine-11.18.tar.xz
tar xf wine-11.18.tar.xz && cd wine-11.18
patch -p1 < ../wine-patch/mshtml-keypress-keycode.patch
./configure --with-mingw=llvm-mingw --enable-archs=i386,x86_64 --disable-tests
make -j"$(nproc)" dlls/mshtml/i386-windows/mshtml.dll dlls/mshtml/x86_64-windows/mshtml.dll
```

Takes about 3 minutes to build once the toolchain is unpacked.

## Installing

`install-tebeos-client.sh` does it automatically. By hand:

```sh
cp wine-patch/mshtml/i386-windows/mshtml.dll   <wine>/lib/wine/i386-windows/mshtml.dll
cp wine-patch/mshtml/x86_64-windows/mshtml.dll <wine>/lib/wine/x86_64-windows/mshtml.dll
```

The installer/script keeps the original DLLs next to them as `mshtml.dll.orig`, so
`mv mshtml.dll.orig mshtml.dll` reverts.

**Close TEBEOS first** — Wine locks the modules of running programs.

## After a Wine upgrade

These DLLs are built for **Wine 11.18**. A different Wine build has its own
`mshtml.dll`; the installer skips the patch when the version does not match. Either
rebuild with the recipe above, or check whether upstream Wine has since implemented
the IE keypress `keyCode` behaviour (in which case the patch can be dropped).

## Upstream

Worth filing at <https://bugs.winehq.org/> (component `mshtml`), with the minimal
page and the `keyCode=0` measurement. The same file also has a separate bug:
`DOMKeyboardEvent_get_charCode()` calls `nsIDOMKeyEvent_GetKeyCode` instead of
`GetCharCode`.

#!/usr/bin/env bash
# build.sh -- build the shim COM DLL + test harnesses with llvm-mingw (32-bit and 64-bit).
set -euo pipefail

cd "$(dirname "$0")"

M=$(ls -d "$HOME"/toolchain/llvm-mingw-* "$HOME"/Projects/TebeosOnLinux/toolchain/llvm-mingw-* 2>/dev/null | head -1 || true)
echo "toolchain: $M"

# Common generated-stub object is arch-independent C; compile per-arch.
COMMON="shim.c dispmap.c dispwrap.c stubs.c docobj.c element.c window.c page.c jsprop.c rcache.c render.c input.c log.c util.c"
CDP="cdp.c json.c"
# render.c (screencast pump + GDI blit) and input.c (CDP Input) are compiled into the DLL
# for Milestone 6; StretchDIBits/GdiFlush need gdi32, ReleaseBindInfo (POST of a moniker
# navigation) needs urlmon.
DLL_LIBS="-lole32 -loleaut32 -luuid -luser32 -lgdi32 -lws2_32 -lurlmon -lcomdlg32 -lwinspool -lshell32"

build_arch () {
    local arch="$1" out="$2"
    local gcc="$M/bin/${arch}-w64-mingw32-gcc"
    local cflags="-O2 -Wall -Wno-unused-parameter -Wno-unused-function -D_WIN32_WINNT=0x0601"
    # DLL
    "$gcc" $cflags -shared -o "$out.dll" $COMMON $CDP shim.def \
        $DLL_LIBS || { echo "$arch dll FAIL"; exit 1; }
    echo "built $out.dll"
    # probe
    "$gcc" $cflags -o "$out-probe.exe" probe.c \
        -lole32 -loleaut32 -luuid -luser32 -lurlmon || { echo "$arch probe FAIL"; exit 1; }
    echo "built $out-probe.exe"
    # hosttest (minimal DocObject container harness)
    "$gcc" $cflags -o "$out-hosttest.exe" hosttest.c \
        -lole32 -loleaut32 -luuid -luser32 -lurlmon || { echo "$arch hosttest FAIL"; exit 1; }
    echo "built $out-hosttest.exe"
    # hostrender (Milestone 6: host + render + input integration vs real Chromium)
    "$gcc" $cflags -o "$out-hostrender.exe" hostrender.c \
        -lole32 -loleaut32 -luuid -luser32 -lurlmon || { echo "$arch hostrender FAIL"; exit 1; }
    echo "built $out-hostrender.exe"
    # eventtest (attachEvent -> DOM event -> CDP binding -> sink Invoke(0) loop)
    "$gcc" $cflags -o "$out-eventtest.exe" eventtest.c \
        -lole32 -loleaut32 -luuid -luser32 -lurlmon || { echo "$arch eventtest FAIL"; exit 1; }
    echo "built $out-eventtest.exe"
    # cdptest (CDP WebSocket+JSON transport self-test, mock server, no Chromium)
    "$gcc" $cflags -o "$out-cdptest.exe" cdptest.c json.c cdp.c \
        -lws2_32 || { echo "$arch cdptest FAIL"; exit 1; }
    echo "built $out-cdptest.exe"
    # livetest (CDP client against a real Chromium debugging port)
    "$gcc" $cflags -o "$out-livetest.exe" livetest.c json.c cdp.c \
        -lws2_32 || { echo "$arch livetest FAIL"; exit 1; }
    echo "built $out-livetest.exe"
    # rendertest (screencast session + async frame pump vs real Chromium)
    "$gcc" $cflags -o "$out-rendertest.exe" rendertest.c json.c cdp.c \
        -lws2_32 || { echo "$arch rendertest FAIL"; exit 1; }
    echo "built $out-rendertest.exe"
    # blittest (JPEG decode via stb_image + GDI blit round-trip vs real Chromium)
    "$gcc" $cflags -o "$out-blittest.exe" blittest.c json.c cdp.c \
        -lgdi32 -luser32 -lws2_32 || { echo "$arch blittest FAIL"; exit 1; }
    echo "built $out-blittest.exe"
    # renderlive (continuous screencast render worker + blit into an HWND)
    "$gcc" $cflags -o "$out-renderlive.exe" renderlive.c render.c log.c json.c cdp.c \
        -lgdi32 -luser32 -lws2_32 || { echo "$arch renderlive FAIL"; exit 1; }
    echo "built $out-renderlive.exe"
    # inputtest (CDP Input forwarding: synthetic mouse/key vs real Chromium)
    "$gcc" $cflags -o "$out-inputtest.exe" inputtest.c input.c json.c cdp.c \
        -lws2_32 || { echo "$arch inputtest FAIL"; exit 1; }
    echo "built $out-inputtest.exe"
    # domtest (CDP-backed DOM COM surface against real Chromium + the shim DLL)
    "$gcc" $cflags -o "$out-domtest.exe" domtest.c \
        -lole32 -loleaut32 -luuid -luser32 || { echo "$arch domtest FAIL"; exit 1; }
    echo "built $out-domtest.exe"
    # wbtest (the shim behind Wine's real WebBrowser control, against wbserver.py)
    "$gcc" $cflags -o "$out-wbtest.exe" wbtest.c \
        -lole32 -loleaut32 -luuid -luser32 -lgdi32 || { echo "$arch wbtest FAIL"; exit 1; }
    echo "built $out-wbtest.exe"
}

build_arch i686   shim32
build_arch x86_64 shim64

echo "done."

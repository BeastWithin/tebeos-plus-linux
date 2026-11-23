#!/usr/bin/env bash
# runtests.sh -- run every shim harness on both arches under Wine.
# Starts its own headless Chromium on $PORT with a throwaway profile (never touches the
# user's Chromium profile) and kills it afterwards.
#   bash runtests.sh            # all tests
#   bash runtests.sh domtest    # only tests whose name matches
set -uo pipefail
cd "$(dirname "$0")"

WINE="$HOME/.local/share/tebeos/wine/wine-11.18-staging-amd64-wow64/bin/wine"
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-tebeos}"
export WINEDEBUG="${WINEDEBUG:--all}"
PORT="${PORT:-9222}"
FILTER="${1:-}"

PROFILE=$(mktemp -d /tmp/shimtest-chromium.XXXXXX)
chromium --headless=new --remote-debugging-port="$PORT" --user-data-dir="$PROFILE" \
    --no-first-run --no-default-browser-check about:blank >/dev/null 2>&1 &
CHROME=$!
# Documents with no harness-supplied tab (probe, hosttest) make the shim start its own
# Chromium; give it a port and profile of its own so it can be stopped afterwards.
export TEBEOS_CDP_PORT=$((PORT + 1)) TEBEOS_CDP_PROFILE="$PROFILE-shim"
# wbtest's pages
WBPORT=$((PORT + 2))
python3 wbserver.py "$WBPORT" &
WBSERVER=$!
cleanup () {
  kill $WBSERVER 2>/dev/null; wait $WBSERVER 2>/dev/null
  kill $CHROME 2>/dev/null; wait $CHROME 2>/dev/null
  pkill -f "[u]ser-data-dir=$TEBEOS_CDP_PROFILE" 2>/dev/null; sleep 0.5
  rm -rf "$PROFILE" "$TEBEOS_CDP_PROFILE"
}
trap cleanup EXIT
for _ in $(seq 50); do curl -s "http://127.0.0.1:$PORT/json/version" >/dev/null && break; sleep 0.2; done

# name | args after the exe (DLL name is substituted per arch)
TESTS=(
  "probe|DLL"
  "hosttest|DLL"
  "cdptest|"
  "livetest|$PORT"
  "rendertest|$PORT"
  "blittest|$PORT"
  "renderlive|$PORT"
  "inputtest|$PORT"
  "domtest|$PORT"
  "hostrender|DLL $PORT"
  "eventtest|DLL $PORT"
  "wbtest|DLL $WBPORT"
)

pass=0; fail=0; failed=()
for arch in 32 64; do
  for t in "${TESTS[@]}"; do
    name=${t%%|*}; args=${t#*|}
    [[ -n "$FILTER" && "$name" != *"$FILTER"* ]] && continue
    args=${args//DLL/shim$arch.dll}
    log="/tmp/shimtest-$name-$arch.log"
    # shellcheck disable=SC2086
    timeout 90 "$WINE" "shim$arch-$name.exe" $args >"$log" 2>&1
    rc=$?
    if [[ $rc -eq 0 ]] && grep -q "PASS" "$log" && ! grep -q "FAIL" "$log"; then
      echo "PASS  $name ($arch)"; pass=$((pass+1))
    else
      echo "FAIL  $name ($arch) rc=$rc  -> $log"; tail -5 "$log" | sed 's/^/      /'
      fail=$((fail+1)); failed+=("$name/$arch")
    fi
  done
done
# a navigation cancelled in BeforeNavigate2 must never reach the server
if [[ -z "$FILTER" || "wbtest" == *"$FILTER"* ]]; then
  hits=$(curl -s "http://127.0.0.1:$WBPORT/hits")
  if ! grep -q "^/index " <<<"$hits"; then
    echo "FAIL  wbtest: nothing reached the server"; fail=$((fail+1)); failed+=("wbtest/server")
  elif grep -q "^/cancelme " <<<"$hits"; then
    echo "FAIL  wbtest: the server saw /cancelme"; fail=$((fail+1)); failed+=("wbtest/cancel")
  else
    echo "PASS  wbtest: cancelled navigation never reached the server"; pass=$((pass+1))
  fi
fi
echo "== $pass passed, $fail failed ${failed[*]:-}"
[[ $fail -eq 0 ]]

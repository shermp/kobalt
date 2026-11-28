#!/bin/sh
set -eu

run=$(mktemp -d)
pid=
cleanup() {
    if [ -n "$pid" ]; then
        kill "$pid" 2>/dev/null || :
        wait "$pid" 2>/dev/null || :
    fi
    rm -f "$run/address" "$run/renderer.sock"
    rmdir "$run"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
export G_DEBUG=fatal-criticals
"$1" "$run" >"$run/address" &
pid=$!
for attempt in $(seq 1 100); do
    [ ! -s "$run/address" ] || break
    kill -0 "$pid"
    sleep .02
done
"$2" "$(cat "$run/address")"
wait "$pid"
pid=

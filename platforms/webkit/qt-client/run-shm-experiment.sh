#!/bin/sh
# Run inside an offline container with an init process. The wrappers must exec
# the renderer and client, so the supervisor can identify the renderer PID.
set -eu
renderer=$1
client=$2
mode=$3
tmpfs=$4
case "$mode" in fd|named|crash) ;; *) exit 2 ;; esac
run=$(mktemp -d "$tmpfs/kobalt-shm.XXXXXX")
pid=
cleanup() {
    if [ -n "$pid" ]; then
        kill "$pid" 2>/dev/null || :
        wait "$pid" 2>/dev/null || :
    fi
    rm -f "$run/address" "$run/renderer.sock" "$run"/frame-??????
    rmdir "$run"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
if [ "$mode" = fd ]; then
    "$renderer" "$run" >"$run/address" &
else
    "$renderer" --shm-experiment "$run" >"$run/address" &
fi
pid=$!
for attempt in $(seq 1 200); do
    [ ! -s "$run/address" ] || break
    kill -0 "$pid"
    sleep .02
done
address=$(cat "$run/address")
client_status=0
case "$mode" in
    fd) "$client" "$address" || client_status=$? ;;
    named) "$client" "$address" "$run" || client_status=$? ;;
    crash) "$client" "$address" "$run" "$pid" || client_status=$? ;;
esac
if [ "$client_status" -ne 0 ] && [ "$mode" != crash ]; then
    exit "$client_status"
fi
if [ "$client_status" -ne 0 ] && kill -0 "$pid" 2>/dev/null; then
    exit "$client_status"
fi
status=0
wait "$pid" || status=$?
pid=
set -- "$run"/frame-??????
if [ "$mode" = crash ]; then
    test "$status" -eq 137
    test "$#" -eq 1 && test -f "$1"
    echo "One crash orphan reclaimed by supervisor"
else
    test "$status" -eq 0
    test ! -e "$1"
    test ! -e "$run/renderer.sock"
fi
exit "$client_status"

#!/bin/sh
# Run only from the isolated device install; no persistent writes outside it.
set -eu
root=/mnt/onboard/.adds/kobalt-webkit
umask 077
[ -d "$root/tests" ] && [ -d "$root/libexec" ] && [ -d "$root/lib" ] || exit 1
# mkdir is atomic and refuses an existing or symlinked run directory.
run=/tmp/kobalt-webkit-m2-$$
mkdir "$run" || exit 1
cleanup() { rm -rf "$run"; }
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
unset LD_PRELOAD
mkdir "$run/home" "$run/cache" "$run/config" "$run/data" "$run/runtime" "$run/gio"
export HOME="$run/home" XDG_CACHE_HOME="$run/cache" XDG_CONFIG_HOME="$run/config"
export XDG_DATA_HOME="$run/data" XDG_RUNTIME_DIR="$run/runtime" TMPDIR="$run"
export GIO_MODULE_DIR="$run/gio"
export WEBKIT_EXEC_PATH="$root/libexec" WEBKIT_INJECTED_BUNDLE_PATH="$root/lib"
export LD_LIBRARY_PATH="$root/lib"
export LIBGL_DRIVERS_PATH=/nonexistent __EGL_VENDOR_LIBRARY_FILENAMES=/nonexistent
"$root/tests/wpe-snapshot-fixture"

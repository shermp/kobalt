#!/bin/sh
# Run from a staged RV0 bundle. Each experiment starts a fresh renderer and profile.
set -eu
export LC_ALL=C
umask 077
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "Usage: $0 NEW-LOG-DIRECTORY [--document-viewer]" >&2
    exit 2
fi
cache=${2:-}
case "$cache" in ''|--document-viewer) ;; *) exit 2 ;; esac
bundle=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
mkdir "$1"
logs=$(CDPATH= cd -- "$1" && pwd)
run=$(mktemp -d /tmp/kobalt-rv0.XXXXXX)
trap 'find "$run" -type f -exec rm -f {} +; find "$run" -depth -type d -exec rmdir {} \;' EXIT
trap 'exit 1' HUP INT TERM

uname -a >"$logs/kernel.txt"
cat /proc/meminfo >"$logs/before-meminfo.txt"
cat /proc/vmstat >"$logs/before-vmstat.txt"
dmesg >"$logs/before-dmesg.txt" 2>&1 || true
(
    cd "$bundle"
    sha256sum -c SHA256SUMS
) >"$logs/manifest-check.txt"

# Rotate the order across repetitions to spread warming and load effects.
for trial in 1 2 3; do
    case "$trial" in
        1) modes='one two-independent two-related three-related' ;;
        2) modes='two-related three-related one two-independent' ;;
        3) modes='three-related two-related two-independent one' ;;
    esac
    for workload in short long resources images; do
        for mode in $modes; do
            profile=$run/$workload-$mode-$trial
            mkdir "$profile"
            export XDG_RUNTIME_DIR="$profile" XDG_CACHE_HOME="$profile"
            export XDG_DATA_HOME="$profile" XDG_CONFIG_HOME="$profile"
            log=$logs/$workload-$mode-$trial.log
            if "$bundle/run" "$mode" "$workload" "$bundle/fixtures" ${cache:+"$cache"} >"$log" 2>&1; then
                printf '%s %s %s ok\n' "$trial" "$workload" "$mode"
            else
                printf '%s %s %s failed. See %s\n' "$trial" "$workload" "$mode" "$log" >&2
                exit 1
            fi
        done
    done
done
cat /proc/meminfo >"$logs/after-meminfo.txt"
cat /proc/vmstat >"$logs/after-vmstat.txt"
dmesg >"$logs/after-dmesg.txt" 2>&1 || true

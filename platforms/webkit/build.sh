#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
engine=${CONTAINER_ENGINE:-podman}
command -v cmake >/dev/null || { echo "CMake 3.22 or newer is required; see doc/building.md" >&2; exit 1; }
command -v "$engine" >/dev/null || { echo "Missing container engine: $engine" >&2; exit 1; }
image=$(cmake -DPRINT_IMAGE=ON -P "$root/platforms/webkit/image.cmake")
"$engine" image inspect "$image" >/dev/null || {
    echo "Missing prepared WebKit image $image; see doc/webkit-wpe/m1.md" >&2
    exit 1
}
cache=$(cmake -P "$root/dependencies/cache.cmake")
mkdir -p "$cache" "$root/build/webkit"
container_id=
container_record=
cleanup() {
    if [[ -n "$container_id" ]]; then
        if "$engine" rm -f "$container_id" >/dev/null; then
            cmake -DPATHS="$container_record" -P "$root/platforms/cleanup.cmake"
        else
            echo "Container cleanup failed; owned ID retained in $container_record" >&2
            return 1
        fi
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
container_id=$("$engine" create --network none --read-only --cpus "${KOBALT_WEBKIT_JOBS:-4}" \
    -v "$root:/work:rw" -v "$cache:/dependency-cache:rw" --tmpfs /tmp:rw,size=4g \
    -e KOBALT_DEPENDENCY_CACHE=/dependency-cache -e KOBALT_WEBKIT_BUILD_IMAGE="$image" \
    -e KOBALT_WEBKIT_JOBS="${KOBALT_WEBKIT_JOBS:-4}" \
    --entrypoint /bin/bash "$image" /work/platforms/webkit/in-container.sh)
container_record=$(mktemp "$root/build/webkit/container-XXXXXX.id")
printf '%s\n' "$container_id" > "$container_record"
"$engine" start -a "$container_id"
status=$("$engine" inspect "$container_id" --format '{{.State.ExitCode}}')
exit "$status"

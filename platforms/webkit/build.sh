#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd -P)
engine=${CONTAINER_ENGINE:-podman}
command -v cmake >/dev/null || { echo "CMake 3.22 or newer is required; see doc/building.md" >&2; exit 1; }
command -v "$engine" >/dev/null || { echo "Missing container engine: $engine" >&2; exit 1; }
jobs=${KOBALT_WEBKIT_JOBS:-4}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "KOBALT_WEBKIT_JOBS must be positive" >&2; exit 2; }
cpus=${KOBALT_DOCKER_CPUS:-$jobs}
[[ "$cpus" =~ ^[0-9]+(\.[0-9]+)?$ && "$cpus" =~ [1-9] ]] || { echo "KOBALT_DOCKER_CPUS must be positive" >&2; exit 2; }
if [[ -z "${KOBALT_LOAD_LOCK_HELD:-}" ]]; then
    exec 3< <(cmake "-DROOT=$root" "-DOWNER=$$" -P "$root/platforms/load-lock.cmake")
    read -r registered <&3 && [[ "$registered" == registered ]] || { echo "Cannot register the WebKit build" >&2; exit 1; }
fi
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
source_mount=()
source_bundle=${KOBALT_WEBKIT_SOURCE_BUNDLE:-}
if [[ -z "$source_bundle" && -f "$root/source-info.json" && -f "$root/../manifest.json" ]]; then
    source_bundle=$(cd -- "$root/.." && pwd)
fi
if [[ -n "$source_bundle" ]]; then
    [[ -d "$source_bundle" ]] || { echo "Missing WebKit source bundle: $source_bundle" >&2; exit 1; }
    source_bundle=$(cd -- "$source_bundle" && pwd -P)
    source_mount=(-v "$source_bundle:/source-bundle:ro" -e KOBALT_WEBKIT_SOURCE_BUNDLE=/source-bundle)
fi
container_id=$("$engine" create --network none --read-only --cpus "$cpus" \
    -v "$root:/work:rw" -v "$cache:/dependency-cache:rw" --tmpfs /tmp:rw,size=4g \
    -e KOBALT_DEPENDENCY_CACHE=/dependency-cache -e KOBALT_WEBKIT_BUILD_IMAGE="$image" \
    -e KOBALT_WEBKIT_JOBS="$jobs" \
    "${source_mount[@]}" --entrypoint /bin/bash "$image" /work/platforms/webkit/in-container.sh)
container_record=$(mktemp "$root/build/webkit/container-XXXXXX.id")
printf '%s\n' "$container_id" > "$container_record"
"$engine" start -a "$container_id"
status=$("$engine" inspect "$container_id" --format '{{.State.ExitCode}}')
exit "$status"

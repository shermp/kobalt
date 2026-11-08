#!/usr/bin/env bash
set -euo pipefail
cmake -DACTION=IMPORT -P /work/platforms/webkit/sources.cmake
cmake -P /work/dependencies/webkit/prepare.cmake
export PATH="$(cat /work/build/webkit/cmake-host.txt)/bin:$PATH"
export KOBALT_WEBKIT_TOOLCHAIN="$(cat /work/build/webkit/kobov5.txt)"
cmake -DKOBALT_WEBKIT_BUILD_IMAGE="$KOBALT_WEBKIT_BUILD_IMAGE" \
    -DKOBALT_WEBKIT_JOBS="$KOBALT_WEBKIT_JOBS" \
    -DKOBALT_WEBKIT_OUTPUT_FILE=/work/build/webkit/dependency-entry.txt \
    -P /work/dependencies/webkit/build.cmake
prefix="$(cat /work/build/webkit/dependency-entry.txt)/install/opt/kobalt-webkit"
cd /work/platforms/webkit
cmake --preset armhf -DKOBALT_WEBKIT_PREFIX="$prefix"
cmake --build --preset armhf --parallel "$KOBALT_WEBKIT_JOBS"
cmake -DKOBALT_WEBKIT_PREFIX="$prefix" -P /work/platforms/webkit/package.cmake

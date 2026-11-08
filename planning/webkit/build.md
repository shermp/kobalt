# Reproducing the M0 investigation

Run from the feature worktree on the verified x86-64 Ubuntu 24.04 WSL2 host. These are standalone dependency/probe builds, not Kobalt application builds. Use Podman and the existing local simulator checkout; do not rebuild Qt or replace the application SDK. The Python files below are Device-API runtime diagnostics, not application build tooling.

## Inputs and prepared images

| Image | Measured local ID |
| --- | --- |
| Native WebKit tools | `662599b1c68c64b57e6a3d8f35994592a3f4ac6042b3a66c82651db1f36428d7` |
| ARM WebKit tools/sysroot | `32211887c07ac767d606cca9df82250a24b5c2f1938c48f687aaf8ae353ff7b2` |
| Simulator runtime after normal prepare | `2dc8b8a34e6c326cdc3ade9deedf8574e624e153c3937bd1961cd87d105bf9d6` |
| Existing simulator Qt6 builder | `6ec49c4517cf30c3ff4544d40a0cb160de73a846f91fb1b08bc6fd10d16a429a` |

The WebKit tags are `localhost/kobalt-webkit-m0-native:dev` and `localhost/kobalt-webkit-m0-arm:dev`. Inspect their IDs before reuse. Their [Containerfiles](../../dependencies/webkit-m0/Containerfile.arm) pin Ubuntu 22.04 and 24.04 base manifests, but use moving apt repositories. Exact package inventories are in [evidence/](evidence/arm-packages.tsv). Recreating these local images later is not byte-for-byte reproducibility; M1 needs stronger package/source locks. If the tools images are absent, their build-only setup is:

```sh
podman build -t localhost/kobalt-webkit-m0-native:dev \
  -f dependencies/webkit-m0/Containerfile.native .
podman build -t localhost/kobalt-webkit-m0-arm:dev \
  -f dependencies/webkit-m0/Containerfile.arm .
```

Only those setup builds and uncached archive downloads need external network access. Probe and firmware runs keep network restrictions. Normal simulator preparation can refresh its own stale runtime image/plugins; it reused the installed Qt build in this investigation.

Set paths explicitly. The shared dependency cache belongs to the main checkout, never another worktree's output:

```sh
task_root="$PWD"
task_cache=/home/sherman/kobo-dev/kobalt/build/dependency-cache
```

[fetch.cmake](../../dependencies/webkit-m0/fetch.cmake) verifies WebKit/toolchain archives using the existing cache helpers. [fetch-host-tools.cmake](../../dependencies/webkit-m0/fetch-host-tools.cmake) verifies and extracts CMake 3.31.8. With the archives already cached, run offline:

```sh
podman run --rm --network none --read-only \
  -v "$task_root:/work:rw" -v "$task_cache:/dependency-cache:rw" \
  --entrypoint /bin/sh localhost/kobalt-webkit-m0-native:dev -c '
    export KOBALT_DEPENDENCY_CACHE=/dependency-cache
    cmake -P /work/dependencies/webkit-m0/fetch.cmake &&
    cmake -P /work/dependencies/webkit-m0/fetch-host-tools.cmake'
```

For missing archives, use these same helpers with build-time network access; they verify the exact [source](../../dependencies/webkit-m0/sources.json) and [host-tool](../../dependencies/webkit-m0/host-tools.json) hashes. Extract `downloads/kobov5-a9eba0aaf17e5cc94fdabb46c2ae06d9bb41936b869ea6479d325c9f79b14a64.tar` into this worktree's `build/webkit-m0/toolchain/`. Its root is `x-tools/arm-kobov5-linux-gnueabihf`. Do not replace or re-extract over an existing toolchain. The CMake archive helper's `.tar` filename retains the original compressed archive bytes.

## ABI prerequisites

The [prototype CMake project](prototype/CMakeLists.txt) builds `abi-probe`, `abi-library` and `sandbox-probe` without WebKit by default. Configure with the existing builder's CMake and `dependencies/webkit-m0/kobov5.cmake`; no Qt target is involved. Private `libstdc++.so.6.0.33` and `libgcc_s.so.1` come from the toolchain's `arm-kobov5-linux-gnueabihf/sysroot/lib/`.

The small ABI probe must run against a firmware 6 root before WebKit. For a saved device volume, mount the root read-only and do not start/reset that device. The measured root was `virtualnickel-system-libra_colour_6_0_274403:/firmware:ro`. Run through existing `vn-offline` and `qemu-arm -cpu max -L /firmware/rootfs`, passing guest `QEMU_LD_PREFIX=/firmware/rootfs` and a helper-only library path. Expected: firmware glibc 2.35, Unix IPC/shared-memory success, `ARM_CHILD_OK`, exit 0. Stock libstdc++ instead rejects `GLIBCXX_3.4.30`. Detailed results and namespace/seccomp comparisons are in [toolchain.md](toolchain.md) and [virtualnickel.md](virtualnickel.md).

## Build WebKit

The recipes apply both candidate patches through the existing patch helper and use the existing dependency-set lock/publication mechanism. Builds run with `-O1 -DNDEBUG`. Never change a cache entry with a `complete` marker. Change an input/recipe to obtain a new key.

The native recipe requires CMake >= 3.24; the ARM allocator requires >= 3.25. ARM uses the pinned host CMake 3.31.8. Record actual image IDs rather than trusting mutable tags:

```sh
native_image=$(podman image inspect localhost/kobalt-webkit-m0-native:dev --format '{{.Id}}')
arm_image=$(podman image inspect localhost/kobalt-webkit-m0-arm:dev --format '{{.Id}}')

podman run --rm --network none --read-only --cpus 4 \
  -v "$task_root:/work:rw" -v "$task_cache:/dependency-cache:rw" \
  --tmpfs /tmp:rw,size=4g \
  -e KOBALT_WEBKIT_BUILD_IMAGE="$native_image" \
  --entrypoint /bin/sh localhost/kobalt-webkit-m0-native:dev -c '
    KOBALT_DEPENDENCY_CACHE=/dependency-cache cmake \
      -DKOBALT_WEBKIT_BUILD_IMAGE="$KOBALT_WEBKIT_BUILD_IMAGE" \
      -DKOBALT_WEBKIT_LAZY_EGL=ON \
      -DKOBALT_WEBKIT_OUTPUT_FILE=/work/build/webkit-m0/native-entry.txt \
      -P /work/dependencies/webkit-m0/build-native.cmake'

podman run --rm --network none --read-only --cpus 8 \
  -v "$task_root:/work:rw" -v "$task_cache:/dependency-cache:rw" \
  --tmpfs /tmp:rw,size=4g \
  -e KOBALT_WEBKIT_BUILD_IMAGE="$arm_image" \
  --entrypoint /bin/sh localhost/kobalt-webkit-m0-arm:dev -c '
    export PATH=/work/build/webkit-m0/host-tools/cmake-3.31.8-linux-x86_64/bin:$PATH
    export KOBALT_WEBKIT_TOOLCHAIN=/work/build/webkit-m0/toolchain/x-tools/arm-kobov5-linux-gnueabihf
    KOBALT_DEPENDENCY_CACHE=/dependency-cache cmake \
      -DKOBALT_WEBKIT_BUILD_IMAGE="$KOBALT_WEBKIT_BUILD_IMAGE" \
      -DKOBALT_WEBKIT_OUTPUT_FILE=/work/build/webkit-m0/arm-entry.txt \
      -P /work/dependencies/webkit-m0/build-arm.cmake'
```

These are expensive first builds; use the completed cache when identities match. Status files select `/dependency-cache/<key>` inside containers. Detailed configure/build/install logs live under the selected entry's `project/wpewebkit-build-prefix/src/wpewebkit-build-stamp/`. Compilation uses four jobs for the native recipe and eight for ARM.

The completed ARM key is `webkit-m0-armhf-61ebcc82cd3b82ebd8d4929b49d4c5a4e990c54c7a4e50c12eba45736dbe1fb4`. Its staged prefix is `<entry>/install/instance/artifacts/kobalt-wpe-m0`, with runtime prefix `/instance/artifacts/kobalt-wpe-m0`. The native measured experiment is `webkit-m0-native-b0ff50682b8215ee178a74943bdaf2648bdd9bd85d646406ee712343fb6b83ec`: it was fixed incrementally while incomplete and **remains unpublished**. It must not be marked complete under its earlier identity. The final native recipe above selects a fresh key with both patches included.

## Build and bundle the ARM probe

The installed prefix is imported explicitly so pkg-config cannot incorrectly prepend a cross sysroot to it. All other probe dependencies come from target-only pkg-config/library paths.

```sh
podman run --rm --network none --read-only --cpus 2 \
  -v "$task_root:/work:rw" -v "$task_cache:/dependency-cache:ro" \
  --tmpfs /tmp:rw,size=1g \
  --entrypoint /bin/sh localhost/kobalt-webkit-m0-arm:dev -c '
    export PATH=/work/build/webkit-m0/host-tools/cmake-3.31.8-linux-x86_64/bin:$PATH
    export KOBALT_WEBKIT_TOOLCHAIN=/work/build/webkit-m0/toolchain/x-tools/arm-kobov5-linux-gnueabihf
    arm_entry=$(cat /work/build/webkit-m0/arm-entry.txt)
    arm_prefix="$arm_entry/install/instance/artifacts/kobalt-wpe-m0"
    cmake -GNinja -S /work/planning/webkit/prototype \
      -B /work/build/webkit-m0/render-arm \
      -DCMAKE_TOOLCHAIN_FILE=/work/dependencies/webkit-m0/arm.cmake \
      -DKOBALT_WEBKIT_RENDER_PROBE=ON -DKOBALT_WEBKIT_PREFIX="$arm_prefix" \
      -DCMAKE_BUILD_TYPE=Release &&
    cmake --build /work/build/webkit-m0/render-arm --target wpe-snapshot -j2 &&
    cmake -DKOBALT_WEBKIT_PREFIX="$arm_prefix" \
      -DKOBALT_PROBE_BINARY=/work/build/webkit-m0/render-arm/wpe-snapshot \
      -DKOBALT_BUNDLE_OUTPUT=/work/build/webkit-m0/arm-bundle \
      -P /work/dependencies/webkit-m0/bundle-probe.cmake'
```

Bundling requires a fresh output directory. Clean only an owned previous staging directory with `cmake -DPATHS="$task_root/build/webkit-m0/arm-bundle" -P platforms/cleanup.cmake`. The resolver recursively follows target `DT_NEEDED` entries, excludes the firmware libc/loader family, copies symlink chains, verifies ELF32 ARM, strips only copied output files, and records input hashes. Preserve subprocess and injected-bundle directories. Linkers emitted a transitive libatomic search warning; the bundle includes the compiler's libatomic and firmware loading passed. Do not repair this by modifying the published cache.

Use `readelf -h`, `-A`, `-l`, `-d` and `--version-info` over the whole copied ELF closure. [arm-elf-abi.json](evidence/arm-elf-abi.json) records hard-float ABI, interpreter and ISA attributes, including libgcrypt ARMv8 attributes. The optional Cortex-A7 run below checks the controlled workload on ARMv7. The retained [audit](evidence/arm-bundle-audit.json) contains actual stripped checksums and required version maxima. No host ELF, glibc or loader is in the bundle.

## Native rendering

Configure the same prototype inside the native tools image with `KOBALT_WEBKIT_RENDER_PROBE=ON`, `KOBALT_WEBKIT_PREFIX=<native-entry>/install`, and no cross toolchain. Use the installed library through a helper-only `LD_LIBRARY_PATH` and mount that dependency cache at the same path. The probe's own RPATH is `$ORIGIN/../private` for the ARM bundle.

Run `wpe-snapshot fixture.xhtml output.png 800 1100` inside an offline, read-only native tools container with private writable XDG directories and `/tmp`, no display server, no `/dev/dri` or `/dev/dxg`, and nonexistent `__EGL_VENDOR_LIBRARY_FILENAMES` and `LIBGL_DRIVERS_PATH`. Compile the unchanged local simulator `runtime/offline.c` natively for this Ubuntu environment and put it before the command, as the firmware tests do with `vn-offline`. Do not set the sandbox-disable variable or use the experiment argument for the native proof.

The executed wrappers, negative EGL control, native images and logs are retained under `build/webkit-m0/`. Native runs covered repeated capture, repaint, missing-resource failure, override rejection and cleanup. [rendering.md](rendering.md) records their results. Rebuilding native WebKit again is unnecessary for the reviewed M0 result.

## Run inside firmware 6.x

The diagnostic [run-firmware.py](prototype/run-firmware.py) uses the existing local Device API and Podman. It creates a unique disposable device and always closes/deletes that exact device in `finally`. It uses the authorized per-run sandbox exception; running it does not validate WebKit's internal sandbox. The retained driver is the tested worktree-local driver with checkout location, memory cap and helper CPU model exposed as arguments. The parameterized driver itself also passed the Cortex-A7 run.

```sh
python3 planning/webkit/prototype/run-firmware.py \
  --virtualnickel /home/sherman/kobo-dev/VirtualNickel --memory-mib 768
python3 planning/webkit/prototype/run-firmware.py \
  --virtualnickel /home/sherman/kobo-dev/VirtualNickel --memory-mib 512
# Optional ISA check, with Nickel retaining its ordinary CPU model:
python3 planning/webkit/prototype/run-firmware.py \
  --virtualnickel /home/sherman/kobo-dev/VirtualNickel --memory-mib 768 --cpu cortex-a7
```

It prints a fresh results directory under this worktree's `build/webkit-m0/`, waits for library readiness, copies the bundle into the existing artifact mount, applies one CPU, samples cgroups and descendant PSS/RSS, tests repeated capture/repaint/crash/post-crash startup, verifies Nickel command responsiveness, then removes its instance and volume. [resource-sample.py](prototype/resource-sample.py) is a native sampler and subreaper for only the owned command tree. It reaps adopted QEMU children; no simulator code is patched.

For manual diagnosis, the guest command is `qemu-arm -cpu max -L /firmware/rootfs -U LD_PRELOAD`, with guest-only `QEMU_LD_PREFIX=/firmware/rootfs`, `QEMU_CPU=max`, private `LD_LIBRARY_PATH=<prefix>/private:/firmware/rootfs/usr/lib:/firmware/rootfs/lib`, private XDG directories, `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` and the probe's `--allow-unsandboxed-experiment` argument. Keep the existing native `vn-offline` wrapper and ordinary container network/security policy. No privileged container or unconfined seccomp setting is used.

Measured runs are retained in `build/webkit-m0/firmware-results-supervised/` and `firmware-results-512m/`; the initial cleanup failure remains in `firmware-results/`. The Cortex-A7 results are in `build/webkit-m0/firmware-results-768m-4a6fb3f55e/`. Concise results are in [evidence/](evidence/firmware-512m.json). Screenshots, logs and current incremental outputs are retained; abandoned worktree build outputs can be removed through `platforms/cleanup.cmake`. Never clean saved simulator devices, SDKs or published dependency entries.

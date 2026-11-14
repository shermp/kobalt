# WPE-M0 firmware and architecture feasibility

The FW4 userspace gate passed for a minimal probe. A disposable VirtualNickel Nia booted the kobo7 update at 4.38.23852. With a temporary loader directory made from the original update's libraries, an ARM hard-float probe ran against Kobo glibc 2.19, including an ARM child exec. A C++23 binary built with the candidate toolchain also ran against that loader. The WPE 2.54.2 dependency build and physical Forma inventory remain open for review.

## Revisions and inputs

| Input | Identity |
| --- | --- |
| Selected worktree base | `0b8f3c6014c7bdb8891c7384d85f4f257d0fd3d0`. This is local `develop` at `2a708d19abfbb23aab95fab94451efe8feb001a8` plus the commit containing the plan and draft XML. The published `develop` refs still report `f0befcaffd74910ce1dd472ad5ab9251c38b2bb2`. |
| Blaze | Latest upstream head `8de6ea8127a472378829910bfe59ac05f9571350`, inspected as a Git object. Fork head remains `a4bb4d1cf86acaa404064dca184a0e6708a43b00`. |
| PoC | `feature/webkit-wpe-m4` at `755d1db1cb6a625cb5b6161f71e2d8bab0e9d16a`. Its historical feasibility evidence also appears at `1fac62442b4b036bed94adc7c3f9ea9f617d2f5e`. |
| Simulator | `154ff91578775409b40f6a3e98ff49f11c455a77`, Podman runtime image `sha256:2dc8b8a34e6c326cdc3ade9deedf8574e624e153c3937bd1961cd87d105bf9d6`, QEMU 10.0.13. Offscreen Nia profile, Qt 5.2.1. |
| Kobo firmware archive | kobo7 4.38.23852, SHA-256 `db8915cd12871bc10dd01097c634a6109f0bda611540d189f77f371a44a5564c`, verified locally. The checkout has a Nia hardware profile and no Forma profile. [Kobo update archive](https://ereaderfiles.kobo.com/firmwares/kobo7/Oct2026/kobo-update-4.38.23852.zip) |
| WPE candidate | 2.54.2, release tag resolves to `32717fabcb9dcad9aa00a54ef1f23e41ee64e849`. Cached pristine archive SHA-256 `f3284e19e47fcb5ac8c63c2e1c113dbd65b2423eb8a545f5b345707829004f56`, verified locally against the published digest. [WPE release](https://wpewebkit.org/release/wpewebkit-2.54.2.html) |
| FW4 C++23 candidate | koxtoolchain 2026.08 `kobov4.tar.zst`, verified SHA-256 `42baff346352f1e31de427245adcab39d85be4ba935b72da1605d09e4a15a8ef`. GCC 14.4.0, `arm-kobov4-linux-gnueabihf`, Thumb/Cortex-A9/NEON hard float, glibc 2.19, minimum kernel 4.1. [Release](https://github.com/koreader/koxtoolchain/releases/tag/2026.08), [toolchain configuration](https://github.com/benoit-pierre/crosstool-ng/blob/34844bc8e985ad1ba26b072a5b58264967072e19/samples/arm-kobov4-linux-gnueabihf/crosstool.config) |

## FW4 probe

| Check | Observed result |
| --- | --- |
| Kobo Nickel ELF | Little-endian ELF32, ARMv7-A, EABI5 hard float with VFP arguments, interpreter `/lib/ld-linux-armhf.so.3`. |
| Kobo libraries | Archive stores libc as `libc-2.11.1.so`, which reports glibc 2.19 at runtime. The bundled `libstdc++.so.6.0.12` defines symbols through `GLIBCXX_3.4.20`. |
| Stock loader | The update carries `ld-linux-armhf.so.3 -> ld-linux.so.3` and the real `ld-2.11.1.so` file. The intermediate link and normal libc-family aliases are absent in the extracted update. Direct execution with that prefix failed. A run-owned POSIX prefix using archive files and restored aliases passed. |
| Stock and static binaries | A dynamically linked NickelTC GCC 4.9.4 C probe and a fully static kobov4 C probe both passed AF_UNIX socket creation, POSIX shared memory/mapping and nested ARM fork/exec under the Cortex-A9 QEMU model. The static probe has no interpreter or dynamic section. |
| C++23 | A kobov4 `std::expected` smoke binary linked with static libstdc++ and libgcc ran under Kobo's loader. Its only ELF dependencies are libc and libm, with required glibc symbols through `GLIBC_2.4`. |
| Negative control | The Debian GCC 14 C probe passed against Debian ARM glibc 2.41 and failed against the same reconstructed Kobo loader with `GLIBC_2.34 not found`. Nickel responded after testing. |
| Kernel and filesystems | QEMU reports ARMv7 userspace on x86-64 WSL2 kernel 6.18.58.1. `/tmp` is tmpfs, `/dev/shm` is a 63 MiB usable tmpfs with `noexec`, and simulated `/mnt/onboard` is ext4 mounted for execution. `MemTotal` was 32,651,124 kB and the container had no memory cap. |
| Physical Forma N782 | The plan identifies firmware 4.38.23852 and about 42.7 MB free on root. Physical kernel, CPU features, loader, RAM, VFAT permissions, namespace/seccomp and process/shared-memory/socket behavior require a read-only inventory with operator-provided access. |

The stock-prefix probe exercises the target-version archive's libc and loader on this host. QEMU supplies the host kernel, the ARM CPU model and binfmt child execution. VirtualNickel's normal FW4 Nickel process uses the Debian ARM loader at `/usr/arm-linux-gnueabihf`. Keep the explicit Kobo prefix for ABI checks. The physical device still gates executable VFAT layout, POSIX run-directory capacity, child startup, memory and sandbox behavior.

### Reproduce

From the Kobalt worktree, build the probe into an instance-independent worktree output. The existing simulator toolchain image was also used to build its FW4 observer and offscreen plugin.

```sh
worktree=$PWD
mkdir -p "$worktree/build/webkit-m0"
podman run --rm --network none -v "$worktree:/work:rw" --entrypoint sh \
  ghcr.io/pgaskin/nickeltc@sha256:3296eb400d270cfd26f5773ab09625d4469f85680b3a191951dfb78d8ade7840 \
  -ec 'arm-nickel-linux-gnueabihf-gcc -Wall -Wextra -Werror -O2 \
    /work/platforms/webkit/fw4-abi-probe.c -lrt -o /work/build/webkit-m0/fw4-stock'
readelf -lAVW build/webkit-m0/fw4-stock
```

In the VirtualNickel checkout, set `name` to a new disposable instance name in both terminals. Prepare the instance before starting it in a dedicated terminal. In the second terminal set `worktree` to the Kobalt worktree path, wait for the library, then copy and run the probe. This check needs an enabled host ARM binfmt handler for nested exec.

```sh
CONTAINER_ENGINE=podman ./launch.sh --device nia --firmware 4.38.23852 \
  --display-backend offscreen --no-viewer --name "$name" --prepare
CONTAINER_ENGINE=podman ./launch.sh --device nia --firmware 4.38.23852 \
  --display-backend offscreen --no-viewer --name "$name"
```

```sh
CONTAINER_ENGINE=podman ./launch.sh control --instance "$name" --timeout 120 library-ready 1
cp "$worktree/build/webkit-m0/fw4-stock" "state/instances/$name/artifacts/"
podman exec "virtualnickel-$name" sh -ec '
  prefix=$(mktemp -d /tmp/kobalt-wpe-m0.XXXXXX)
  mkdir "$prefix/lib"
  for pair in ld-linux-armhf.so.3:ld-2.11.1.so ld-linux.so.3:ld-2.11.1.so \
      libc.so.6:libc-2.11.1.so librt.so.1:librt-2.11.1.so \
      libpthread.so.0:libpthread-2.11.1.so libm.so.6:libm-2.11.1.so; do
    soname=${pair%:*}
    real=${pair#*:}
    cp "/firmware/rootfs/lib/$real" "$prefix/lib/"
    ln -s "$real" "$prefix/lib/$soname"
  done
  QEMU_LD_PREFIX="$prefix" QEMU_CPU=cortex-a9 timeout 10 \
    qemu-arm -U LD_PRELOAD -L "$prefix" /instance/artifacts/fw4-stock
'
```

Expected output includes `glibc=2.19` and `PASS AF_UNIX, POSIX shm/mmap and ARM fork/exec`. Stop the owned launcher, then run `CONTAINER_ENGINE=podman ./launch.sh --delete --name "$name"`. The automated run verified removal of its container, system volume and instance directory. Complete logs and the exact comparative probe commands are in the worktree's ignored `build/webkit-m0/`.

## Runtime feasibility and build decision

Use a separate FW4 C++23 sysroot based on the verified kobov4 candidate. Keep Kobalt's prepared Qt5 SDK for its current application, and build renderer dependencies from pinned sources through the existing content-addressed cache. Standard FW4 application builds use an older compiler. The PoC ARM binaries and Ubuntu 22.04 ARM libraries require up to glibc 2.35 and need a new recipe and ELF audit for this target.

The pristine 2.54.2 source selects C++23 and requires GCC 12.2 or newer. Use native build generators and CMake 3.25 or newer for the ARM32 allocator. The modern WPE headless/Skia path exposes `webkit_web_view_get_snapshot()` and its finish function with a `WebKitImage` on the 2022 GLib API. Skia uses BGRA8888 and exposes bytes and row stride via the image API. The PoC's lazy-EGL initialization and no-libdrm DMA-BUF guard are two small, separable patch candidates for the CPU/SHM path. Recheck the export set and pixel revision proof on the eventual FW4 build. [Snapshot implementation](https://github.com/WebKit/WebKit/blob/32717fabcb9dcad9aa00a54ef1f23e41ee64e849/Source/WebKit/UIProcess/API/glib/WebKitWebView.cpp), [image implementation](https://github.com/WebKit/WebKit/blob/32717fabcb9dcad9aa00a54ef1f23e41ee64e849/Source/WebKit/UIProcess/API/glib/WebKitImage.cpp)

| Closure | What to verify |
| --- | --- |
| Processes | Standalone renderer and supervisor, `WPEWebProcess`, `WPENetworkProcess`, and the GLib injected bundle module. Record actual launch paths, clean teardown and relocated paths under the private install. Disable the GPU process, media, legacy backend, DRM and Wayland for the initial CPU build. |
| Library inputs | GLib ≥ 2.70 including GIO Unix, threads and GModule, libsoup 3, ICU ≥ 70.1, HarfBuzz ≥ 2.7.4, FreeType, Fontconfig, Epoxy, xkbcommon, libgcrypt, libtasn1, libxml2, SQLite, JPEG/PNG/WebP and zlib plus their transitive closure. JavaScriptCore and WebCore are object libraries in WPE and Skia is a static archive. Epoxy is a build/link prerequisite even in the tested CPU configuration. |
| Dynamic resources | Audit the injected bundle and GIO/GModule lookups, fontconfig/fonts, ICU data, local resource mounts and optional TLS/proxy, trust, MIME and hyphenation paths. Inventory sandbox helpers and the kernel facilities they need. Verify runtime process maps against any EGL, GPU or display-server dependency. |
| Static-first | Start with a static supervisor and static compiler runtimes where the ELF and license audits support it. Check archives and PIC readiness for leaf dependencies and test static GIO/GLib module behavior. WPE's default shared library serves the renderer and both WPE helpers; the injected module is a separate dynamic load. Fully static WebKit could duplicate code in the helpers or require changing module behavior. Measure stripped deploy bytes and combined process PSS before selecting linkage. |
| Private shared fallback | If full static linkage fails or costs materially more, compare one private WPE library shared by all three processes and only the necessary private shared dependencies, with matched stock libc/loader. Present the measured closure, plugin/resource inventory and licenses for review before adopting it. |

The PoC provides leads for the headless patches, fixed XHTML/SVG fixtures, ELF/SONAME inventory, package path validation and child reaping. Its earlier FW6 shared experiment used about 96.3 MiB of files and roughly 235 MiB renderer-tree idle PSS under QEMU `max`, including QEMU overhead. Those figures belong to FW6 and its fixture. WPE-M0 produced no rendered frames, frame FDs, deployment size or renderer latency measurements. The historical PNG path and temporary sandbox exception supply no FW4 evidence. For later source-location tests, PoC `kobalt-source:1:` strings provide legacy fixtures only.

## Blaze seams at the inspected upstream revision

| Seam | Implemented in Blaze | Proposed or still required for WPE-M5 |
| --- | --- | --- |
| Session, rendering and frame ownership | `ReaderSession` owns a worker and captures a `QImage` through synchronous `engine.image(engine.page())`; it then reads links, native position and state through separate calls. | An async visual operation must finish the worker request with one generation-checked state/frame result, including mapped FD lifetime. The proposed contract describes worker-owned rendering and result ownership. |
| Navigation and totals | `Query<T>`, `PageInfo`, `LayoutGeneration`, continuation flags and estimate refinement exist. The capture path still carries raw page/rough-page state and the UI reads those fields. | Route pending and estimated results through the actual frame consumers. Preserve a known chapter jump and current-spine turn when book totals are pending. |
| Source locations | `Location`, `locationUncounted`, `Restoration`, `locationOf()` and `positionOf()` exist. The capture path still derives platform/native and engine-state positions. | Pass canonical source locations through host restore and checkpoint flows. Preserve native hints and old checkpoints. Quote fallback, generic ranges and hint policy remain proposals. |
| Cancellation | Worker jobs have revision and lease checks. Search and estimate refinement accept cooperative cancellation, and pending turns can be cleared. | Propagate request cancellation to an outstanding async renderer operation and reject stale replies after navigation, layout change or teardown. The proposed `Outcome` describes cancelled and superseded states. |
| Engine creation and capabilities | `createEngine()` constructs the current `EngineDocument`. Existing interface methods still include engine-specific cache, preview, image and layout operations. | Approve a small backend-selection and capabilities seam and test it with asynchronous frame completion. CREngine remains the default. |

[Blaze engine interface](https://github.com/nicoverbruggen/kobalt/blob/8de6ea8127a472378829910bfe59ac05f9571350/src/engine/engine.h), [ReaderSession](https://github.com/nicoverbruggen/kobalt/blob/8de6ea8127a472378829910bfe59ac05f9571350/src/reading/reader_session.cpp), [contract proposal](https://github.com/nicoverbruggen/kobalt/blob/8de6ea8127a472378829910bfe59ac05f9571350/doc/engines/contract.md)

## Review

Approve the Nia kobo7 profile and per-run stock-loader prefix as the FW4 development baseline, then use the verified kobov4 compiler for a pinned WPE build in WPE-M1. Capture the Forma's read-only baseline when access is available so its kernel and libc can constrain the build. The first FW4 snapshot must verify fresh pixels and the real helper closure before proposing a device package.

Changed files are this report and `platforms/webkit/fw4-abi-probe.c`. One agent performed the work. The actual runtime model/effort was not exposed through the session metadata. No application or simulator production source changed. Staged = no, committed = no, pushed = no, next milestone started = no.

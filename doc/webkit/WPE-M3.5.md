# WPE-M3.5 Forma transport gate

The deployment candidate uses the exact Qt 5.2.1 client and WPE renderer built at `8db6a443f97eeb7d6eea604a82c66d55b7d4256f`. Local Blaze reference `a4bb4d1cf86acaa404064dca184a0e6708a43b00`. One GPT-6.1 Sol agent. The physical Forma transport gate passed with the final fixture client.

## Payload for review

The generated `build/webkit-m3.5/kobalt-webkit.tar` is 119,674,880 bytes with SHA-256 `8073f3ef9a3adbe817588ff6cbb626222055dc8723945bcd254103d28fd85d8c`. Its only install destination is `/mnt/onboard/.adds/kobalt-webkit`. The archive contains regular files and directories, with a per-file `SHA256SUMS`.

- `bin/` has the M3.4c ARM client and renderer and two fixture launch scripts.
- `libexec/` and `lib/` have the same four WebKit ELF files verified in M2 and the same WPE closure. WebKit and its helpers use the private WPE library and the firmware glibc libraries.
- `qt-lib/` contains the prepared Qt5 SDK's Core, Gui and Network libraries plus its libstdc++, libgcc_s, ICU, zlib and libpng. These are separate from the renderer search path. Both executables rely on the stock ARM hard-float loader and glibc 2.19. No stock firmware files are installed.
- `logs/` is initially empty. The run creates private ephemeral runtime and cache directories on `/tmp`. It uses the existing fixture-only Bubblewrap environment override and runs the built-in color pages.

To recreate the staged directory from the existing M3.4c outputs, use a fresh destination:

```sh
mkdir -p build/webkit-m3.5
sh platforms/webkit/forma-transport/prepare.sh \
  build/webkit-m3.4a/qt5/wpe-pool-smoke build/webkit-m3.2/fw4/wpe-socket-renderer \
  build/webkit-m3.3/qt5-runtime build/webkit-m2-package build/webkit-m3.5/kobalt-webkit
(cd build/webkit-m3.5/kobalt-webkit && tar -cf ../kobalt-webkit.tar .)
```

The recorded archive digest identifies this exact staging run. Archive timestamps may change on restaging. Compare per-file digests before deploying a restaged archive.

## Local verification

The exact staged package verified its own manifest and passed red → blue → red, cancelled resize rollback, stale and malformed reply rejection, descriptor audit, SIGKILL, restart and retained mappings through pool retirement. The run was from a read-only worktree mount in an offline container under QEMU Cortex-A9 using the cached VirtualNickel 4.38.23852 stock loader. It used the package's actual launch scripts and separate Qt and WPE library paths. This emulated run preceded the approved device deployment.

```sh
podman run --rm --init --network none --userns keep-id -v "$PWD:/work:ro" \
  -v /usr/bin/qemu-arm:/usr/bin/qemu-arm:ro \
  -v /usr/bin/qemu-arm:/usr/libexec/qemu-binfmt/arm-binfmt-P:ro \
  --entrypoint sh 662599b1c68c64b57e6a3d8f35994592a3f4ac6042b3a66c82651db1f36428d7 -ec '
    export QEMU_LD_PREFIX=/work/build/webkit-m3.2/stock QEMU_CPU=cortex-a9
    timeout 30 /work/build/webkit-m3.5/kobalt-webkit/bin/run
  '
```

## Proposed device steps

First record the firmware version, `uname -srm`, loader and glibc versions, `df -k / /tmp /mnt/onboard`, mount flags, memory and `pidof nickel`. Require executable VFAT, writable POSIX tmpfs for runtime state, adequate free space, and a fresh destination. Stop if the destination exists. Transfer over the authorized device channel, streaming the tar directly into the new onboard directory. The intended receiver commands are:

```sh
d=/mnt/onboard/.adds/kobalt-webkit
[ ! -e "$d" ] && [ ! -L "$d" ] || exit 1
mkdir "$d" || exit 1
cd "$d" || exit 1
tar -xf -
sha256sum -c SHA256SUMS
```

When using the device's SSH login shell, stream the archive after the shell acknowledges directory creation and decode the transfer there with `base64 -d | tar -xf -`. Keep the uncompressed archive off the device. Before running, verify the file manifest against the host's copy and check that the renderer's build RPATH directory is absent. Resolve each executable's libraries using the firmware loader's `--list` with its own private library path. The proposed device test command is:

```sh
d=/mnt/onboard/.adds/kobalt-webkit
timeout 45 "$d/bin/run" >"$d/logs/transport.log" 2>&1
status=$?
cat "$d/logs/transport.log"
printf 'transport_exit=%s\n' "$status"
df -k / /tmp /mnt/onboard
pidof nickel
```

The client has an internal deadline and verifies the observed WebKit helper processes exit before restart. If the test fails or times out, capture process and FD evidence, stop any verified processes belonging to this install and confirm they have exited. Copy logs back before deleting the install. Remove only the known package files and owned logs from the new directory, then remove the empty directories. Check that root space and the Nickel PID remained stable. No other onboard application or Nickel setting is in scope.

## Physical results

The approved archive and every payload hash matched on the Forma. Its firmware was 4.38.23852 on ARMv7 Linux `4.1.15-00287-gc98fdced63d6`, with glibc 2.19 and the expected ARM loader. The loader resolved the private Qt and WPE paths and system glibc. The device had no visible `/dev/shm`. The first real WPE frame passed the Qt client's full pixel and slot checks, demonstrating that POSIX SHM allocation, descriptor inheritance and initial frame delivery worked on this device.

The test then exited 134 with `did not inspect both WebKit helpers`. The old audit enumerated `/proc/PID/task/TID/children`, which this kernel does not expose. Because the audit aborts on the first valid frame, this run did not exercise the full A→B→A or SIGKILL/restart sequence. No package process remained after the abort. The uninstall restored onboard free space to 6,135,612 KiB. Root free space stayed at 43,772 KiB and Nickel kept PID 240. Device evidence is in the `forma-*.log` files under `build/webkit-m3.5/`.

The first retry used `PPid` from `/proc/PID/status` to find the same direct helper processes, then performs the same descriptor and exit checks. Only `bin/wpe-pool-smoke` and its manifest entry differ from the first package. The replacement client SHA-256 is `25774e063084e6df7a778c4fc81cd7edb72af2324ba2447dd3b0c760c25e18c0`.

The first retry archive was `build/webkit-m3.5/kobalt-webkit-retry.tar`, 119,674,880 bytes, SHA-256 `7ae29850e47c0e5cd3abf9f5c991fc1b2cbc82b5cc7fcc1ae93c66741ada3d3f`. Its extracted directory is `build/webkit-m3.5/retry-final/`. It passed the same exact-package emulation command above with the final path replaced by `/work/build/webkit-m3.5/retry-final/bin/run`. The Qt6 WebKit smoke also passed with the new audit. See `retry-emulation.log`.

On the Forma the first retry ran through SIGKILL and produced a valid frame after restart. The client then received the shutdown reply and reaped the renderer, but Qt 5.2.1 closed the adopted socket without emitting `disconnected`. It reached the test deadline waiting for that signal. The final client observes both socket closure and QProcess exit and completes once regardless of their order, including when the descriptor is already closed at child exit. Clean shutdown still requires an intact shutdown reply. The test deadline was increased to allow multiple cold child launches on the Forma.

The final client SHA-256 is `33f2188fef9a090c2be76bcf7fbe651dcd9419b5663705386505eab4da08853b`. The final package is `build/webkit-m3.5/kobalt-webkit-final.tar`, 119,674,880 bytes, SHA-256 `634f846188a7b7c41b48c69df04ecb8ca76261067ff891a64588cee14962959d`. It has the same dependencies and paths as the original. The authorized final run updated only the client and manifest inside the temporary install. The resulting device manifest matched the host byte for byte. To repeat from a fresh install, use the same commands above with the final archive. Logs are in `forma-gate-run.log` and `forma-final-uninstall.log`.

The final physical run exited 0. The Qt client validated every pixel across the six frames below, cancellation rollback, stale or malformed reply rejection, leased images after renderer SIGKILL and restart, and mapping retention during pool replacement. It inspected both WebKit helper FD tables and waited until the observed helpers disappeared before restart or shutdown. No package-owned process remained after the run.

| Pool | Revisions | Viewport | Pages |
| --- | --- | --- | --- |
| 1, first renderer | 1–3 | 160 × 120 | red → blue → red |
| 1, first renderer | 4 | 208 × 136 | red |
| 1, restarted renderer | 1 | 160 × 120 | blue |
| 2, replacement pool | 1 | 208 × 136 | red |

The isolated install was removed and onboard free space returned to 6,135,612 KiB. Root free space remained 43,772 KiB, and Nickel retained PID 240. ARM Qt 5.2.1 and Qt 6.5.2 also passed the final `run-fw4-smoke.sh <sdk> pool webkit` suite under QEMU with the stock FW4 renderer loader and cached VirtualNickel libraries. Logs are in `final-emulation.log`. The physical run used color fixtures. EPUB resources, retained-view freshness and performance remain later gates.

M3.5 staged NO, committed NO, pushed NO. Next slice started NO.

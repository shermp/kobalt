# WPE-M3.3b transport switch review

The Qt 5.2.1 peer crash reproduces before any frame exists. A QtCore/QtDBus probe connects to the old `RendererShm1` service, receives a valid `Hello` reply, sends SIGKILL to the test renderer and sends no further RPC. Qt5 exits with SIGSEGV (139). Qt 6.5.2 observes disconnection and exits 0. The supervisor confirms the renderer was killed before creating any frame file. This isolates the crash to the old peer disconnect path in the tested ARM runtime. Identifying the exact fault inside QtDBus, libdbus or their runtime is outside this slice.

The existing Qt5 named-frame test still passes on normal shutdown and fails with status 139 after SIGKILL. Both Qt6 comparison controls pass. The existing FD-decoding failure remains separately documented in [M3.3](WPE-M3.3.md). [M3.3a](WPE-M3.3a.md) records the physical POSIX SHM probe.

| ARM SDK client | Baseline | Status |
| --- | --- | --- |
| Qt 5.2.1 | Named frames, cancellation and retained images after normal shutdown | 0 |
| Qt 5.2.1 | Named frames, then renderer SIGKILL | 139 |
| Qt 5.2.1 | Hello only, then renderer SIGKILL | 139 |
| Qt 6.5.2 | FD frames, cancellation and retained images after normal shutdown | 0 |
| Qt 6.5.2 | Named frames, then renderer SIGKILL | 0 |
| Qt 6.5.2 | Hello only, then renderer SIGKILL | 0 |

The renderer used the stock 4.38.23852 loader from the cached VirtualNickel firmware. Qt5 also used that loader, with Qt and libdbus from the prepared Qt5 SDK. Qt6 used its prepared ARM SDK runtime. QEMU Cortex-A9 ran in the offline M1 tools container. This slice did not boot Nickel or run on the Forma.

## Replacement boundary for review

- Kobalt owns the immediate renderer child through `QProcess`. A new renderer lifetime gets a new Unix stream socketpair and connection generation. Qt takes the parent endpoint through `QLocalSocket::setSocketDescriptor` in connected, read/write mode. The child wraps the other endpoint in `GSocket`. Use nonblocking I/O, independent stderr and one bounded output queue per endpoint. Combine socket EOF and errors with QProcess signals. The launcher must reap the child and handle its WebKit descendants.
- Create both endpoints and the eventual two SHM FDs close-on-exec in the parent. Pass the actual child-endpoint and SHM FD numbers as trusted launch arguments. In the forked child, clear close-on-exec only for those descriptors using Qt5's `setupChildProcess()` or the Qt6 child modifier. Use `fcntl` and an async-signal-safe failure exit. Close the parent's copy of the child endpoint after the launch has taken ownership. The child restores close-on-exec on its socket, validates and maps the SHM FDs, then closes its copies before spawning WebKit helpers. Verify inheritance in each SDK, including unrelated-FD exclusion.
- The wire uses a fixed little-endian header carrying magic, version, type, payload length and request ID. Define the exact field widths and per-method payloads with the codec. Reject invalid headers and payloads before allocating beyond the 1 MiB control-message ceiling. Incrementally parse split or coalesced messages and resume short writes. Separate foreground work from cancel and shutdown. Preserve session, document, layout, frame and request identity. An uncertain commit retires the connection. This wire uses no socket path, D-Bus, per-frame descriptor or filename.
- The Qt-owned pool creates two bounded `shm_open` objects with fresh names, immediately unlinks them, fixes their sizes and maps them read-only. The child maps the inherited descriptors writable. The client reserves a free slot per visual request and sends its index and pool generation. One successful reply publishes validated state and pixels together. Accept it only for that request and slot, after checking revisions, dimensions, stride, length, capacity and format. A slot moves from Free to Writing to Leased. A cancelled or crashed write cannot publish. A lease lasts until the last QImage reference releases it, including across restart. Defer when both slots are leased. Use a new pool generation for resizing.

| Slice | Tests required before advancing |
| --- | --- |
| M3.4a | Run Qt5 and Qt6 ARM handshake, split reads and short writes, malformed/truncated/oversized messages, cancellation and normal exit. Repeat SIGKILL, observe EOF and reaping, and audit unrelated descriptors. This spike has no WebKit or frame pool. |
| M3.4b | Check inherited anonymous SHM, exact capacity, A→B→A, concurrent retained leases, detachment, no free slot, cancel while writing, kill/restart and pool retirement. Verify no torn pixels or FD leaks. |
| M3.4c | Run real WPE frames and validate request attribution, stale and malformed replies, revision rollback, helper FDs and crash recovery. Keep the old D-Bus paths as comparison controls. |
| M3.5 | Verify the Qt5 socket and pool path on the Forma after separate physical deployment approval. |

## Reproduce

Use the staged runtime libraries and wrappers from [M3.3](WPE-M3.3.md) and [M3.3a](WPE-M3.3a.md). From this worktree:

```sh
mkdir -p build/webkit-m3.3b
for sdk in qt5 qt6; do
  podman run --rm --network none --userns keep-id -v "$PWD:/work:rw" \
    --entrypoint sh "$(sh platforms/sdk-image.sh "$sdk")" -ec '
      cd /work
      cmake -S platforms/webkit/qt-client -B "build/webkit-m3.3b/$1" \
        -DKOBALT_PEER_QT_MAJOR="${1#qt}" \
        -DCMAKE_TOOLCHAIN_FILE="/work/platforms/kobo/toolchains/$1.cmake"
      cmake --build "build/webkit-m3.3b/$1" --target wpe-qt-peer-disconnect-probe
    ' sh "$sdk"
done
sed 's#build/webkit-m3.3/qt-run#build/webkit-m3.3b/probe-run#' \
  build/webkit-m3.3a/run.sh > build/webkit-m3.3b/run.sh
sed 's#build/webkit-m3.3/\$PEER_VARIANT/wpe-qt-peer-smoke#build/webkit-m3.3b/$PEER_VARIANT/wpe-qt-peer-disconnect-probe#' \
  build/webkit-m3.3/qt-run > build/webkit-m3.3b/probe-run
chmod +x build/webkit-m3.3b/probe-run
for sdk in qt5 qt6; do
  podman run --rm --init --network none --userns keep-id --shm-size=128m \
    -v "$PWD:/work:rw" -v /usr/bin/qemu-arm:/usr/bin/qemu-arm:ro \
    -v /usr/bin/qemu-arm:/usr/libexec/qemu-binfmt/arm-binfmt-P:ro \
    --entrypoint sh 662599b1c68c64b57e6a3d8f35994592a3f4ac6042b3a66c82651db1f36428d7 \
    /work/build/webkit-m3.3b/run.sh "$sdk" hello-crash
  printf '%s status=%s\n' "$sdk" "$?"
done
```

For the original controls, use the same container command with `/work/build/webkit-m3.3a/run.sh` and arguments `qt5 named`, `qt5 crash`, `qt6 fd` and `qt6 crash`. Logs are under `build/webkit-m3.3b/`.

The clean starting WPE branch was at `2e5f3c835116a4d583c55837d7cdb1b91812adc7`. Local `develop` was at `2a708d19abfbb23aab95fab94451efe8feb001a8` and local Blaze was at `a4bb4d1cf86acaa404064dca184a0e6708a43b00`. The replacement plan was read from commit `7addac06` in the main checkout. No host integration work relies on those local reference branches in this slice.

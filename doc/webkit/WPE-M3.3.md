# WPE-M3.3 standalone Qt peer client

The QtCore, QtDBus and QtGui client lives under `platforms/webkit/qt-client/`. Its public calls use `QDBusConnection::asyncCall` and pending-call watchers on the client's owning thread. The smoke executable establishes its private peer before starting its event loop. A future GUI launcher can establish the connection off the GUI thread and give the client the connected peer.

The client checks the reply signature, request and document identity, layout and frame revisions, state dictionary and requested viewport before publishing a frame. QtDBus resolves the wire handle against the reply's descriptor list. The client requires a valid `QDBusUnixFileDescriptor` and rejects an untyped handle. It checks byte order, format, dimensions, stride, bounded length and the exact size of the regular backing object before mapping it read-only. The const-data `QImage` shares ownership of that mapping with its copies and releases it when the last copy is gone. Mutable access detaches a copy. The mapping remains readable after the peer closes.

## Results

| Prepared runtime | Build and link | Live peer result |
| --- | --- | --- |
| Kobo Qt 5.2.1 ARM | Passed | Blocked on visual FD decoding |
| Kobo Qt 6.5.2 ARM | Passed | FD transfer, red → blue → red, and retained image after shutdown passed |
| Native Linux Qt 6.11.3 | Passed | Same live checks passed against the ARM renderer |

The Qt5 test used the SDK's Qt and libdbus libraries with the stock Kobo 4.38.23852 ARM loader and glibc 2.19. `QDBusUnixFileDescriptor` and the capability APIs compile and link. The peer reports no Qt FD capability. Nevertheless, the renderer accepts `Hello`, which checks transport-level FD negotiation, and replies to `Open` with the expected signature, including `h`. Qt 5.2.1 exposes the argument as a `void*` QVariant instead of the descriptor type. The client returns `UnixFdUnavailable` and retires the connection before mapping a frame. The test exits with status 1. This is the observed behavior of the prepared SDK in this peer test. It does not establish the Qt5 behavior on a message bus or on the physical Forma.

The standalone renderer remains the M3.2 ARM build and uses the same M1 runtime closure. The tests ran in offline containers under QEMU Cortex-A9 using the cached VirtualNickel FW4 firmware libraries for the renderer. The Qt6 ARM client used the prepared Qt6 SDK's ARM userland. No device package, real-device transport test, or performance baseline was produced.

## Reproduce

From the worktree, build in each pinned SDK:

```sh
for sdk in qt5 qt6; do
  image=$(sh platforms/sdk-image.sh "$sdk")
  podman run --rm --network none --userns keep-id -v "$PWD:/work:rw" \
    --entrypoint sh "$image" -ec '
      cd /work
      cmake -S platforms/webkit/qt-client -B "build/webkit-m3.3/$1" \
        -DKOBALT_PEER_QT_MAJOR="${1#qt}" \
        -DCMAKE_TOOLCHAIN_FILE="/work/platforms/kobo/toolchains/$1.cmake"
      cmake --build "build/webkit-m3.3/$1"
    ' sh "$sdk"
done
```

The run wrappers used for validation are in `build/webkit-m3.3/`. Stage the Qt5 SDK's `/tc/qt5/sysroot/lib` and `usr/lib` under `qt5-runtime/`. Stage the Qt6 SDK's `/usr/lib/arm-linux-gnueabihf` and target Qt6 library directory under `qt6-runtime/lib` and `qt6-runtime/qt`. Run `run.sh qt5` or `run.sh qt6` in the M1 tools image with the worktree mounted at `/work`, no network, an init process and the QEMU interpreter mounted as in M3.2. The wrappers use the [M3.2](WPE-M3.2.md) ARM renderer and private runtime. The Qt5 wrapper uses the stock FW4 loader and `LC_ALL=C` to avoid the host locale archive.

## Handoff

Source base `d720fcb95abe22f7efcea4fd98d208da31115e4a`. Local Blaze reference `a4bb4d1cf86acaa404064dca184a0e6708a43b00`. Single agent, GPT-6.1 Sol. Changed the standalone client, its smoke executable and this handoff. Qt5 live FD passing remains a blocker for the M3 gate. The smallest next investigation is whether the prepared Qt5 peer can decode the descriptor through an existing supported API. If it cannot, a scoped FW4 transport fallback needs review before changing the protocol or SDK.

M3.2 was committed as authorized. M3.3 staged: NO. Committed: NO. Pushed: NO. M3.4 started: NO.

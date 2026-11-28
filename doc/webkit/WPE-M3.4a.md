# WPE-M3.4a Qt launcher and GLib socketpair spike

The prepared ARM Qt 5.2.1 and Qt 6.5.2 launchers both completed the socketpair lifecycle tests. They each survived five renderer SIGKILL runs while mock work was pending, observed socket EOF, and received QProcess completion with crash status.

The spike lives under `platforms/webkit/socket-spike/`. It has a QtCore/QtNetwork launcher and a minimal GLib/GIO child. It does not link WebKit, QtDBus, or shared-memory frame code. The existing comparison services remain available.

The launcher creates a close-on-exec Unix stream socketpair and gives the parent endpoint to QLocalSocket. Qt5 overrides `setupChildProcess()` and Qt6 uses a child process modifier to clear close-on-exec on the child endpoint only. The launcher verifies its descriptor still has the flag after startup, then closes its copy. The child validates the inherited descriptor, restores close-on-exec, and verifies that the parent's endpoint and a separate close-on-exec sentinel are absent. A deliberate missing-descriptor run exits before the handshake.

The test protocol uses a fixed 20-byte little-endian header and a 4 KiB payload ceiling. The launcher sends a fragmented maximum-size mock request and reads replies incrementally. The child bounds each nonblocking send and resumes from the number of bytes accepted. The test checks the target request's cancellation result and a separate acknowledgement before shutdown. Wrong version, oversized payload declarations, and EOF during a partial header cause rejection. The launcher separately observes socket EOF and the QProcess exit before declaring a pass. The fragment sizes exercise multiple reads and writes. Kernel-induced short writes and a full socket send buffer were not forced.

## Reproduce

Prepare the stock FW4 loader and the staged Qt5 and Qt6 libraries using [M3.2](WPE-M3.2.md) and [M3.3](WPE-M3.3.md). From the worktree:

```sh
for sdk in qt5 qt6; do
  podman run --rm --network none --userns keep-id -v "$PWD:/work:rw" \
    --entrypoint sh "$(sh platforms/sdk-image.sh "$sdk")" -ec '
      cd /work
      cmake -S platforms/webkit/socket-spike -B "build/webkit-m3.4a/$1" \
        -DKOBALT_SPIKE_QT_MAJOR="${1#qt}" -DKOBALT_SPIKE_GLIB=OFF \
        -DCMAKE_TOOLCHAIN_FILE="/work/platforms/kobo/toolchains/$1.cmake"
      cmake --build "build/webkit-m3.4a/$1"
    ' sh "$sdk"
done

image=662599b1c68c64b57e6a3d8f35994592a3f4ac6042b3a66c82651db1f36428d7
cache=$(realpath ../../build/dependency-cache)
podman run --rm --network none --userns keep-id -v "$PWD:/work:rw" \
  -v "$cache:/dependency-cache:ro" --entrypoint sh "$image" -ec '
    cd /work
    glib=$(cat build/webkit-m1/glib-entry)
    export PKG_CONFIG_LIBDIR="$glib/install/lib/pkgconfig"
    cmake -S platforms/webkit/socket-spike -B build/webkit-m3.4a/fw4 -GNinja \
      -DCMAKE_TOOLCHAIN_FILE=/work/dependencies/webkit/fw4-toolchain.cmake \
      -DKOBALT_WEBKIT_TOOLCHAIN="$glib/sources/kobov4/arm-kobov4-linux-gnueabihf" \
      -DKOBALT_WEBKIT_PREFIX="$glib/install" \
      -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++"
    cmake --build build/webkit-m3.4a/fw4
  '

for sdk in qt5 qt6; do
  podman run --rm --init --network none --userns keep-id -v "$PWD:/work:rw" \
    -v /usr/bin/qemu-arm:/usr/bin/qemu-arm:ro --entrypoint sh "$image" \
    /work/platforms/webkit/socket-spike/run-fw4-smoke.sh "$sdk"
done
```

Both suites used QEMU Cortex-A9 in an offline tools container. The ARM GLib 2.80.5 child and Qt5 launcher used stock Kobo glibc 2.19 from the cached VirtualNickel 4.38.23852 firmware. Qt6 used its prepared SDK userland. For emulation, QProcess explicitly launches the host QEMU binary with the ARM child as its program. SIGKILL targets that immediate process. This does not exercise direct ARM exec on the Forma, inherited SHM, or process-tree cleanup with WebKit helpers. The child directly needs the stock ARM loader, libc, libm, libpthread, libdl, and libresolv. Test logs are in `build/webkit-m3.4a/`.

WPE base `50d9bddb` and local Blaze `a4bb4d1c`. One GPT-6.1 Sol agent. M3.3b was committed as separately authorized. M3.4a staged: NO. Committed: NO. Pushed: NO. M3.4b started: NO.

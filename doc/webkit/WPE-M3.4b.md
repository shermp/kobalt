# WPE-M3.4b inherited POSIX SHM and QImage leases

Both prepared ARM Qt clients passed the two-slot mock-renderer suite. Each slot is a bounded POSIX SHM object with an unpredictable name, immediately unlinked. The parent keeps close-on-exec descriptors and read-only maps. The M3.4a child hook passes only the socket and two SHM descriptors. The child checks the backing objects are distinct, regular, unlinked, and exactly the expected size. It maps them writable and closes both descriptors before handshaking.

The mock protocol carries the pool generation, slot and frame metadata. The launcher validates the request ID and the complete metadata before publishing a const-data QImage. Slot state progresses from Free to Writing to Leased. When the last shared image reference releases its lease, the slot is free again. The slot mapping outlives its original pool if an image still holds it. The smoke launcher defers its single queued operation until a slot is available. It does not use synchronous waits.

## Evidence

- Qt 5.2.1 and Qt 6.5.2 passed full-pixel A→B→A, concurrent red and blue leases, copy-on-write detachment, and QPainter reading the leased image. Both occupied slots reject a new reservation, including while an extra image copy retains the second one.
- Both rejected deliberately altered generation and stride metadata. The mock pauses halfway through a write so cancellation and SIGKILL occur before publication. After cancellation the previous image stays readable. The crashed child is reaped and its socket reaches EOF before the dirty slot can be reused.
- Both restarted the mock child with a new socketpair and the same pool. The child fully rewrote the dirty slot while the retained image stayed valid. A new generation with a different capacity left both retired images readable. All four SHM descriptor numbers returned EBADF after the pools and their leases were released.
- Both original M3.4a socket suites passed after the shared launch hook changed. This remains a mock-child check. WPE snapshots, WebKit helper FD auditing and the production wire are M3.4c work.

## Runtime limits

The passing runs used ARM QEMU Cortex-A9 in the offline M1 tools image. The Qt5 launcher and GLib child used the stock Kobo 4.38.23852 loader and glibc 2.19 from the cached VirtualNickel firmware. The Qt6 launcher used its SDK runtime. The shared-memory tests used Podman's normal tmpfs layout. No new device deployment or performance baseline was produced. Binary dependencies add QtGui and the SDK's POSIX SHM support to the pool test launcher. The GLib child still uses the existing static GLib runtime and stock loader closure.

The launcher contains no hard-coded SHM filesystem path. A separate Qt5 run with `--ipc=none` and writable `/tmp` tmpfs failed allocation. QEMU traced stock glibc's fallback after the absent `/dev/shm` to Podman's first tmpfs mount, the file `/etc/hosts`, where opening the SHM name failed with ENOTDIR. That mount layout differs from the Forma. The earlier stock Forma POSIX SHM probe passed without a visible `/dev/shm`. The full pool still needs verification on that device at M3.5. No fallback allocator was added.

## Reproduce

Use the three build commands in [M3.4a](WPE-M3.4a.md) with this source. In the final tools-container command, append the `pool` argument after `"$sdk"`:

```sh
image=662599b1c68c64b57e6a3d8f35994592a3f4ac6042b3a66c82651db1f36428d7
for sdk in qt5 qt6; do
  podman run --rm --init --network none --userns keep-id -v "$PWD:/work:rw" \
    -v /usr/bin/qemu-arm:/usr/bin/qemu-arm:ro --entrypoint sh "$image" \
    /work/platforms/webkit/socket-spike/run-fw4-smoke.sh "$sdk" pool
done
```

Omit `pool` for the original socket controls. To reproduce the container mount failure, add `--ipc=none --tmpfs /tmp:rw,mode=1777,size=64m` and run the Qt5 pool. Logs are under `build/webkit-m3.4b/`.

WPE base `532c08d7`, local Blaze `a4bb4d1c`. Single GPT-6.1 Sol agent. M3.4a was committed as authorized. M3.4b staged NO, committed NO, pushed NO. M3.4c started NO.

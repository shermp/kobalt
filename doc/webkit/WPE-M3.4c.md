# WPE-M3.4c WPE snapshots over the socket and slot protocol

The standalone `wpe-socket-renderer` uses the inherited GSocket and two writable mappings from M3.4b. The Qt fixture client launches it through QProcess and receives control replies through QLocalSocket. The real fixture path uses neither D-Bus nor per-frame file descriptors. The previous Renderer1 and RendererShm1 remain comparison controls.

Each visual command constructs a staged headless view with built-in HTML. The renderer waits for that view's load completion, requests a visible WebKit snapshot, checks its dimensions and uses a worker to validate every pixel and copy the rows into the reserved slot. The renderer commits the state and queues one visual reply after the worker has finished. Cancellation drains outstanding work before confirming that the dirty slot can be reused. The client validates the complete reply before creating a QImage lease. A new QProcess lifetime owns a new socket and request sequence. Test restarts reuse the existing pool without overwriting retained frames.

## Fixture wire

The framing uses the M3.4a 20-byte little-endian header. It contains `WPES`, u16 version 1, u16 message type, u32 payload length (at most 4096), and a nonzero u64 request ID. Request IDs increase strictly on each socket. A reply uses the request ID it settles. The request sequence and active connection distinguish renderer lifetimes, and the pool generation identifies mapped storage. The prototypes use the following fixed-size fixture fields:

- `PoolHello` sends u64 pool generation and u32 slot capacity. The renderer validates the mapped capacity and echoes those fields in `Ready`.
- `Open`, `Turn`, `Navigate`, `SetViewport` and `SetAppearance` send a 24-byte slot spec followed by a 24-byte base revision. The spec is u64 pool generation and four u32 fields for slot, width, height and page. The revision is three u64 fields for document, layout and frame. Open uses a zero base and selects one of the two built-in pages. Updates require the current base. Turn changes to the other page. Navigate selects the requested built-in page. SetViewport preserves the page. SetAppearance is the fixture's default-appearance operation.
- One successful visual reply carries the slot spec followed by u32 stride, byte length and format, then the committed revision. Format 1 is tightly packed little-endian premultiplied BGRA8888. The two page values identify exact fixture locations. Open starts layout and frame at 1. Resize advances the layout revision and every visual success advances the frame revision within its document.
- `Query` and `Close` send the exact base revision. State replies carry the revision and u32 page, width and height. Close replies with an empty acknowledgement. `Cancel` sends the target u64 request ID. Its one-byte acknowledgement reports whether it won before commit. An accepted target returns a terminal cancellation error. `Shutdown` drains any pending operation and returns an empty acknowledgement before EOF. Errors are u32 codes and carry no frame.
- A malformed header or unrecognized command retires the socket. A stale or invalid visual request does not advance committed state. The Qt fixture uses one outstanding visual request. It discards a failed reservation only after the terminal result or after EOF and child reaping.

This codec covers the two-page fixture. EPUB resources, location and appearance payloads beyond this fixture require later slices. Each operation still uses a fresh candidate view. The retained-view page freshness gate belongs to M4.2.

## Verification

Use the [M3.2 FW4 configuration](WPE-M3.2.md) and the [M3.4a Qt SDK configurations](WPE-M3.4a.md). Build `wpe-socket-renderer` in `build/webkit-m3.2/fw4` inside the M1 tools image with its existing WPE dependency cache. Rebuild each Qt target in `build/webkit-m3.4a/qt5` and `build/webkit-m3.4a/qt6` in the pinned SDK. From the worktree, with those build outputs and the M2 private runtime present:

```sh
image=662599b1c68c64b57e6a3d8f35994592a3f4ac6042b3a66c82651db1f36428d7
for sdk in qt5 qt6; do
  podman run --rm --init --network none --userns keep-id -v "$PWD:/work:rw" \
    -v /usr/bin/qemu-arm:/usr/bin/qemu-arm:ro \
    -v /usr/bin/qemu-arm:/usr/libexec/qemu-binfmt/arm-binfmt-P:ro \
    --entrypoint sh "$image" \
    /work/platforms/webkit/socket-spike/run-fw4-smoke.sh "$sdk" pool webkit
done
```

- ARM Qt 5.2.1 and Qt 6.5.2 passed the mock pool and real WPE fixture. The real path checks every pixel on A→B→A, successful resize, cancelled resize followed by a query of the original state, stale query rejection, and mapping retention during renderer SIGKILL, restart and pool replacement.
- The same Qt receive validator rejects copies of the live reply with wrong request, generation, revision, or stride and with truncated metadata. Replaying the same valid reply cannot acquire a second lease.
- The client inspected both WPE helper types' descriptor tables under `/proc`. The audited helpers held neither shared object nor the renderer's control socket. The renderer's inherited SHM descriptors were also closed. The harness waited for the observed helpers to disappear before restart or final cleanup. The container init handles reparented helper reaping in this test.
- The separate Qt6 Renderer1 FD smoke passed against the rebuilt comparison service.

The WPE build used stock 4.38.23852 ARM glibc 2.19 under QEMU Cortex-A9, from the cached VirtualNickel firmware. The Qt5 client used the stock loader and the Qt5 SDK libraries. Qt6 used its prepared SDK userland. The M1 private WebKit and helper closure is unchanged. The tests ran in an offline container with the fixture-scoped Bubblewrap override. Nickel was not booted for this test. Device SHM allocation, helper cleanup and the complete process tree have yet to be verified on the Forma. This run did not measure performance.

WPE base `5578fde8`, local Blaze `a4bb4d1c`. Single GPT-6.1 Sol agent. M3.4b was committed as authorized. M3.4c staged NO, committed NO, pushed NO. M3.5 started NO.

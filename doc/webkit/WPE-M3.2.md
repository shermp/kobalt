# WPE-M3.2 standalone Renderer1 service

The standalone `wpe-renderer` target implements the [Renderer1 contract](WPE-M3.1.md) for the two built-in color pages. It embeds the introspection XML, accepts one authenticated peer at a private Unix socket, and prints the address for the launcher. The launcher supplies an empty, owner-only run directory and owns its cleanup on abnormal exit.

The service serializes foreground calls on the GLib event loop. It checks request IDs and base revisions before starting WebKit work. Each visual operation has an isolated candidate view, so cancellation or failure can discard that view and preserve the committed state. After WebKit's load and snapshot callbacks, a worker validates every fixture pixel and writes an owned frame. The GLib loop remains available to process cancellation during that work. A successful reply commits the candidate state and sends one FD with its identity and geometry. The backing object is sealed memfd where available, with an unlinked, read-only private tmpfs file as the fallback. The server closes its writable backing references before publishing. Shutdown cancels pending work, flushes the reply and closes the peer.

## Reproduce

Use the M1 native tools image and WPE prefix documented in [WPE-M1](WPE-M1.md). In that container, from the worktree:

```sh
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
cmake -GNinja -S platforms/webkit -B build/webkit-m3.2/native
cmake --build build/webkit-m3.2/native
LD_LIBRARY_PATH="$prefix/lib" WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 \
  ctest --test-dir build/webkit-m3.2/native --output-on-failure
```

Use an offline container with an init process for this fixture run. The smoke client checks handshake gating, request and revision identities, cancellation of an in-flight replacement, stale-state rejection, red → blue → red, resize, appearance, query, close and shutdown. It validates the FD's length, checks that a write fails, reads every expected pixel through the read-only mapping and compares the initial and returned red frames after the connection closes.

For the ARM build, use the M1 FW4 fixture CMake inputs, select a fresh build directory and build the `wpe-renderer` target. The cross-build passed using the existing GCC 14.4 hard-float toolchain and embedded GLib WPE runtime. The executable is 169,348 bytes before stripping. It requires the stock ARM loader, libc, libm and the existing private WPE library. Its highest directly required glibc symbol version is 2.17. The [M1 helper and library closure](WPE-M1.md) still applies. The executable retains a build-time RPATH, and the emulated run used the private library path.

The native smoke test passed with GLib criticals fatal. The same client passed against the ARM service in an offline tools container under QEMU Cortex-A9 and the stock Kobo 4.38.23852 glibc 2.19 loader copied from the cached VirtualNickel firmware volume. The QEMU wrapper sets `QEMU_LD_PREFIX` to the M1 stock loader layout, `WEBKIT_EXEC_PATH` and `WEBKIT_INJECTED_BUNDLE_PATH` to the M2 package helpers and libraries, and passes that package's `lib` through QEMU's `-E LD_LIBRARY_PATH=…`. Mount the registered ARM binfmt interpreter in the container for nested helper exec. Run the wrapper and native client through `run-renderer-smoke.sh` under a timeout. The first emulated launch lacked the binfmt interpreter inside the container and failed to launch the network helper. Adding the interpreter mount resolved that failure.

## Handoff

Source base `ff9ac476d6fb899c3488ca832f3e8bde61f845bb`. Local Blaze reference `a4bb4d1cf86acaa404064dca184a0e6708a43b00`. Single agent, GPT-6.1 Sol. Changes cover the standalone build, service, backing storage, smoke client and test runner under `platforms/webkit/`. The M2.2 physical Forma result remains the latest device evidence. M3.2 did not deploy a device package or collect performance measurements.

M3.3 implements the asynchronous Qt clients and verifies actual SDK FD support. M3.4 covers the larger race and failure matrix, including forced renderer death and invalid frame metadata. The tmpfs fallback, independent worker-copy cancellation, physical FD path and long-run resource use were not exercised separately. The fixture uses a fresh view per visual operation. Retained pagination and EPUB access are M4 work.

M3.1 was committed as authorized. M3.2 staged: NO. Committed: NO. Pushed: NO. M3.3 started: NO.

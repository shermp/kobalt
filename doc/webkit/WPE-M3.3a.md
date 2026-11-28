# WPE-M3.3a named frame transport experiment

The named tmpfs prototype delivered correct frames to Qt 5.2.1 and Qt 6.5.2 under ARM emulation. Qt 5.2.1 still crashed when the renderer was killed. The existing Renderer1 FD transport passed the Qt6 control run. The experiment is opt-in through `wpe-renderer --shm-experiment RUN_DIR` and uses the separate `org.kobalt.RendererShm1` object and interface. Review is required before changing Renderer1.

## Design

Visual replies keep the same state, identities, revisions and pixel metadata. The experimental signature substitutes a string basename for the Unix FD handle. The renderer accepts a mode-0700 tmpfs run directory, writes and validates a fresh backing object, sets mode 0400 and closes the writer before publishing. The client opens only the expected basename relative to the launcher-supplied directory, follows no final symlink, checks the owner, mode, regular-file type and length, and maps read-only. This relies on the trusted renderer and private directory. File permissions do not provide the kernel-enforced immutability of a sealed FD against the owner or root.

The renderer keeps one published name until the next visual commit, close, or disconnect. The client serializes foreground requests and opens each successful frame before it submits the next request. Replacing or unlinking a name leaves existing QImage mappings valid. Failed and cancelled candidates do not advance the committed revision. A forced kill leaves the current file for the launcher to remove from the run directory it owns. Compared with descriptor passing, the named path adds name validation, filesystem checks and orphan cleanup. Both paths use one copy from the WebKit snapshot.

## FW4 verification

The probe at `platforms/webkit/shm-probe.c` passed `shm_open`, read-only `mmap`, `shm_unlink` and retained mapping checks on the physical Forma and in the VirtualNickel runtime image with the cached stock ARM loader. The simulator container had 64,000 KiB available on `/dev/shm`. The Forma (4.38.23852, ARMv7, Linux 4.1.15, glibc 2.19) had no `/dev/shm` and about 254,208 KiB free on `/tmp` during the probe. The stock POSIX shared-memory probe still passed. The prototype explicitly uses private tmpfs files so placement and cleanup are under launcher control. The Forma ran only the short-lived probe. Its full Qt and renderer transport remains unverified on hardware.

The M3.3 finding was reproduced in the prepared Qt5 SDK. The Qt peer reported no FD capability although the server's transport negotiation passed. QtDBus exposed the visual reply's `h` field as a `void*` QVariant. The client rejects the field before mapping. No firmware Qt library changes were made. The string-based experiment bypasses that decoding step.

| ARM SDK client under QEMU | Named frames, cancellation and retained images | Renderer SIGKILL |
| --- | --- | --- |
| Qt 5.2.1 | Passed A→B→A, cancellation and retained images after normal shutdown | Client SIGSEGV after the last frame and renderer kill, including when no further RPC is sent |
| Qt 6.5.2 | Passed the same checks, including the FD control run | Retained images passed and the supervisor reclaimed the orphaned file |

The source of the Qt5 crash remains unverified. It is reproducible with the prepared Qt5 libraries and stock loader in ARM emulation. Do not treat this prototype as satisfying the M3 gate.

## Relative cost in emulation

Five interleaved runs per transport on the same ARM Qt6 SDK client and ARM renderer. Latency includes the full fixture sequence through the final frame. CPU covers the supervised run and reaped descendants. PSS is the median of each run's sampled process-tree peak and includes QEMU overhead. The fixture is small and the measurements do not establish device performance.

| Transport | Median fixture latency | Median CPU time | Median peak sampled PSS |
| --- | ---: | ---: | ---: |
| FD | 2,183 ms | 2.847 s | 271,647 KiB |
| Named tmpfs | 2,184 ms | 2.981 s | 275,718 KiB |

Latency was effectively tied at this scale. Named tmpfs used slightly more CPU and sampled memory in this run set. Measurements and wrappers are under `build/webkit-m3.3a/`.

## Reproduce and review

Compile the ARM probe using the M1 compiler with `-Wall -Wextra -Werror -Os platforms/webkit/shm-probe.c -lrt`. Run with the stock loader as in M3.2. Build the ARM renderer and the two Qt clients using [M3.2](WPE-M3.2.md) and [M3.3](WPE-M3.3.md). Then use the prepared ARM wrappers in an offline tools container:

```sh
timeout 45 sh platforms/webkit/qt-client/run-shm-experiment.sh \
  /work/build/webkit-m3.2/fw4-run /work/build/webkit-m3.3/qt-run named /dev/shm
```

The Qt wrapper uses the M3.3 SDK runtime variables and forwards all arguments. Repeat with `fd` for the control and `crash` for forced renderer death. The supervisor checks normal name and socket removal and reports the orphan before cleaning it after a successful crash test.

Source base `d720fcb9`, plan update `4db52ae8`, local Blaze reference `a4bb4d1c`. Single agent, GPT-6.1 Sol. Existing renderer and helper closures are unchanged. The standalone client changes build on the unstaged M3.3 work. Review the Qt5 crash and ownership tradeoffs before making a production transport decision.

Staged: NO. Committed: NO. Pushed: NO. M3.4 started: NO.

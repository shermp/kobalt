# Software rendering investigation

The inspected source is the hash-verified WPE WebKit **2.54.2** release. Native and ARM firmware runtime behavior below is verified. The ARM experiment uses the expressly authorized per-run sandbox exception.

## Selected existing rendering path

- `AcceleratedSurface.cpp`: a software swap chain uses shared memory; `RenderTargetSHMImage` creates a Skia bitmap surface when GL is not in use.
- `AcceleratedSurface.h`: composited rendering still uses GL even when hardware acceleration is disabled. Both preferences must be disabled.
- `DrawingAreaCoordinatedGraphicsGLib.cpp` selects `NonCompositedFrameRenderer` with accelerated compositing disabled; that renderer skips GL-context creation for a raster surface.
- `UIProcess/wpe/AcceleratedBackingStore.cpp` wraps received bitmaps as `WPEBufferSHM`. The headless view acknowledges rendered/released buffers without a native window server.

The public feature identifiers are **`HardwareAcceleration`**, **`AcceleratedCompositing`** and **`ForceCompositingMode`**. `WebKitFeature.cpp` strips `Enabled` from internal preference keys. The prototype disables and reads back all three before view creation, and fails if any is missing. The dedicated hardware-acceleration-policy API is GTK-only in this release.

The probe also sets the existing `WEBKIT_SKIA_ENABLE_CPU_RENDERING=1` control in its own process before WebKit initialization. `WebProcessGLib.cpp` uses it to disable accelerated buffers process-wide. It is independent of the sandbox exception.

## Minimal release corrections

Modern WPE eagerly calls `initializePlatformDisplayIfNeeded()` before page preferences arrive. Its surfaceless-display fallback terminates without EGL. The [lazy initialization patch](../../dependencies/webkit-m0/lazy-wpe-egl.candidate.patch) removes that eager request for the tested non-legacy WPEPlatform configuration and enables the existing preference-time path for hardware-accelerated views.

A separate [guard](../../dependencies/webkit-m0/no-libdrm-snapshot.candidate.patch) fixes a compile failure in `getImageInfoFromBuffer()`: its DMA-BUF branch references `DRM_FORMAT_XRGB8888` outside the conditional DRM header include. The guard retains that branch when libdrm or Android supplies the definitions and leaves SHM handling available without them.

Both candidates are small, separate and tested on native x86-64 and ARM32 firmware. Their behavior in default GPU/Vulkan/legacy configurations is unverified. They are not claimed to be upstream-approved.

## Public capture and local resources

`webkit_web_view_get_snapshot()` completes asynchronously with a WPE 2.0 `WebKitImage`. Public accessors expose BGRA8888 bytes, dimensions and stride. The probe retains the image while validating/writing its pixels. GTK's Cairo/GdkTexture return types do not apply here. Direct WPEBufferSHM capture remains an alternative requiring correct buffer-release lifetime handling.

The minimal native image lacks a desktop MIME database: GIO guessed XHTML as `text/plain`. The proof therefore serves only its fixed local XHTML/font/PNG/SVG resources through WebKit's public URI-scheme API with explicit MIME types. It does not implement an EPUB parser or a general file server. Future EPUB integration should reuse Kobalt's publication/resource layer and manifest MIME information.

Trusted scripts run in a named world. They await font readiness and image decode, assert the embedded font and document geometry, check WebGL/WebGPU APIs are absent, update the DOM, and wait for animation-frame completion before snapshotting. Arbitrary book JavaScript is not exercised by this fixture.

## Verified native evidence

The [page image](../../build/webkit-m0/render-native/native-cpu.png) is 800×1100 with stride 3200. It was visually inspected: text, ligatures, PNG, SVG, the equation and the trusted-JavaScript marker render correctly. Repeated startup produces SHA-256 `47487d385f5daa08836c0d3e680545ae507035c07bd87b84cc8aaf115d5cf31b`. A second snapshot after changing the marker produces different pixels.

Native runs retain Bubblewrap sandboxing and the existing offline filter. They expose neither `/dev/dri` nor `/dev/dxg`, have no display server, and set EGL vendor and GL driver paths to nonexistent locations. A negative-control program cannot initialize surfaceless EGL under the same settings. All observed frames are SHM; the measured process maps contain no EGL/GL/Vulkan/DRM/GStreamer modules.

| Diagnostic | Observed native result |
| --- | --- |
| First correct snapshot | About 111 ms in the initial successful run, before PNG encoding |
| DOM-update snapshot | About 10–21 ms across recorded runs, before PNG encoding |
| PNG encoding/write | About 17–27 ms |
| Idle aggregate memory | 111156 KiB PSS; 181712 KiB summed RSS in the longer sample |
| Settled idle CPU | No CPU ticks observed in a 5.03-second window; stable process set |

The memory sample includes the UI/helper, WebProcess, NetworkProcess, Bubblewrap parents and timeout wrapper. RSS double-counts shared pages; PSS is the better aggregate. Some timing runs overlapped the ARM build. These are diagnostic host measurements, not device performance or power estimates.

Success, repeated startup, DOM repaint and missing-resource failure passed cleanup checks, including zombie process names and Bubblewrap parents. The per-run sandbox opt-out is guarded by an explicit CLI argument and was not used for native rendering.

Useful logs remain under `build/webkit-m0/logs/`: `native-render-5.log`, `native-render-repeat.log`, `native-metrics.log`, `native-metrics-final.log`, `native-idle-5s.log`, `native-missing-resource.log`, `override-guard.log` and `egl-negative-control.log`.

## Verified firmware evidence

The same source was cross-compiled with GCC 14.4.0 and run through QEMU 10.0.13 inside stock firmware 6.0.274403 alongside Nickel. All captures are 800×1100, stride 3200, with SHM frames only. The [ARM first page](../../build/webkit-m0/firmware-results-512m/cold-1.png) was visually inspected: typography, accents, ligatures, raster/SVG images, the equation and script marker are present. ARM and native PNGs need not have the same checksum because their font/text stacks differ.

Repeated ARM startups, both memory caps and the start after a killed WebProcess produce SHA-256 `89252a681e502ab38fe3f8f178d89c696030f05085a5dc0f3d774cbdaadb0008`. The second snapshot after the trusted DOM update produces `33e88530cefc9501b5d96ce1f518ab406671ff7289cf0ebe0d69cbd33660a8e7`. No GL/EGL/Vulkan/DRM/GStreamer modules appear in the measured process maps; EGL vendor and GL driver paths point to nonexistent locations, and there is no display server.

| Diagnostic | ARM firmware result with QEMU `max` |
| --- | --- |
| First correct snapshot | 2.16–2.38 s across the supervised runs, before PNG encoding |
| DOM-update snapshot | 43.505 ms at 768 MiB; 36.330 ms at 512 MiB |
| PNG encoding/write | About 133–156 ms |
| Idle PSS / summed RSS | 240407 / 306316 KiB at 768 MiB; 240682 / 306592 KiB at 512 MiB |
| Settled idle CPU | One CPU tick in 5.012 s at 768 MiB; two ticks in 5.012 s at 512 MiB, about 0.2% / 0.4% |
| Process set | UI/helper, NetworkProcess and WebProcess under QEMU, plus timeout; stable during sampling |
| Temporary XDG cache/config/data directory | Zero allocated KiB after each run; the probe uses an ephemeral network session |

The PSS sum excludes Nickel and the small native Python sampler/supervisor, but includes QEMU's code and translation/runtime overhead. Summed RSS double-counts shared mappings. Temporary shared-memory, image and allocator use is included in these process samples and whole-container peaks, not isolated as per-allocation peaks. This is not a memory leak or large-document stress test.

At one CPU, the controlled fixture passed 768 MiB and 512 MiB device memory caps without OOM events. The whole-container peaks were 547913728 bytes and 528117760 bytes respectively. The latter is about 504 MiB, close to its 512 MiB cap. Peaks include Nickel and charged caches, so neither they nor QEMU PSS predict a physical Kobo's footprint.

An unsupervised first run produced the correct pixels and exited successfully but left exited child zombies under the container's PID 1. The native test supervisor's `PR_SET_CHILD_SUBREAPER` and `waitpid` loop collected them. It also collected the remaining child after an intentional WebProcess SIGKILL; the probe returned 1 and Nickel answered a ping. A fresh start after that crash returned the original image. This verifies failure containment and test-owned cleanup, not a production recovery protocol.

Concise resource records are [firmware-768m.json](evidence/firmware-768m.json) and [firmware-512m.json](evidence/firmware-512m.json). Logs and captures remain under `build/webkit-m0/firmware-results-supervised/` and `firmware-results-512m/`. These runs preserve the simulator's external-network restrictions, but do not validate WebKit's disabled internal sandbox or physical hardware performance.

The follow-up [Cortex-A7 record](evidence/firmware-cortex-a7.json) runs the helper and inherited WebKit children with `-cpu cortex-a7` / `QEMU_CPU=cortex-a7`, while stock Nickel retains the launcher's ordinary CPU model. Render, repaint, repetition, crash containment and post-crash startup pass with the same PNGs. The update snapshot took 33.228 ms; aggregate PSS was 258266 KiB and cgroup peak 578162688 bytes at the 768 MiB cap. This extra run checks the fixture's ARMv7 execution after the ABI audit found libgcrypt ARMv8 attributes; it does not test all of that library's optimized entry points. The different QEMU memory use reinforces why these measurements are not physical-device predictions.

# WPE-M8 footprint and release assessment

The host baseline is Kobalt `53ef80b9`. The installed FW4 runtime is WPE 2.54.2 with ARM32 JIT, DFG and WebAssembly enabled. The candidate is an opt-in build of the same pinned sources. It uses the C-loop JavaScript interpreter, disables JIT, DFG, FTL, WebAssembly and the incompatible sampling profiler, and compiles out Bubblewrap. Both configurations use headless WPE, CPU Skia, `-O1`, static compiler runtimes and two private WPE shared objects. The existing host and default Blaze selection require no changes.

## Buffer comparison

Use `wpe-footprint-probe` from [the standalone WPE project](../../platforms/webkit/CMakeLists.txt). It sends the same retained XHTML workload through the production renderer and frame protocol at 1440 × 1920. Each run checks first-page pixels, an adjacent preview that leaves the displayed page intact, repeat turns, font reflow and renderer shutdown. Two-slot mode retains mappings. Copy mode keeps published images in private memory and sends every render to slot zero. The second slot remains sparse to satisfy the current renderer handshake. The experiment measures resident buffers, including the extra copy, with the same renderer and FD count in both modes.

The probe samples itself, the renderer, WebProcess and NetworkProcess after confirmed frames. Timings cover submission through receipt of the committed frame, including the private copy where applicable. Open includes process startup and handshake. Hashing and memory sampling follow the measured interval. The PSS column is the median of each run's largest observed sum. RSS high-water values are sums of per-process maxima and can include shared pages more than once. Treat the PSS as a set of observations at frame checkpoints. Nickel and the Qt reader's own allocations are outside this standalone comparison.

| WPE baseline on physical Forma, FW 4.38.23852 | Two SHM slots | One touched slot with copy |
| --- | ---: | ---: |
| Median open | 939 ms | 979 ms |
| Median per-run turn median | 170 ms | 211 ms |
| Median preview | 304 ms | 313 ms |
| Median font reflow | 325 ms | 344 ms |
| Median highest sampled tree PSS | 120,335 KiB | 115,464 KiB |
| Median sum of RSS high-water marks | 211,172 KiB | 199,296 KiB |

The device repeated the comparison with ELF-map logging. Turn medians were 178 and 220 ms, and the highest sampled tree PSS medians were 117,837 and 115,422 KiB. The native Linux run showed higher sampled PSS with the copy and only a small turn difference. Device samples show a small PSS benefit with a consistent turn penalty. Keep two SHM slots. Both modes produced matching page hashes within each platform, returned to the probe's starting descriptor count and reaped their helpers. The physical run kept the same Nickel PID. The standalone probe ends at frame publication.

## Forma reader and display

A user-driven run of the installed `53ef80b9` WPE reader produced 12 matching turn frames and paint records. The median input-to-painted event was 339 ms (310–1,067 ms). Median render and paint portions were 275 ms and 60 ms. The median native display-flush span within those paint windows was 0.75 ms. The trace measures paint and driver submission. Physical panel completion remains unmeasured.

Within a spine item, the median turn was 337 ms. The observed crossing took 1,067 ms, with 986 ms before paint and 81 ms in paint. The renderer retains one DOM. A same-spine turn reuses it, while a crossing loads another publication URI and waits through its resource requests, CSS and font readiness, layout and first snapshot. The trace groups these operations in one render interval. The controlled probe separately reproduced the gap, with a 567 ms median for a new spine item versus 177 ms within the retained DOM using two slots.

## Firmware floor

Kobalt documents 4.38.21908 as the FW4 application minimum. The [Kobo7 package](https://cdn.kobo.com/downloads/firmwares/kobo7/Aug2023/kobo-update-4.38.21908.zip) contains the Forma's `4.1.15-00287-gc98fdced63d6` kernel. The [Kobo4 package](https://cdn.kobo.com/downloads/firmwares/kobo4/Aug2023/kobo-update-4.38.21908.zip) contains `2.6.35.3-850-gbc67621+` images. The two archives contain byte-identical ARM hard-float loader, libc, libm, libpthread, libdl, librt and libresolv files. Their libc exports symbol versions through `GLIBC_2.18`.

The baseline ran the complete retained-text probe in network-disabled QEMU with the stock Kobo7 and Kobo4 loaders and libraries, using Cortex-A9 and Cortex-A8 respectively. Emulator timings include translation overhead. This proves the observed userspace path on the firmware floor. The physical Forma run used the same kernel revision found in the Kobo7 archive, with the installed 4.38.23852 userspace. The Kobo4 kernel was inspected from its boot image and still needs an actual WPE runtime test on that kernel.

## Build and staging

Use the pinned tools image and the three dependency entries from [WPE-M1](WPE-M1.md). Pass `-DKOBALT_WEBKIT_MINIMAL=ON` and a separate `KOBALT_WEBKIT_OUTPUT` to `dependencies/webkit/configure-fw4.cmake`. The minimal FW4 build defaults to `-O2` after PERF-M1. Add `-DKOBALT_WEBKIT_OPTIMIZATION=1` to reproduce the measurements below. The variant settings are included in the output key. Build and strip-install that WebKit entry, then configure the standalone WPE project against it with `KOBALT_WEBKIT_EMBEDDED_GLIB=ON` and `CMAKE_SKIP_RPATH=ON`. Build `wpe-book-renderer` and `wpe-footprint-probe`.

Run `platforms/webkit/footprint/package-fw4.sh WEBKIT-ENTRY RENDERER GLIB-ENTRY TEXT-ENTRY RUNTIME-ENTRY NEW-OUTPUT` for the test archive. It stages the ARM renderer, both helpers, the WPE library, the injected bundle, and the four Noto Serif faces. It includes an ELF inventory, notices for the static and private dependencies, the source pins and patches, and a SHA-256 manifest. The archive recipe fixes entry order, permissions, ownership and timestamps for repeat bundling from the same build inputs.

The baseline ELF closure is the stock ARM loader and firmware libc, libm, libdl, libpthread, librt and libresolv, plus the private WPE library. The retained-text run mapped the injected bundle from the private library directory. It mapped no external EGL, OpenGL, DRM or GIO module. This is the closure observed under the controlled EPUB workload. WebKit, the ICU data and text and image libraries are included in the private WPE build. The archive's notices and pinned source inventory cover the configured inputs, including toolchain runtime notices.

## Minimal runtime results

The rebuilt private library is 94,084,456 bytes, 212,864 bytes smaller than the 94,297,320-byte baseline. The candidate archive is 44,964,118 bytes and reproduced byte for byte from the same compiled inputs. The highest required ABI version remains `GLIBC_2.18`. The offline Kobo7 and Kobo4 runs passed retained pages, preview, chapter changes, reflow, helper shutdown and the Unicode source-location checks. The loaded-object evidence includes the injected bundle.

The first packaged-device run returned identical first and preview pixels. Fontconfig 2.13 recognizes the XDG prefix but does not resolve the newer relative prefix against the configuration file. The corrected launcher enters the bundle directory before starting the renderer and uses the bundled Noto Serif faces. The corrected layout passed the relocated-font emulator run and the temporary Forma run. Both buffer modes preserved the retained frame and produced the same checked page hashes as the baseline. The renderer, its two helpers and all probe-owned descriptors drained cleanly. Nickel kept its original PID and the existing install stayed in place.

| Forma retained-text probe with two slots | Installed baseline | No-JIT candidate |
| --- | ---: | ---: |
| Median open | 946 ms | 983 ms |
| Median per-run turn median | 177 ms | 182 ms |
| Median chapter load | 567 ms | 558 ms |
| Median reflow | 331 ms | 329 ms |
| Median highest sampled tree PSS | 119,249 KiB | 114,642 KiB |

The PSS ranges overlap (113,448–119,589 KiB baseline, 113,970–118,153 KiB candidate). The no-JIT configuration showed a small binary saving and similar physical timings. The one-slot copy variant again increased the candidate's median turn time, from 182 to 220 ms. The data supports retaining two slots, with the optional no-JIT runtime available for controlled testing.

## Release decision

The owner chose to keep WPE experimental and Blaze as Kobalt's default engine. Keep the two-slot transport and retain the no-JIT bundle for controlled experiments. Public distribution remains deferred pending physical transport and teardown coverage on the intended older kernels, panel-completion measurements, source-distribution review and a security decision for running WebKit without OS process isolation.

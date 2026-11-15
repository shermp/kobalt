# WPE-M5.2 optional reader integration

The WPE feature branch is rebased on Blaze `55870b001b401cb52b73bac53dedb0c39c269fe9`. The rebased M5.1 review is `e5135b2e`. The publication conflict used Blaze's embedded-font size limit with M4.1's package-derived Adobe key.

Koblaze remains the default. Linux desktop can opt in with an absolute launcher path in `KOBALT_WPE_RENDERER`. Kobo also accepts an executable `wpe/run` in the Kobalt profile directory. The normal package contains the host adapter and leaves the optional WPE runtime to the test setup.

The WPE path reuses publication inspection, its fingerprint, `ResourceFactory`, the source lease and M4.1's admitted `WebResources`. The renderer gets only scoped member URLs and bounded chunks returned by that worker-owned scope. A WebKit view-level content security policy limits member loads to the publication scheme. The existing view policy restricts top-level navigation and disables book script markup. The trusted isolated script measures the current chapter and restores its anchor after reflow.

The adapter reuses the socketpair process launcher and the two inherited SHM slots. The client checks the pool, request, document and layout before acquiring an image lease. ReaderSession checks its own revision and source lease before presenting the frame. Coalesced work starts from the last presented location. The source endpoint remains alive until outstanding reads and the closing renderer drain. The window uses its regular paint and e-ink refresh path.

Open, forward and backward turns across the spine, font size and viewport reflow are available. Exact spine-item pages feed local navigation. Whole-book counts and progress remain pending. The read-only path leaves native and Kobalt reading positions and the original appearance record intact. The milestone keeps WPE source anchors in memory. Saved positions, navigation beyond turns, and feature support such as search, annotations and previews are separate milestones. The pool currently admits up to 4 MiB per slot, so a full-resolution Forma viewport is outside this integration check. Renderer death leaves the last frame leased and requires reopening for another turn. This slice uses a controlled EPUB with Bubblewrap disabled for the renderer.

## Acceptance evidence

- The pinned Kobo Qt5 and Qt6 SDKs compiled the host and passed the Kobo adapter suites. The standalone WPE 2.54.2 renderer built for native and stock FW4 armhf.
- Native source checks, default-engine reading and session regressions, publication resource tests and opening-cover UI passed. The WPE ReaderSession run under strict main-thread checks opened Type Tester, turned forward and back, coalesced two font updates, resized and closed during a pending turn. The native retained-view and source-location fixtures also passed.
- VirtualNickel Touch C ran the Qt5.2.1 host under firmware 4.38.23852. The real Kobalt menu changed the font from 26 to 28 pixels and rendered the same spine item and column. Framebuffer captures changed after the turn and after reflow. Exit returned to a responsive Nickel library. The renderer, WPEWebProcess and WPENetworkProcess were present before exit and absent afterward. The run removed the instance and system volume. No physical Forma run was needed.

## Reproduce

From the prepared WPE tools image, build both variants of the standalone renderer.

```sh
cmake --build build/webkit-m3.2/native --target wpe-book-renderer wpe-retained-fixture
cmake --build build/webkit-m3.2/fw4 --target wpe-book-renderer
ctest --test-dir build/webkit-m3.2/native -R '^wpe-source-location$' --output-on-failure
```

In the pinned Linux SDK, prepare a launcher for the native WPE install and give it its own runtime library path. Run the targeted ReaderSession test with a prepared Qt headless platform.

```sh
cmake --build build/desktop-linux --target kobalt-reader-session-tests kobalt-desktop-tests \
  kobalt-reader-ui-tests kobalt-local-resources-tests kobalt-zip-resource-tests
ctest --test-dir build/desktop-linux \
  -R '^(session\.reader-session|model\.(local-resources|zip-resource)|ui\.openingCoverLayout|desktop\.reading)$' \
  --output-on-failure
QT_QPA_PLATFORM=minimal:enable_fonts:freetype KOBALT_STRICT_MAIN_THREAD=1 \
  KOBALT_WPE_RENDERER="$PWD/build/webkit-m5.2/run-native" \
  build/desktop-linux/kobalt-reader-session-tests --webkit \
  "$PWD/tests/fixtures/trevelyans-type-tester.kepub.epub"
cmake -P platforms/checks/source-checks.cmake
```

Build and check both pinned Kobo SDKs with `platforms/kobo/build.sh qt5 --check` and `qt6 --check`. For the FW4 UI gate, use the Qt5 diagnostic package in a disposable offline VirtualNickel Touch C 4.38.23852 instance. Put `wpe-book-renderer` and the M2 WPE `lib/` and `libexec/` in the instance's onboard `.adds/kobalt/wpe/`. Set `WEBKIT_EXEC_PATH` and `WEBKIT_INJECTED_BUNDLE_PATH` to those directories in `wpe/run`, and execute the renderer under `qemu-arm` with the stock armhf loader and those libraries. The test launcher supplies a Fontconfig file pointing to the staged Noto Serif regular, bold and italic fonts. Enable the Kobalt diagnostics marker in the disposable profile.

Open Type Tester from the Nickel library, wait for `rrPage`, tap forward and wait for an updated frame. Continue into text, open the reader menu and tap `rrWpeFontLarger`. The `wpe-frame` event identifies the font and chapter of the frame. Use `rrReturn` and wait for the closed session. Verify Nickel responds and that the renderer and both WebKit helpers have exited. Remove the disposable instance and its volume. Run evidence is under `build/webkit-m5.2/`.

M5.2 staged NO, committed NO, pushed NO. M6 started NO.

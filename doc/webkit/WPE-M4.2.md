# WPE-M4.2 retained paginated WebView

The retained-view fixture passed on native WPE and a booted VirtualNickel Nia running FW4. Both runs produced identical BGRA hashes at every checked step. The code lives under `platforms/webkit/retained/`.

The view keeps one WebKitWebView and the current chapter's DOM. The owner supplies a local chapter URI and a bounded viewport, column and font size. The view uses a native user stylesheet and resize API for pagination. A small, versioned script world waits for the requested viewport, applies the font size, verifies the font and image loads, measures the column extent and scrolls to the requested column. A visible WebKit snapshot completes the operation. Only a new chapter URI triggers a load. If a request fails or is cancelled, its callback drains before the view restores the last confirmed settings and scroll position. If restoration itself fails, the owner must replace the view.

The fixture stages two chapters, CSS and a raster image from the checked-in Type Tester EPUB via M4.1's pinned `WebResources` scope. It also stages the existing Amstelvar fixture font. The standalone renderer serves the exact staged resources from an in-memory scheme provider and never opens the EPUB ZIP. The provider counts chapter requests and confirms the second chapter is requested only when selected. Pre-staging these bounded test inputs is fixture setup. The renderer loads and lays out only the active chapter.

WebKit 2.54.2 permits the trusted isolated-world bridge while `enable-javascript-markup=false` suppresses page-authored scripts. The fixture verifies that an inline script leaves no mutation. Its controlled documents use a restrictive CSP and deny non-candidate top-level navigation. The test asserts that an external stylesheet attempt never starts a WebKit resource load, and VirtualNickel also applies its offline socket filter. This is a controlled fixture boundary. The scheme provider is not yet connected to the socket/SHM transport or the live publication worker. General EPUB sanitization and navigation policy need review before untrusted books can use the unsandboxed runtime.

## Acceptance evidence

| Check | Native and VirtualNickel result |
| --- | --- |
| First frame | The red swatch, delayed green image and text pixels were visible. The bridge required a loaded font face. |
| Same-view turn and reversal | Red → blue → red matched the expected swatch pixels. The two red hashes matched exactly. The fixture URI was fetched once. |
| Font reflow | Font size 18 → 26 changed the glyph pixels and retained the current column. |
| Invalid turn and cancellation | The invalid column and immediate cancellation published no frame. The next successful snapshot matched the frame before both failures, byte for byte. |
| Resize | The snapshot changed from 160 × 120 to 208 × 136 and a subsequent turn showed the expected blue column. |
| Real EPUB reading | Two different columns of Type Tester chapter 1 rendered text and produced different hashes without a reload. Chapter 2 rendered after its first request. |

The Nia used FW 4.38.23852, the stock armhf glibc 2.19 loader, QEMU Cortex-A9, and the existing M1 WPE 2.54.2 helper and library closure. Its Fontconfig default still warned. The fixture's explicit embedded font loaded and rendered despite that warning. System font discovery, retained source locations across reflow, long-run performance and the physical Forma were outside this result. Nickel responded after the test. The disposable simulator, container and system volume were removed.

## Reproduce

Use the M4.1 desktop build in the prepared Linux SDK. Stage fresh fixture inputs:

```sh
cmake --build build/desktop-linux --target kobalt-webkit-export-fixture
mkdir -p build/webkit-m4.2/resources
build/desktop-linux/kobalt-webkit-export-fixture \
  tests/fixtures/trevelyans-type-tester.kepub.epub \
  tests/fixtures/AmstelvarAlpha-VF.ttf build/webkit-m4.2/resources
```

Use the existing [M3.2 native and FW4 configurations](WPE-M3.2.md) in the M1 tools image. Build the new executable in each:

```sh
cmake --build build/webkit-m3.2/native --target wpe-retained-fixture
cmake --build build/webkit-m3.2/fw4 --target wpe-retained-fixture
LD_LIBRARY_PATH="$prefix/lib" WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 \
  timeout 45 build/webkit-m3.2/native/wpe-retained-fixture build/webkit-m4.2/resources
```

For the FW4 run, prepare and boot a new Nia 4.38.23852 with `--display-backend offscreen --no-viewer` as in [M0](WPE-M0.md). After the current boot exists, wait for `library-ready 1`. In the instance's `artifacts/retained/`, stage the FW4 executable, exported `resources/`, M3.2 `stock/` loader tree and the M2 package's `lib/` and `libexec/`. Then execute the following in the running container:

```sh
bundle=/instance/artifacts/retained
export QEMU_LD_PREFIX="$bundle/stock" QEMU_CPU=cortex-a9
export WEBKIT_EXEC_PATH="$bundle/libexec" WEBKIT_INJECTED_BUNDLE_PATH="$bundle/lib"
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
/usr/bin/vn-offline timeout 45 qemu-arm -U LD_PRELOAD -E "LD_LIBRARY_PATH=$bundle/lib" \
  -L "$bundle/stock" "$bundle/wpe-retained-fixture" "$bundle/resources"
```

Compare the `retained step` lines, verify Nickel responds, then stop and delete only the disposable instance. The captured logs are `build/webkit-m4.2/native.log` and `build/webkit-m4.2/virtualnickel-final.log`. `cmake -P platforms/checks/source-checks.cmake` also passed.

WPE base `8b83d95b83f503451c987d51c765e9bbb7dd16a1`, local Blaze `a4bb4d1cf86acaa404064dca184a0e6708a43b00`. One GPT-6.1 Sol agent. M4.1 was committed as authorized. M4.2 staged NO, committed NO, pushed NO. M4.3 started NO.

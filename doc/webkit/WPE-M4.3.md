# WPE-M4.3 locations and page model

The retained renderer captures a chapter-relative source anchor with each successful snapshot. The anchor carries the Blaze v1 href, zero-based body-rooted element-child path, counted Unicode code-point offset and affinity. The isolated DOM bridge translates JavaScript UTF-16 positions, distinguishes the two sides of omitted characters and reports exact, approximate or unresolved restoration. A retained view uses its confirmed anchor for same-chapter viewport and font reflow. Failed restoration leaves the previous frame and page information confirmed.

The caller can query the current chapter's zero-based column and measured column count. A separate per-spine PageModel records observations for the current layout. It estimates one chapter per indexing call using its byte count and the ratio from measured chapters. Each call can be cancelled before processing its unit. Indexing neither requests chapter content nor lays it out. The whole-book total stays unknown while any chapter lacks a count, becomes estimated when all entries have estimates or measurements, and becomes exact when all are measured. The caller owns publication identity and resets the model when layout changes.

## Acceptance evidence

| Check | Native WPE and booted VirtualNickel FW4 |
| --- | --- |
| Source map | JavaScriptCore and C++ checks passed for astral characters, logical RTL offsets, combining marks, whitespace gaps across inline elements, element paths and all restoration outcomes. |
| Whole-book model | Unknown, estimated and exact totals, the per-chapter bound, cancellation before an indexing unit and layout invalidation passed. |
| Chapter return | Type Tester chapter 1 restored to zero-based column 2 after navigating to chapter 2. Its 208 × 136 snapshot matched the original byte for byte on each runtime, with SHA-256 `1a4805a365d32aebde37e4e0cb2cebb964731a5145f3e7d58f37cbf22de10b29` on both. |
| Reflow and invalid anchors | The anchor resolved after shrinking the viewport to 160 × 120. A missing element path rolled back and an offset beyond the source text reported approximate restoration. |
| RTL | The saved logical offset restored in the first column before and after increasing the font size. The fixture verified that the embedded Hebrew font was requested. |
| Second publication | The Figure Caption EPUB emitted its own chapter anchor. A Type Tester chapter href failed to resolve there and the second book's confirmed page was preserved. |

The reported Type Tester chapter 1 count at 208 × 136 was 100 on native and 101 on FW4, while the checked frame hashes agreed. The two RTL frame hashes differed between runtimes even with the embedded Hebrew font. The FW4 run reported a missing default Fontconfig configuration. RTL geometry was exercised in the first column. The tests compare the source contract against Blaze's implemented rules and do not run a Blaze-to-WPE round trip. The caller must bind locations to the publication fingerprint, including when two books share the same href. The standalone fixtures exercise WPE 2.54.2. FW4 used VirtualNickel Nia 4.38.23852, the stock armhf loader and QEMU Cortex-A9. Nickel responded before and after the offline tests. The run removed the disposable instance and system volume. No physical Forma test was run.

## Reproduce

In the prepared Linux SDK, stage both admitted EPUB fixtures and their fonts.

```sh
cmake --build build/desktop-linux --target kobalt-webkit-export-fixture
mkdir -p build/webkit-m4.3/resources
build/desktop-linux/kobalt-webkit-export-fixture \
  tests/fixtures/trevelyans-type-tester.kepub.epub \
  tests/fixtures/AmstelvarAlpha-VF.ttf build/webkit-m4.3/resources \
  tests/fixtures/figure-caption.epub
cp assets/fonts/NotoSerifHebrew-Regular.ttf build/webkit-m4.3/resources/hebrew.ttf
cmake -P platforms/checks/source-checks.cmake
```

In the M1 tools image with the [M3.2 native and FW4 configurations](WPE-M3.2.md), build and test with the native WPE install prefix in `prefix`.

```sh
cmake --build build/webkit-m3.2/native --target wpe-retained-fixture wpe-location-check
cmake --build build/webkit-m3.2/fw4 --target wpe-retained-fixture wpe-location-check
export LD_LIBRARY_PATH="$prefix/lib"
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
ctest --test-dir build/webkit-m3.2/native -R '^wpe-source-location$' --output-on-failure
timeout 45 build/webkit-m3.2/native/wpe-retained-fixture build/webkit-m4.3/resources
```

Follow the [M4.2 VirtualNickel procedure](WPE-M4.2.md). Stage both FW4 executables and the new resource directory in `artifacts/location/` with the M3.2 stock loader and M2 package's `lib/` and `libexec/`. Boot a disposable Nia 4.38.23852, wait for `library-ready 1`, and run inside the container.

```sh
bundle=/instance/artifacts/location
export QEMU_LD_PREFIX="$bundle/stock" QEMU_CPU=cortex-a9
export WEBKIT_EXEC_PATH="$bundle/libexec" WEBKIT_INJECTED_BUNDLE_PATH="$bundle/lib"
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
/usr/bin/vn-offline timeout 45 qemu-arm -U LD_PRELOAD -E "LD_LIBRARY_PATH=$bundle/lib" \
  -L "$bundle/stock" "$bundle/wpe-location-check"
/usr/bin/vn-offline timeout 45 qemu-arm -U LD_PRELOAD -E "LD_LIBRARY_PATH=$bundle/lib" \
  -L "$bundle/stock" "$bundle/wpe-retained-fixture" "$bundle/resources"
```

Verify Nickel responds again and delete the disposable instance. Logs are available in `build/webkit-m4.3/native.log` and `build/webkit-m4.3/virtualnickel.log`.

WPE base `cbd7288548035b800af8af024f54b9ae9b21f676`, local Blaze `a4bb4d1cf86acaa404064dca184a0e6708a43b00`. M4.3 staged NO, committed NO, pushed NO. M5.1 started NO.

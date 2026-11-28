# WPE-M7 current-page links and notes

The links and notes slice builds on WPE `cee695b64ea9a9f89cbe4f534abbfa92ff8632b1` and Blaze `55870b001b401cb52b73bac53dedb0c39c269fe9`.

The retained view supplies clipped rectangles for admitted internal links. It identifies same-chapter EPUB footnote and endnote destinations and explicit EPUB note references, including references to another spine item. Kobalt applies its existing Tappable links setting. Footnote-only mode opens marked notes, and Highlight links exposes ordinary passages.

The reader resolves note previews on the existing publication worker with the active source lease. It reads the admitted XHTML spine item, looks up the decoded fragment and sends escaped text to the existing footnote panel. A preview leaves the confirmed reading position alone. Show in book uses the existing navigation and return stack. A page change invalidates a pending preview. Empty, missing, malformed and oversized previews fall through to ordinary internal navigation. Cross-chapter destinations without an explicit note reference remain ordinary passages. The preview keeps paragraph breaks and omits publisher markup. It does not lay notes into the page. Web and executable links remain outside the WPE page-link path.

The private transport uses a high bit in the link length field to mark note rectangles. Update the host adapter and renderer together. Reading checkpoints and annotation storage are unaffected.

## Validation

Build and check the matching host and renderer using the prepared SDKs described in [M5.2](WPE-M5.2.md). Then run:

```sh
cmake -P platforms/checks/source-checks.cmake
ctest --test-dir build/webkit-m3.2/native -R '^wpe-source-location$' --output-on-failure
ctest --test-dir build/desktop-linux \
  -R '^(engine.links|desktop.(links|tappableLinks|inpageFootnotes)|session.reader-session|model.(local-resources|zip-resource))$' \
  --output-on-failure
QT_QPA_PLATFORM=minimal:enable_fonts:freetype KOBALT_STRICT_MAIN_THREAD=1 \
  KOBALT_WPE_RENDERER="$PWD/build/webkit-m5.2/run-native" \
  build/desktop-linux/kobalt-reader-session-tests --webkit \
  "$PWD/tests/fixtures/trevelyans-type-tester.kepub.epub"
```

The native WPE regression uses the existing link EPUB fixture. It checks note classification and tap modes, direct passage details, a cross-chapter note whose filename contains a space, same-chapter note markup, HTML escaping, oversized and missing notes, rejection of external links, stale preview cancellation and return to the reference. The native default-engine suites, retained source-location fixture, and both Kobo SDK package checks and adapter suites passed. The host Qt5 and Qt6 syntax check required the native container's dependency include paths to be supplied at their corresponding host locations.

On VirtualNickel Touch C with firmware 4.38.23852 and stock Qt 5.2.1, a direct tap opened the Footnote panel without producing another WPE frame. Show in book loaded the note's spine item. Return to passage produced the original framebuffer hash. Closing the book shut down the renderer and both WebKit helpers, and Nickel responded. The disposable instance and volume were removed. The test used a two-chapter EPUB with an explicit inter-chapter note reference. Evidence is in `build/webkit-m7/`.

M7 selection geometry, ranges, highlights, search, previews and stress validation are subsequent work.

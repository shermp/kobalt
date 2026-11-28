# WPE-M6 persistence and navigation

M6 builds on WPE commit `c551f078e90021dcff5b9b1f3caae1bfed336544` and Blaze `55870b001b401cb52b73bac53dedb0c39c269fe9`.

The existing publication scope now exposes OPF title and author and reads the admitted EPUB 3 navigation document or EPUB 2 NCX. The reader uses its existing chapter and bookmark panels. WebKit resolves fragments in the retained isolated world and returns bounded rectangles for admitted on-page links. The existing reader return stack records a source anchor and can restore it across a chapter load or reflow.

WPE uses the existing book checkpoint writer to add a fingerprint-bound `Location` companion to the existing book record. It keeps the original XPointer and `dom_version`. Koblaze writes a shared location alongside its native position, and checks the companion when switching from WPE. A companion is ignored if its publication, version, or recorded native checkpoint no longer matches. WPE saves the observed Nickel baseline in Kobalt’s record so that unchanged unread status does not erase WPE progress on the next opening. No change is made to Nickel progress.

WPE bookmarks use the existing bookmark store and its transactions. A bookmark created by WPE can jump in either engine. Koblaze keeps its native bookmark positions. An XPointer-only bookmark or checkpoint remains usable in koblaze until that engine supplies a shared location. Old PoC CFI strings are preserved as opaque input. The active WPE bookmark ribbon matches a confirmed source position and may need a jump to find an older bookmark after repagination.

## Validation

Run the prepared WPE tools and Kobo SDK builds from the [M5.2 procedure](WPE-M5.2.md). Run the default-engine reading, session, bookmarks and publication regressions in the pinned Linux SDK:

```sh
ctest --test-dir build/desktop-linux \
  -R '^(session\.(reader-session|reading-book|bookmark-session)|model\.(local-resources|zip-resource)|ui\.openingCoverLayout|desktop\.reading)$' \
  --output-on-failure
QT_QPA_PLATFORM=minimal:enable_fonts:freetype KOBALT_STRICT_MAIN_THREAD=1 \
  KOBALT_WPE_RENDERER="$PWD/build/webkit-m5.2/run-native" \
  build/desktop-linux/kobalt-reader-session-tests --webkit \
  "$PWD/tests/fixtures/trevelyans-type-tester.kepub.epub"
```

The explicit WPE run exercises the EPUB 3 TOC, the existing NCX songbook fixture with fragment links, source navigation and return, a font and viewport change, saved-position reopening, and switching between engines. It seeds an existing XPointer and `dom_version`, checks the retained values, and verifies that stale and unsupported optional records are preserved.

For VirtualNickel, install the Qt5 diagnostic build and M6 renderer as in M5.2. Open Type Tester, select Chapter One in Contents, tap the bookmark corner, and close using Exit Kobalt. Reopen the book, select another chapter and select the saved bookmark from the existing list. Inspect the `wpe-frame` and `bookmarks-saved` events. Verify the renderer and its helpers exit when the reader closes. Evidence is kept under `build/webkit-m6/`.

## Acceptance evidence

- Native source checks, retained rendering, source locations, and the focused default-engine regressions passed. The explicit WPE regression passed with strict main-thread checks. It exercised both engine switches and verified preservation of the existing XPointer and DOM version.
- The pinned Kobo Qt5 and Qt6 builds and adapter suites passed.
- In offline VirtualNickel Touch C on FW 4.38.23852, Kobalt jumped from the cover to Chapter One, saved a bookmark, closed, reopened at Chapter One, and followed the bookmark back from Chapter Two. Reopening and following the bookmark produced the same captured frame hash. Nickel responded after close and the WPE renderer and helper processes exited. The disposable instance and volume were removed.

- On the Forma with firmware 4.38.23852, the owner confirmed the WPE-enabled Kobalt build worked. The build raised the per-slot frame limit to 16 MiB to fit the Forma’s 1440 × 1920 display. Portrait and landscape frame bounds and the Qt5 Kobo adapter suite passed before installation.

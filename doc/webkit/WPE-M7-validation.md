# WPE-M7 validation

M7 was built against Blaze `55870b001b401cb52b73bac53dedb0c39c269fe9`. Both pinned Kobo SDKs passed the Qt5 and Qt6 package checks and adapter suites. The native and FW4 ARM WPE renderers built and the retained source-location fixture passed.

## Reader behavior

- Current-page EPUB footnotes and internal links, single-word dictionary lookup, temporary match highlights and adjacent-page previews use the confirmed frame. The preview leaves the reading position and displayed pixels intact.
- Annotation mode saves and recolors multiword highlights through the existing annotation writer. Renderer geometry supplies the visible range and its endpoints. The native regression covered backward drag, overlap rejection and redraw after reopen. WPE handle adjustment is limited to the active spine item and displayed geometry. Saved ranges the active engine cannot resolve are retained.
- Search finds text across inline markup and supports whole-book, current-spine, before-page and after-page scopes. The native fixture checked a phrase split by an emphasis element and compared before/after results within a multi-page spine item.
- WPE supplies an exact whole-book page count once every spine item has a confirmed layout at the current settings. The current absolute page number becomes available once the counts of the preceding items are known. A preview supplies neither. Reflow invalidates those counts.

## Stress and platform coverage

The native strict-main-thread suite opened a 180-entry EPUB, navigated deep into it, turned repeatedly in both directions, and verified the restored source position and pixels. It also checked close with a preview outstanding, renderer exit and descriptor counts across repeated open and close cycles, and recovery from a renderer killed during a pending turn. Focused default-engine reader, link, annotation, dictionary, search and storage tests passed.

A disposable VirtualNickel Touch C on firmware 4.38.23852 opened a word dictionary, navigated to a search result in the second chapter, and exposed Navigate with the exact totals still unknown. Closing returned to responsive Nickel and removed all three WPE processes. A separate firmware 4 run selected two words, moved the end handle into a third and saved the highlight. All three words showed the overlay after reopening.

On the physical Forma (firmware 4.38.23852, kernel 4.1.15), a disposable renderer completed three 1440 × 1920 sessions. Each checked retained pixels, adjacent preview, repeated page turns, reflow and shutdown. Nickel kept the same PID throughout. The probe used the installed WPE libraries and font configuration and ran its own renderer from temporary storage.

A standalone ARM renderer passed the same three-session probe alongside Nickel in VirtualNickel on Clara Colour firmware 5.18.270971 and 6.0.274403. The first integrated Kobalt runs fell back to Nickel after a failed session-marker write. A disposable emulator shim returned `EOPNOTSUPP` for `O_TMPFILE` in the Kobalt profile, allowing Qt to use its named temporary-file path for atomic writes. With the shim, both firmware versions opened the WPE reader, turned a page, closed to responsive Nickel and reaped the renderer and its helpers. The same Kobalt packages were used throughout. All disposable VirtualNickel instances and volumes were removed.

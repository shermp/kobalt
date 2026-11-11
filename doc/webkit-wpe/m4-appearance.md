# M4 appearance previews

The existing Text and Layout panels can preview the appearance settings
already supported by WebKit, including text size, line spacing and side
margins. A panel opened at the reading viewport uses the displayed pixels
and markup as its initial preview. The first edit reuses the warm WebKit
helper when the draft and book are on the same layout and source point.

The book and its draft keep their own frame and layout state. They share
one live helper on the document worker. Edits paginate the displayed spine
item. Applying the draft uses the latest settings to paginate the book and
restore the same source anchor. Switching between incompatible layouts
reaps the previous helper before starting another. A book sleep or close
can also reap the helper while the draft owns it.

The appearance edits apply to the current opening. [Vertical margins](m4-margins.md)
connects the existing Layout control. Alignment, hyphenation and the full
font catalog remain part of the typography work.

## Verification

The native layout and appearance checks cover the shared panel lifecycle.
The WebKit proxy test verifies that warming and editing a draft reuse the
existing helper, that only apply scans the book, and that sleep reaps a
helper owned by a draft. The ARM reader session changes a side margin on
the Trevelyan fixture. It verifies a distinct draft frame, an unchanged
reading frame before apply and matching pixels at the same source anchor
after apply.

The installed check exercises the Text size and side-margin controls using
the production panels on a disposable VirtualNickel device.

```sh
python3 tests/webkit/run-firmware.py \
  --virtualnickel /path/to/VirtualNickel \
  --installed-only --installed-appearance \
  --allow-unsandboxed-experiment
```

The paired review in `build/review/webkit-m4-appearance/` shows the entry,
both edited panels and the applied page. The Text size and side-margin
rows have matching bounds, caption and value alignment, and rendered
controls in both backends. The Text tab's Aa sample uses the interface font
in WebKit. The excerpt above the Text panel changes after the size edit
in both engines.

On the installed emulator, the WebKit text-size draft rendered in 518 ms
and the side-margin draft in 304 ms. Applying the combined layout took
8.7 seconds, including restarting the helper and paginating the book.
The run peaked at 682 MiB under the 768 MiB limit and logged no
main-thread blocks. Both pinned Kobo SDK checks passed. The host syntax
check requires `clang++`, which is unavailable on this host.
See [recorded results](m4-appearance-results.json).

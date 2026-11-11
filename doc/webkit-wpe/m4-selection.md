# M4 pointer selection and annotation editing

The WebKit document now supplies annotation tokens, resolved selection ranges
and page edges to the shared reader editor. A word hold selects text for
dictionary lookup. In highlight mode, the same hit testing feeds the draft
outline, handles, mark choices and saved-highlight actions. The document worker
queries the helper without replacing the displayed image or reading position.

## Selection behavior

Pointer hits use WebKit's caret location and visible glyph rectangles. A
punctuation hit selects that mark directly. A rendered space can select the
preceding word, and blank paper has no token. When extending a handle, the
terminal edge of the candidate word determines whether the drag has passed
it. The helper can keep the last complete word if movement skips the space
and enters the following word. Inline bidi handles use the glyph's outward
normal.

The range query sorts the chosen tokens into source order and preserves the
start and end policies with their respective boundaries. It can add terminal
punctuation when the selection includes the full final sentence. It includes
paired marks only when the relevant opener and closer are selected together.
Explicit punctuation hits preserve their exact boundaries. The policy leaves
uncertain endings, such as ellipses and abbreviations, at the chosen boundary.
Word and sentence segmentation use the pinned WebKit runtime's
`Intl.Segmenter`. Reopening an older exact range retains its endpoints during
adjustment until the reader moves them.

Page-edge queries return the first and last selectable tokens and their true
edges. The helper verifies that an adjacent glyph belongs to the next or
previous rendered page in the same semantic section. The proxy also checks the
spine entry and resolved table of contents. The shared editor uses those results
to stop at section boundaries and to finish one page extension per gesture.

The existing annotation writer persists source anchors and optional chosen
boundaries in the source namespace. Adjusting a saved highlight keeps its ID.
Source and CREngine lists retain their separate storage keys and formats.

## Validation and review

The native annotations lane and focused proxy checks cover the shared reader,
source anchor validation, punctuation provenance and TOC boundaries. Both
prepared Kobo SDK checks passed. The host syntax script requires `clang++`,
which is unavailable on this host.

The standalone ARM EPUB diagnostic checks actual hit geometry, reversed
selection, balanced punctuation, conservative sentence endings, whitespace,
far-edge dragging and continuation boundaries. The production ARM session
saves a pointer selection and reloads its exact chosen boundaries after reflow.
The installed diagnostic exercises a hold and drag, the mark choices, the saved
end handle, annotation list isolation and a cold reopen on Libra Colour
firmware in VirtualNickel.

```sh
python3 tests/webkit/run-firmware.py \
  --virtualnickel /path/to/VirtualNickel \
  --installed-only --installed-annotations \
  --allow-unsandboxed-experiment
```

The targeted review compares the existing Annotate entry, draft, mark choices,
saved highlight and annotation list on the same title passage in both engines.
The toolbar control pixels match, and the choice controls match after aligning
their positions. The annotation list shares the tab and row metrics. Its
WebKit excerpt uses the interface font because the reading-font bridge is
still pending. The review data is in `build/review/webkit-m4-selection/`.
The user authorized the commit in chat. At that point the round recorded five
seen screens, one unseen and no requested changes.

An earlier installed run stopped the emulator during the same-boot WebKit
reopen after switching from CREngine. The container exited before the final
memory sample. The recorded samples do not establish the cause. The completed
scenario verifies the adjusted mark after a full Nickel restart using the
same profile. Same-boot recovery remains open.

New ranges stay within one spine entry. Publication pagination currently
supports horizontal LTR text with inline bidi. Vertical and RTL publication
layout, graphemes split across text nodes and physical e-ink touch behavior
need further validation. The offline firmware experiments use an explicit
unsandboxed mode. See [the recorded evidence](m4-selection-results.json).

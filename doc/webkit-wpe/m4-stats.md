# M4 reading statistics

WebKit supplies the existing Stats panel with a book word total and chapter
lengths in words. The document worker counts visible source text while each
spine item is loaded for pagination and table of contents resolution. The
helper returns totals and prefixes at the chapter anchors in metadata replies.
The displayed page and source locator stay in place. The document retains the
counts through reflow and helper recovery while the book is open.

The count follows Kobalt's logical word rules across inline markup, including
joining internal apostrophes and hyphens and counting Han and kana letters
individually. Visibility follows WebKit's layout. The total includes
non-linear spine items and text before the first chapter anchor. Hidden text,
SVG text and ruby glosses are excluded. Chapter intervals follow source order
and assign the interval at a duplicate destination to the last matching TOC
entry. An unavailable count leaves the page-length chart available.

The shared session clock measures active reading time and supplies the existing
session pace estimate from forward page turns. The clock pauses when the
reader is hidden or suspended. The Stats data and pace live in memory. Source
position checkpoints continue to use the existing background writer and the
independent WebKit book-state slot.

## Verification

The native progress lane checks the shared reading clock, estimates and
progress behavior. The WebKit proxy check exercises source-ordered chapter
intervals across a non-linear spine item, duplicate destinations, reuse of
counts and fallback to the page-length chart for malformed replies. The
standalone firmware check exercises inline word boundaries, Unicode,
exclusions and prefixes. The ARM reader session checks the Trevelyan fixture's
7,316 words, reflow, active time and preservation of the existing saved record.
Both pinned Kobo SDKs passed their adapter checks and the Qt6 diagnostic
package passed its archive check. The host lacks the syntax check's
`clang++`.

The existing firmware runner can check Stats on a disposable VirtualNickel
device.

```sh
python3 tests/webkit/run-firmware.py \
  --virtualnickel /path/to/VirtualNickel \
  --installed-only --installed-stats \
  --allow-unsandboxed-experiment
```

The installed Libra Colour check showed 7,316 words in WebKit's Stats
panel and recorded active time in both engines. CREngine displayed Nickel's
remaining-time estimate in the Book cell. The shared Stats entry, chapter
title, cell captions and session figure matched pixel for pixel within their
widget bounds. Each engine uses its own pagination for the remaining pages.
The paired entry and panel captures are in
`build/screenshots/webkit-m4-stats/`. The review is in
`build/review/webkit-m4-stats/`. The user reviewed both screens and recorded
no changes.

The initial separate count pass took 6.5 seconds on the emulator. Counting
during pagination and TOC resolution removed the extra spine loads. The
final installed run opened the fixture in 12.6 seconds and reached a 689 MiB
peak under the emulator's 768 MiB limit. Physical-device opening time and
memory use remain unmeasured. See [recorded results](m4-stats-results.json).

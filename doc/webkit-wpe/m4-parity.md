# M4 reading-feature verification

The M4 candidate runs in the existing Kobalt reader on the experimental
WebKit backend. The checks cover reflowable EPUBs on Kobo firmware 6.x.

| Feature | Automated evidence |
| --- | --- |
| Reading positions and Chapters | Cold reopening, source assertions, fragment navigation, chapter counts and exact native and CREngine import. [Source positions and Chapters](m4.md), [Position import](m4-position-import.md) |
| Bookmarks, highlights and annotations | Storage isolation, exact handles, continuation, edits, overlap, reflow and reopening. [Source bookmarks](m4-bookmarks.md), [Annotation ranges](m4-ranges.md), [Pointer selection](m4-selection.md) |
| Search and text extraction | Normalized matching, source geometry, hits across spines and frame preservation. [Source text queries](m4-text.md) |
| Statistics and progress | Word counts, reading intervals, time estimates and cold restoration. [Statistics](m4-stats.md), [History](m4-history.md), [Pace](m4-pace.md) |
| Navigate and appearance | Thumbnail ranges, invalidation, orientation and applying a draft at the same source point. [Page previews](m4-previews.md), [Appearance drafts](m4-appearance.md), [Vertical margins](m4-margins.md) |
| Typography and writing directions | Alignment, thickness, discretionary hyphenation, Arabic RTL and vertical Japanese EPUBs. [Typography](m4-typography.md), [Writing directions](m4-writing-directions.md) |
| Renderer recovery | Helper loss, restoring the displayed page, bounded retention and cleanup. [Recovery](m4-recovery.md), [Memory](m4-memory.md) |

The verification uses the prepared Linux and Kobo SDKs, the standalone
EPUB helper, the production ARM reader session and the installed NickelHook
reader in offline VirtualNickel. The experiments use the same checked-in
representative book as the native suite, with focused fixtures for
particular layout and storage assertions.

The final candidate was checked with both Kobo SDKs, the representative
Linux suite, and the firmware runner's `--epub-only`, `--session-only` and
`--installed-only` actions. The installed runs cover navigation, rotation,
sleep and waking, CREngine fallback, reopening, annotation editing and
recovery from helper loss. The installed tests recorded no main-thread
blocks or out-of-memory events at the 768 MiB emulator limit.
See [recorded results](m4-parity-results.json).

## Limits

WebKit remains opt-in. Existing CREngine and Nickel records retain their
original formats and writeback. WebKit can import compatible checkpoints
and keeps later progress in its own slot. It has its own bookmark and
annotation records. Native writeback and KOSync continue through CREngine.

The Kobo WebKit package still needs compatible dictionaries for automatic
language hyphenation. CREngine-specific variable-font controls and layout
adjustments remain backend-specific. Physical-device performance,
sandbox availability and e-ink verification belong to the later
hardware milestone. The local screen-review rounds remain open for visual
feedback.

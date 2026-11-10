# M4 text queries and search

This slice follows the initial M4 position/Chapters commit `a8e28b1`. It adds
read-only source queries and connects them to the existing Search screen. M4's
complete feature-parity gate remains open.

The targeted round was reviewed without annotations or requested changes. The
user approved committing this slice in chat; the known gaps below remain open.

## Query and frame ownership

`KWRP/1 QUERY <spine> <layout-page> <base64-json>` inspects the selected
content document and layout page. Its JSON request has a `kind`: `word`, `range`
or `search`. The reply is `OK JSON <base64-json>`, with the inspected spine,
page, source URL and result. Query replies retain the protocol request identity
and the proxy's current source lease. They do not allocate or release CPU frames.

The helper reuses the current isolated-world DOM when possible. Inspecting a
different spine or layout page paginates it without taking a snapshot. The
proxy retains its copied image, frame metadata and logical source point. Page
and source jumps remain absolute; an appearance change restores that source
point before using the helper's relative reflow state. No book script receives
the source map or query function.

Requests and replies are bounded. The proxy's complete request line is limited
to 8192 bytes; the helper also bounds decoded query input. Query JSON replies
are limited to 512 KiB, within the 1 MiB protocol output queue and proxy input
bound. Bad query input, unresolved ranges and query exceptions do not exit an
otherwise healthy renderer. Invalid identities, source URLs, locator versions,
package CFI prefixes and rectangle coordinates are rejected at the Qt boundary.

Queries run on the document worker. Search cancellation joins the existing
lease predicate during each IPC call, so cancelling a stuck query interrupts
the wait and reaps its helper. The ordinary lease predicate is restored before
the next job; a following page turn can reopen at the preserved source point.
The GUI does not wait for that cleanup or write storage.

## Text and source ranges

Text queries retain the version-1 source stream and locators defined in
[the initial M4 slice](m4.md). Search builds a separate visible-text map; hidden
text is omitted, inline markup remains contiguous, and block boundaries and
explicit line breaks supply whitespace. Its offsets map back to source UTF-16
boundaries. This does not change saved locators or parsed-book cache formats.

Word queries combine WebKit's caret hit test with ICU word segmentation through
`Intl.Segmenter`. A hit must intersect the word's actual rendered rectangles;
nearby words are not selected from blank paper. Inline text nodes, supplementary
characters and combining sequences retain source boundaries. Range queries
accept either endpoint order, return visible text and clip text-node rectangles
to the current content viewport. Text extraction is bounded to 8192 UTF-16
units and geometry to 512 rectangles.

`WebKitDocument::wordAt`, `textRange` and `rangeRects` now implement those
document contracts. Ranges currently stay within one spine document. The
annotation-driven pointer UI, punctuation policies, drag handles and annotation
writes remain later M4 work; this slice does not enable their persistence.

## Search

Search scans source text in whole-book page order through the existing
`ReaderSession` batches and cancellation machinery. Matches use compatibility
normalization, case normalization, whitespace collapsing, removal of format
controls, and apostrophe/hyphen folding. The normalized index is reused while
the DOM layout remains loaded. Matches spanning inline markup, lines or page
boundaries belong to the page containing their first source character. A
match's original source text and bounded surrounding context are retained.
The existing per-page and overall result limits remain in effect.

The existing Search panel, query editor, native Kobo keyboard, row styles,
chapter captions and search-result highlighting present these results. Opening
a result uses its source token. Returning uses
`ReaderSession::navigationPosition()`, a transient backend-aware token; it
never places a WebKit token in the legacy saved `position` field.

Search indexing and pagination remain worker work. This slice makes no
physical-device latency or memory claim, and does not replace the later M4
preview/cache and renderer-recovery work.

## Verification

[Text-query results](m4-text-results.json) records the exact checks and retained
evidence. The focused native lane covers source binding, geometry validation,
immutable frames, cross-spine search, a stuck-query cancellation followed by a
page turn, reader sessions and existing CREngine search/reflow behavior. Both
prepared Kobo SDKs compile and link the application and run their adapter checks.

The real ARM helper diagnostic covers Unicode, inline markup, hidden text,
explicit line/block breaks, geometry, word hit testing, nonfatal malformed
queries and held frame ownership. The production ARM session checks asynchronous
search, source navigation, range extraction and unchanged displayed state. The
installed diagnostic types through the firmware keyboard and exercises Search
results, highlighted destinations and return navigation on WebKit and CREngine.
These use disposable VirtualNickel devices with the explicit unsandboxed
experiment mode and offline restrictions. They do not alter the retained manual
device or establish physical sandbox behavior.

The targeted review compares Search's surrounding menu, opened panel, results,
destination and return control with the existing CREngine screen. It uses
`build/screenshots/webkit-m4-text/` and `build/review/webkit-m4-text/`. Existing
full-gallery Search scenes remain unchanged. Only the user supplies review
feedback and seen states.

Use the existing firmware runner's `--installed-only --installed-search` action
for this focused check. The ordinary `--installed-only` lifecycle action retains
its previous scope. Reuse the initial M4 rotation/sleep results for unchanged
behavior rather than repeating that complete sequence for each text-query edit.

Two gaps remain explicit. Qt's WebKit excerpt font still falls back to the
interface font, as the M3 typography bridge is incomplete; CREngine's rows use
the book font. The shared control fonts, caption alignment, query geometry,
padding and divider construction are unchanged, but this is not full visual
parity. Also, a combined search/keyboard/rotation experiment was killed with
exit code 137 during a later rotation in the 768 MiB container. Memory pressure
is suspected, not established by that exit status alone. Its failure evidence
is retained in `build/webkit/firmware-d562e2002c/`. The focused Search pass does
not resolve that combined-run failure; memory/cache behavior remains an M4 gate.

The opened Search-entry captures are pixel-identical across the two backends.
The menu's Search action and the query, back, submit and return controls also
have matching captured widget bounds. The results captures show the excerpt
font difference directly; destination page numbers can differ between engines.

# M4 annotation ranges and highlight rendering

This continuation follows the source-bookmark commit `0c7a585`. It adds the
range services needed by the existing annotation editor and saved-highlight
renderer. It is one step toward M4's feature-parity gate.

## Source ranges

The helper's existing metadata-only `QUERY` path accepts `resolve` and `ranges`
requests. Resolution verifies each source point against its CFI and context,
returns a canonical locator and identifies visible-text boundaries. The proxy
binds every reply to its publication spine occurrence before using its order.
No offsets from unverified saved tokens enter overlap comparisons.

Overlap checks use forward, half-open source intervals across spine entries.
Touching passages remain separate. Reversed or unavailable saved intervals do
not become overlaps. A failed helper lookup conservatively rejects an edit
against an existing list, rather than allowing it through an empty result.

Visibility and rectangles use the requested layout page. A range spanning
several spine entries projects onto the current content document; remote ends
never produce local handles. `pageRangeRects` resolves another page without
replacing the displayed image or logical reading point. This supplies geometry
for the existing preview path; it does not complete preview-feature parity.

Resolution batches stay within the existing KWRP line bound. Geometry batches
also bound reply size. Verified points and page geometry have bounded caches;
opening and reflow invalidate them. The markup renderer can ask for visibility
and then paint each visible passage using the same geometry, without another
IPC request per highlight. Existing source-range and aggregate rectangle limits
remain enforced. The queries allocate no exported page buffers.

## Terminal edges

The helper measures each endpoint's terminal grapheme and its DOM caret. It
returns the complete edge span and outward unit normal, separately from clipped
highlight rectangles. Endpoints on other pages remain invisible, including for
long passages whose middle is visible. A boundary between inline nodes uses
the terminal glyph's own text node.

At bidi-run boundaries WebKit can choose the adjacent run's caret affinity.
When that caret is outside the terminal glyph, its opposite boundary supplies
the glyph's direction. The comparison permits the rounding difference between
WebKit's caret and glyph rectangles. Real ARM checks cover inline Arabic,
Latin text, CJK, supplementary characters, combining marks and split inline
words. The proxy rejects malformed edge spans and normals.

The helper currently supports horizontal LTR pagination with inline bidi text.
Vertical/RTL publication layout and graphemes split across text nodes still
need further endpoint work. Unavailable geometry leaves the handle invisible;
clipped rectangles never manufacture a replacement edge.

## Highlight persistence

The production ARM session now saves a source highlight through
`HighlightSession` under the main-thread interaction guard. It verifies the
background save, rejection of an overlapping second mark, preservation of the
CREngine list, production markup, layout changes and a cold store reload.
This uses the source namespace and existing writer introduced by the bookmark
slice. Database schema, annotation formats and saved legacy data are unchanged.

The pointer editor still needs annotation token hit testing, sentence and paired
punctuation policies, far-edge drag behavior and page-edge continuation. Its
optional document methods remain unavailable in this slice. Source text
extraction and new highlight saves still require endpoints in one spine entry
and retain the helper's bounded text-extraction limit. Rendering and overlap
checks can inspect existing cross-spine intervals.

## Verification

[Recorded results](m4-ranges-results.json) identifies the logs and artifacts.
The native annotations lane passed. Final focused checks cover the proxy,
highlight session and main-thread guard; the final proxy assertion also covers
cache invalidation when typography changes without changing the page number.
Both prepared Kobo SDKs compile/link the application and pass their adapter
checks. Host syntax checking remains unavailable because `clang++` is absent.

The ARM protocol diagnostic verifies terminal edges, off-page endpoints,
canonical resolution and long-range geometry alongside the existing EPUB
regressions. The production session uses the checked-in Trevelyan fixture for
persistence, markup, reflow and reopening. Run these with the existing firmware
runner's `--epub-only` and `--session-only` actions and explicit
`--allow-unsandboxed-experiment`. These offline experiments do not establish
physical sandbox, performance or power results.

No controls or pointer interaction are enabled by this foundation. Existing
native UI/gallery scenes retain their coverage. Full native, full gallery and
an installed-reader scenario were not repeated. The manual device remains at
its earlier M4 installation. Disposable diagnostic devices were removed. The
previous bookmark review server was stopped after chat authorization to commit;
its captures and actual review state remain available in the worktree.

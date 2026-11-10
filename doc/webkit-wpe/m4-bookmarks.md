# M4 source bookmarks and isolated annotation storage

This continuation follows `fed0c51`. It enables bookmarks through the existing
reader and establishes separate storage for future WebKit highlights. M4's
pointer selection, highlight editing and complete feature-parity gate remain open.

## Ownership and compatibility

`BookmarkSession` and `HighlightSession` capture the document's position format
when constructed. CREngine retains its existing keys and saved XPointers.
WebKit selects `AnnotationPositionFormat::SourceV1` and a content-bound,
versioned key. There is no conversion between the two formats.

| Data | CREngine | WebKit source version 1 |
| --- | --- | --- |
| Book/list key | Existing native-ID or file-fingerprint key | `source-v1-` plus a digest of that key and the publication fingerprint |
| JSON metadata | Existing identity and format | Original identity with `position_format: "source-v1"` |
| SQLite identity | Original identity | Reserved `kobalt-source-v1://` identity with base64 original identity |
| Position | Existing XPointer | Validated version-1 source token |
| Changed-file adoption | Existing behavior | Separate content namespace; original records retained |

The source key cannot collide structurally with a legacy key. Its SQLite
identity cannot be adopted by an older file-path reader or opened as a native
ID. Direct database operations verify the key/format pairing. Position checks
reject source tokens in legacy lists and XPointers in source lists. Supported
unknown source metadata survives JSON edits. Unsupported source versions or
metadata fail without replacing the stored list.

Source JSON imports use the existing transaction and profile-preparation path,
including migration backups of the original files. SQLite uses the same
profile database and writer, with list revisions, atomic edits and pin metadata.
There is no new database file or schema migration. See the exact contract in
[Storage compatibility](../storage.md).

## Page summaries and lookup

The helper adds read-only `summary` and `position` query kinds. A summary has
the actual page-start locator and visible excerpt, distinct from a restored
logical anchor later on that page. Text extraction skips hidden content.
The proxy retains a bounded summary cache and invalidates it on reflow.

Position queries verify the saved locator using the existing CFI/context path,
then report its current page. Exclusive annotation ends use the preceding
visible source character so a page-boundary endpoint does not add an empty
final page. Source order follows numeric CFI steps. The proxy groups lookups by
spine and checks cancellation during IPC. These logically const lookups may
move the helper DOM; they preserve the logical reading point and immutable
displayed frame.

`pageStart`, `pageExcerpt` and `pagesOf` now implement the document contract
needed by bookmarks and annotation lists. Their queries do not allocate page
buffers or write profile storage. Lookup remains on the document worker;
bookmark edits use the existing background record writer.

## Reader integration

The existing bookmark corner/ribbon, toolbar entry, Bookmarks list, rows,
sort/pin controls and save-failure behavior are reused. No layout or styling
changes are introduced. Bookmark jumps and page-scrubber jumps now retain the
backend-aware transient navigation position, so Return works with source
anchors without writing them into a legacy reading-state field.

Bookmarks resolve after viewport/typography changes and survive a cold reopen.
Switching to CREngine shows its independent list; switching back restores the
source list. An unchanged sideloaded file keeps source records when moved.
Changed-file CFI adoption and CFI/XPointer conversion remain unavailable.

The source storage contract also preserves highlight fields, notes and endpoint
provenance. This does not enable the annotation-driven pointer UI, punctuation
policies, drag handles or complete highlight editing. Those remain later M4 work.

## Verification and review

[Recorded results](m4-bookmarks-results.json) lists the exact evidence. The native
annotations label covers existing store/session/UI behavior. Focused checks
cover the final namespace and metadata guards, failed-load preservation, source
lookup, summary cache invalidation, cancellation and main-thread rules. Both
prepared Kobo SDKs compile/link the application and run their adapter checks.

The real ARM helper checks page summaries, exact position lookup, exclusive
ends and restoration alongside the existing protocol regressions. The production
ARM session creates and persists a source bookmark under the interaction guard,
preserves an existing legacy list, reflows, navigates and cold-loads the store.
The installed diagnostic exercises both backends' actual bookmark UI:

```sh
python3 tests/webkit/run-firmware.py \
  --virtualnickel /path/to/VirtualNickel \
  --installed-only --installed-bookmarks \
  --allow-unsandboxed-experiment
```

The action follows saved-page events and visible controls. Inspection state
files are not required to refresh for every ribbon change. It adds a bookmark,
opens its list, jumps and returns, checks backend isolation and cold-reopens
WebKit. The owned device and helper tree are removed afterward. The retained
manual device is unchanged. These are offline, explicit unsandboxed experiments;
they establish no physical sandbox or device-performance result.

The targeted review is in `build/review/webkit-m4-bookmarks/`, using
`build/screenshots/webkit-m4-bookmarks/`. It compares the surrounding menu,
ribbon, opened list and destination with CREngine, and the reopened source list
with its pre-close capture. The shared control fonts, tabs, caption alignment,
padding and dividers are retained. Different first pages/excerpts reflect each
backend's layout. The existing excerpt-font/Aa bridge gap remains visible:
WebKit's Qt excerpts fall back to the interface font while CREngine uses the
book font. This is not complete visual parity. Existing full-gallery bookmark
scenes remain unchanged. Only the user records seen states and feedback.

The user authorized committing this slice in chat. At that point the review
recorded four seen screens, one unseen and no requested changes.

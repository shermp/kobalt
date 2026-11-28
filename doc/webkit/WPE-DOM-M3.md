# WPE source-position geometry on cold spine loads

Cold layouts previously searched for the trailing visible text one node at a time. F3 already rejects offscreen batches on retained-layout turns. DOM-M3 tests applying that same bounded scan to fresh spine documents after pagination, scrolling and the existing two-frame barrier. Both arms use [early appearance](WPE-DOM-M1.md) with all-image decoding. This keeps the [visible-image experiment](WPE-DOM-M2.md) out of the geometry comparison.

## Geometry changes

The trailing candidate groups at most 32 text nodes and 4096 UTF-16 units in an enclosing range. It uses F3's existing bounds and fragment checks. For a group that might contain visible text, it still checks each node and resolves the final UTF-16 offset using the existing per-node search. It visits text in the same source order with the same limits. The candidate is limited to fresh foreground spine loads. Reflow, preview and rollback use the existing paths.

The first and trailing lookups are timed separately. A forward crossing usually finds the first anchor in a few node queries. Entering at the final column reverses the cost. The test probe opens spine items at the first column on the first traversal and the last column on the second. It records the full page metadata bytes for comparison, including the first anchor, trailing anchor, affinities, precision, restoration status and links.

F3 batching cuts geometry calls without skipping source-order traversal. Any hint that starts the scan later would need to prove that earlier DOM nodes did not contribute visible fragments, including nonmonotonic layouts. The candidates use the existing document-local traversal and keep no per-page geometry or anchor index. The links fixture measures the cost of link handling separately.

## Correctness

Native and Cortex-A9 probes compared the complete serialized source metadata and pixel fingerprints for cold first- and last-column entries. The cases included long text, RTL with mixed Unicode, links and footnotes, image-heavy chapters, broken or unavailable images, and textless pages with image fallback anchors. The retained-view fixture covered font and viewport reflow, source restoration, previews, rollback and word hit testing. The native corpus oracle also compared exact serialized metadata on real EPUB, mixed Unicode, RTL, overflow, deeply nested markup and link fixtures. The headless Qt reader exercised footnotes, word selection and saved annotations with the ARM renderer.

## Forma measurements

The firmware and checksum-verified WPE runtime match the earlier comparisons. Three paired trials use fresh renderer sessions and profiles and reverse the arm order in the middle trial. Cold entries into the first and final columns are reported separately. They exclude initial open and wraparound to the first spine item. Timings are median / p95 in milliseconds. With three measured spine boundaries per entry direction per trial, p95 is the highest observation in each cold cohort. Call counts are medians. A batch query covers multiple text nodes and is reported separately from per-node and offset-search range calls.

### Trailing anchor batching

Values are unbatched → batched on cold entry. Same-spine turns use F3 batching in both arms.

| Fixture | Initial column | Cold crossing ms | First source ms | Trailing source ms | Trailing range calls per crossing | Batch queries per crossing | Links ms |
|---|---|---:|---:|---:|---:|---:|---:|
| long | first | 2039 / 2048 → 1633 / 1649 | 5 / 6 → 6 / 7 | 570 / 619 → 184 / 190 | 3027 → 19 | 0 → 95 | 2 / 2 → 2 / 2 |
| long | last | 2074 / 2099 → 2052 / 2091 | 617 / 630 → 614 / 624 | 7 / 15 → 11 / 18 | 9 → 9 | 0 → 1 | 2 / 2 → 2 / 3 |
| rtl | first | 500 / 527 → 493 / 547 | 6 / 18 → 6 / 7 | 49 / 69 → 39 / 48 | 158 → 30 | 0 → 5 | 1 / 1 → 1 / 7 |
| rtl | last | 553 / 575 → 562 / 572 | 44 / 78 → 44 / 72 | 3 / 4 → 5 / 6 | 6 → 6 | 0 → 1 | 1 / 1 → 1 / 1 |
| links | first | 503 / 514 → 472 / 506 | 6 / 14 → 5 / 18 | 42 / 55 → 16 / 26 | 208 → 16 | 0 → 7 | 15 / 39 → 16 / 31 |
| links | last | 639 / 647 → 642 / 647 | 41 / 61 → 40 / 55 | 4 / 15 → 6 / 11 | 7 → 7 | 0 → 1 | 95 / 100 → 96 / 99 |

Long-text forward crossings improved by 406 ms at the median. The trailing lookup fell from 570 ms to 184 ms with the same anchors and pixels. Its per-node and search range calls fell from 3,027 to 19, alongside 95 batch queries. The number of text nodes visited stayed near 3,000. Load-to-finished medians remained near 1.1 s. The link fixture gained 31 ms on forward crossings, while the RTL median gained 7 ms and its sampled p95 rose 20 ms. Final-column crossings saw little change because the trailing anchor is close to the end of traversal.

### First anchor batching

The independent first-anchor comparison keeps trailing batching enabled in both arms. On a cold entry beyond column zero, it groups nodes in forward source order and tests the enclosing range's individual fragments. A group that intersects is still searched with the existing per-node lookup. Column-zero entry uses the original first-anchor lookup, which typically needs only seven range calls on the long fixture.

| Initial column | Cold crossing ms | First source ms | Trailing source ms | First range calls per crossing | First batch queries per crossing |
|---|---:|---:|---:|---:|---:|
| first | 1629 / 1656 → 1620 / 1693 | 6 / 16 → 6 / 15 | 185 / 190 → 181 / 212 | 7 → 7 | 0 → 0 |
| last | 2063 / 2107 → 1730 / 1766 | 608 / 624 → 286 / 288 | 9 / 30 → 11 / 19 | 3083 → 11 | 0 → 97 |

Entering a long spine item at its final column gained 333 ms at the median. The first lookup fell from 608 ms to 286 ms. Batched traversal still visited the same section of the DOM and preserved the positions byte for byte. Column-zero crossing and warm-turn medians stayed within 9 ms in this comparison.

### Link resolution

The link-heavy fixture spends about 95 ms on the last-column link phase. The experiment checks geometry before URL resolution and footnote classification, and applies the original filters to each visible link. It preserves rectangle rounding, source order and the 24-rectangle output limit. URL or footnote work is deferred for offscreen links. A rejected URL can now incur a geometry query, so the balance depends on the publication.

| Initial column | Cold crossing ms | Link processing ms | Link geometry calls per crossing |
|---|---:|---:|---:|
| first | 475 / 482 → 471 / 491 | 25 / 40 → 25 / 36 | 24 → 24 |
| last | 633 / 650 → 607 / 624 | 86 / 110 → 64 / 76 | 120 → 120 |

Trailing batching was enabled and first-anchor batching was disabled in both link arms. All paired frames matched. Final-column crossings gained 26 ms at the median, including a 22 ms reduction in link processing. The first-column link median stayed at 25 ms. The native admission fixture also preserved the serialized link list with external links, query URLs, malformed fragment escapes and hidden references.

## Review decision

Retain the two source batching options for review. They give repeatable gains on long spine items in their respective entry directions. Retain visibility-first link resolution as a separate opt-in for the smaller improvement on link-dense final pages. Layout and pagination use the existing behavior. The remaining largest measured long-text cost is navigation to load finish at around 1.1 s. The [layout operations comparison](WPE-DOM-M4.md) examines that interval.

The automated Forma runs used the standalone renderer. The headless reader checks used the same ARM build under Cortex-A9 emulation. All temporary device payloads were removed. The renderer trees exited, the installed WebKit and renderer checksums matched, and Nickel retained its PID.

## Reproduction

Use the [cold baseline setup](WPE-DOM-M0.md#reproduction), with `KOBALT_WPE_EARLY_APPEARANCE=1` and `KOBALT_WPE_IMAGE_DECODE=all`. Run `wpe-footprint-probe RENDERER --cold FIXTURE_DIR --source-pages` to record first- and last-column entries and the exact metadata. Set both batching switches to `0` for the unbatched cold control. DOM-M5 enabled them by default. Visibility-first link resolution remains opt-in.

| Switch | Change |
|---|---|
| `KOBALT_WPE_COLD_SOURCE_BATCH=1` | Batch the trailing source scan on fresh foreground documents. |
| `KOBALT_WPE_COLD_FIRST_SOURCE_BATCH=1` | Batch the first source scan on fresh foreground documents entered after column zero. |
| `KOBALT_WPE_COLD_VISIBLE_LINKS=1` | Defer URL resolution and footnote classification until a link has a visible rectangle on a fresh foreground document. |

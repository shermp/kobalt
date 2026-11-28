# WPE combined cold-spine comparison

DOM-M5 compares the preserved [DOM-M0 renderer](WPE-DOM-M0.md) with [early reader appearance](WPE-DOM-M1.md) and both [cold source scans](WPE-DOM-M3.md) enabled. [Visible-only decoding](WPE-DOM-M2.md) and visibility-first link resolution remain opt-in. Both arms use all-image decoding, the same WPE WebKit 2.54.2 runtime, the original Forma fixtures and the existing two-animation-frame barrier. [DOM-M4](WPE-DOM-M4.md) selected the existing layout and readiness behavior.

## Broken-image correction

The combined candidate initially matched the six standard fixtures in native and emulated tests. On an additional fixture with corrupt and unavailable images, native and Cortex-A9 tests found a different broken-image placeholder. Anchors and page counts matched. Testing the appearance and source options independently isolated the difference to early appearance. The broken-image border and icon placement changed when WebKit saw the requested font size before creating its fallback box.

The guarded candidate uses `KOBALT_WPE_EARLY_APPEARANCE=text`. Its early user stylesheet applies only while the XHTML contains none of the media-related elements listed in the selector. Chapters containing images or other listed media use the existing late appearance path. The layout bridge still writes the requested size inline for every chapter. Both cold source scans apply in either case. The original unrestricted mode remains available for reproducing the comparison.

The guarded candidate matched serialized anchors, links, page counts and pixel fingerprints on native and Cortex-A9 cold probes, including image failures, textless image pages and a broken image at the end of a long XHTML document. The native corpus oracle adds first, middle and final pages in real publications, RTL and mixed Unicode, deep nesting, overflow, and links. The headless reader-session test adds preview rollback, font and viewport reflow, location restoration, cancellation, renderer recovery, and source revocation.

## Standalone Forma results

Each arm uses the same footprint probe, viewport, trace collector and resource fixtures. Each paired trial starts fresh renderer processes and isolated profiles. Trial order reverses in the middle trial. Cold metrics cover crossings into spine items one through three on both traversals. Initial open and wraparound are recorded separately. CPU is the renderer-tree time for a full session. Peak PSS is the highest sampled renderer-tree footprint across the trials.

### Unrestricted early appearance and both source scans

| Fixture | Cold crossing median / p95 ms | Warm turn median / p95 ms | DOM bridge median ms | Navigation median ms | CPU per session, median ms | Peak renderer PSS, MiB | Requests / bytes per crossing, median |
|---|---:|---:|---:|---:|---:|---:|---:|
| short | 390 / 412 → 311 / 322 (-20.3% / -21.9%) | 163 / 188 → 164 / 186 (+0.5% / -1.1%) | 175 → 84 (-51.7%) | 117 → 116 (-0.6%) | 5,150 → 4,590 (-10.9%) | 112 → 102 (-9.3%) | 1 / 5,537 → 1 / 5,537 |
| long | 3,088 / 3,139 → 1,637 / 1,678 (-47.0% / -46.5%) | 337 / 380 → 333 / 381 (-1.1% / +0.2%) | 1,869 → 424 (-77.3%) | 1,101 → 1,093 (-0.7%) | 23,770 → 14,750 (-37.9%) | 113 → 112 (-0.6%) | 1 / 110,945 → 1 / 110,945 |
| fonts | 580 / 622 → 401 / 439 (-30.8% / -29.4%) | 182 / 221 → 186 / 244 (+2.6% / +10.1%) | 291 → 119 (-59.2%) | 177 → 172 (-2.5%) | 6,820 → 5,840 (-14.4%) | 106 → 106 (-0.3%) | 1 / 6,443 → 1 / 6,443 |
| images | 795 / 1,020 → 755 / 933 (-5.1% / -8.6%) | 156 / 219 → 157 / 207 (+0.6% / -5.5%) | 455 → 405 (-11.0%) | 245 → 243 (-0.9%) | 7,510 → 7,230 (-3.7%) | 156 → 158 (+1.2%) | 5 / 202,103 → 5 / 202,103 |
| rtl | 718 / 741 → 496 / 514 (-31.0% / -30.7%) | 201 / 257 → 210 / 246 (+4.7% / -4.0%) | 388 → 142 (-63.3%) | 207 → 247 (+19.6%) | 8,090 → 6,780 (-16.2%) | 105 → 107 (+1.9%) | 1 / 7,300 → 1 / 7,300 |
| links | 643 / 689 → 468 / 498 (-27.3% / -27.8%) | 285 / 342 → 302 / 343 (+6.2% / +0.2%) | 342 → 171 (-50.1%) | 153 → 150 (-2.1%) | 8,110 → 7,070 (-12.8%) | 114 → 104 (-8.7%) | 1 / 8,024 → 1 / 8,024 |

All standard paired signatures matched on the Forma. Long-text cold crossing fell by 1.45 s at the median, and the DOM bridge fell by the same amount. The navigation median held near 1.1 s. The warm long-text p95 held near 380 ms. The font fixture gained 179 ms at the median cold crossing, with a 23 ms higher warm p95 in this sample. Resource request and byte distributions matched. The additional broken-image fixture rules out unrestricted early appearance as the default.

### Text-only early appearance and both source scans

| Fixture | Cold crossing median / p95 ms | Warm turn median / p95 ms | DOM bridge median ms | Navigation median ms | CPU per session, median ms | Peak renderer PSS, MiB | Requests / bytes per crossing, median |
|---|---:|---:|---:|---:|---:|---:|---:|
| long | 3,065 / 3,144 → 1,631 / 1,682 (-46.8% / -46.5%) | 333 / 404 → 331 / 374 (-0.8% / -7.5%) | 1,861 → 419 (-77.5%) | 1,096 → 1,093 (-0.3%) | 23,570 → 14,730 (-37.5%) | 114 → 112 (-1.5%) | 1 / 110,945 → 1 / 110,945 |
| fonts | 581 / 616 → 402 / 431 (-30.7% / -30.0%) | 184 / 219 → 185 / 238 (+1.0% / +8.6%) | 294 → 119 (-59.5%) | 178 → 177 (-0.5%) | 6,820 → 5,870 (-13.9%) | 106 → 106 (-0.1%) | 1 / 6,443 → 1 / 6,443 |
| images | 789 / 1,036 → 801 / 984 (+1.5% / -5.1%) | 152 / 219 → 155 / 204 (+1.8% / -6.8%) | 463 → 458 (-1.1%) | 237 → 246 (+3.7%) | 7,510 → 7,490 (-0.3%) | 155 → 158 (+2.0%) | 5 / 202,103 → 5 / 202,103 |

The guard retained the long-text and embedded-font gains across all three pairs. Peak renderer memory was roughly level. The image-heavy median was 12 ms slower and p95 was 52 ms faster. Image chapters use late appearance in this mode. Source batching contributed little on this fixture, where the source scan was already short. Resource counts and byte distributions matched on every pair.

Pixel fingerprints and page metadata matched. The physical broken-image check also compared the complete serialized anchors and links on first- and final-column entry and their adjacent columns. It passed with the guarded stylesheet.

## Forma reader-session replay

The replay uses Kobalt's production Qt5 `ReaderSession`, local EPUB admission and ZIP resource loading. It opens disposable EPUBs at 1,440 × 1,920, advances through three spine boundaries, reverses into the previous chapter's final column, returns forward and revokes the source before closing. The private Qt platform is offscreen. Timings end when the reader session publishes the frame, before the Nickel UI or E Ink refresh.

| Publication | Forward crossing ms | Backward crossing ms | Same-spine turn ms | Initial open ms |
|---|---:|---:|---:|---:|
| long | 2530 / 2695 → 2152 / 2257 | 2539 / 2557 → 2077 / 2092 | 770 / 830 → 762 / 819 | 2935 / 2959 → 2537 / 2581 |
| images | 881 / 946 → 875 / 926 | 531 / 531 → 533 / 533 | 118 / 166 → 120 / 161 | 1361 / 1384 → 1366 / 1399 |
| image-edges | 901 / 922 → 888 / 942 | 548 / 548 → 541 / 541 | 121 / 167 → 122 / 165 | 1411 / 1411 → 1378 / 1378 |

Values show median / p95. Forward timings use the first traversal. There are three paired sessions per main publication and one paired session for the broken-image check. Every published frame matched its baseline counterpart in pixels, source location and chapter page count. The session revoked the source and drained its renderer at close.

Long-text forward crossings improved by 378 ms (15%) and backward crossings by 462 ms (18%). The forward DOM bridge median fell from 1,095 ms to 708 ms. Its style interval fell from 126 ms to 8 ms, and its trailing source lookup fell from 408 ms to 160 ms. Navigation remained near 1.33 s. This replay uses the reader's 18 px default, a different EPUB path and consecutive reading across chapters. Its 2.15 s median is the better expectation for this replay than the 1.63 s standalone timing.

## Recommendation

The default enables text-only early appearance and both cold source batches. The unrestricted style stays experimental because of the broken-image result. Keep all-image decoding as the default and keep visible-only decoding and visibility-first link resolution opt-in. The DOM-M2 warm-turn cost remains relevant and DOM-M3 measured a smaller gain limited to link-dense final pages.

Keep one WebView on the Forma. A guarded long crossing still takes 1.63 s in the standalone probe and 2.15 s through the reader session. Navigation accounts for about 1.1 s and 1.33 s of those respective timings. Profile the path from serving the main XHTML to completing navigation if that remaining delay warrants more work. The [independent and related-view probes](WPE-RV0b.md) found foreground contention and additional renderer memory with a second view. Those costs still outweigh the case for speculative preparation on this device.

All temporary benchmark payloads and isolated profiles were removed. Nickel kept its PID and the installed renderer and runtime checksums matched after the runs.

## Reproduction

Use the [DOM-M0 fixtures and trace collection](WPE-DOM-M0.md#reproduction) and the archived DOM-M0 renderer for the baseline. For the guarded comparison, set:

```sh
KOBALT_WPE_EARLY_APPEARANCE=text KOBALT_WPE_COLD_SOURCE_BATCH=1 \
KOBALT_WPE_COLD_FIRST_SOURCE_BATCH=1 KOBALT_WPE_IMAGE_DECODE=all \
KOBALT_WPE_COLD_VISIBLE_LINKS=0 KOBALT_WPE_PERF=1 \
wpe-footprint-probe RENDERER --cold FIXTURE_DIR
```

Set the three appearance and batching switches to `0` to disable their defaults in the current renderer. Add `--source-pages` for byte-for-byte source and link comparisons on cold first- and last-column entry.

# WPE layout operations on cold spine loads

This comparison holds [early reader appearance](WPE-DOM-M1.md), all-image decoding and [cold trailing-source batching](WPE-DOM-M3.md) fixed. The Forma uses the checksum-verified WPE WebKit 2.54.2 runtime. The layout and script comparisons use the same binary within each comparison, alternate the arm order and start each trial with a fresh renderer and profile. Cold measurements cover forward spine crossings and exclude initial open and wraparound. Values show median / p95 in milliseconds.

## Initial layout read

The control writes the inline font property, reads the body bounding rectangle and reads the root direction. The candidate skips only the body rectangle when the fresh foreground document already has the requested early user stylesheet. It retains the inline value, computed direction, font and image waits, first later pagination read of `scrollWidth` and the two-animation-frame barrier. These intervals measure elapsed time around API calls and can reveal work moving between phases.

The native and Cortex-A9 comparisons matched exact serialized metadata and pixel fingerprints on long, RTL, font, image and textless-image workloads, including final-column entry. The retained view and headless reader checks also cover reflow and preview rollback.

| Fixture | Cold crossing | Style | Font readiness | Columns | DOM bridge | Warm turns |
|---|---:|---:|---:|---:|---:|---:|
| long | 1663 / 1743 → 1652 / 1763 | 6 / 16 → 5 / 16 | 0 / 1 → 0 / 1 | 1 / 1 → 0 / 1 | 429 / 459 → 429 / 471 | 336 / 369 → 335 / 365 |
| fonts | 427 / 479 → 408 / 445 | 6 / 16 → 6 / 15 | 0 / 1 → 0 / 1 | 0 / 1 → 0 / 1 | 124 / 134 → 124 / 134 | 185 / 217 → 188 / 236 |
| images | 755 / 940 → 747 / 991 | 6 / 20 → 5 / 18 | 0 / 1 → 1 / 1 | 0 / 1 → 0 / 1 | 413 / 446 → 405 / 460 | 155 / 205 → 153 / 196 |

All paired Forma frames matched. Skipping the read saved 1 ms in the median style phase on the long fixture and left the DOM bridge median at 429 ms. The long crossing p95 rose 20 ms, and individual trials showed mixed crossing changes. The embedded-font crossing median fell by 19 ms while its style and bridge medians held steady. Navigation independently shifted from 183 ms to 175 ms at the median, before the candidate operation ran. Keep the existing read.

## Final-column alignment

The trailing element supplies the extra scrollable width when a publication extends partway into its last column. A three-column fixture measured a 3,200 px extent at 1,440 px per viewport. The final viewport starts at 2,880 px. Removing the element reduced the maximum scroll to 1,760 px in both LTR and RTL, and the existing scroll check rejected the final frame. Accepting the clamped value would mix content from different columns.

An absolutely positioned CSS `::after` matched page count, metadata and pixels in the plain LTR and RTL fixtures on both architectures. It changed the pixels in a fixture whose publisher CSS already uses that pseudo-element for visible content. The existing dedicated element keeps the authored pseudo-element and supplies the required scroll extent. Keep it.

## Navigation readiness

The navigation report separates document milestones in the Web process from the last completed scheme response and `WEBKIT_LOAD_FINISHED` in the renderer process. Receipt of the final bytes leaves parsing, styling, layout, decoding and scheduling work. The intervals show where time is spent without assigning it to a single internal operation.

A document-start observer on native and Cortex-A9 WPE saw eight eager images with unknown intrinsic dimensions at `DOMContentLoaded` on first-traversal image chapters. On the font fixture's first open, the linked stylesheet was still unavailable at that point. In a separate fixture with undimensioned images flowing across the columns, the early extent was one 1,440 px viewport. Normal rendering after load returned four pages on the initial chapter. The probe observed geometry without publishing an early frame. Keep the current load gate because the early event admits incomplete pagination.

| Forma fixture | Navigation to load finish | Last scheme response to load finish | DOMContentLoaded to browser load event |
|---|---:|---:|---:|
| Long | 1,107 / 1,218 | 1,077 / 1,197 | 163 / 178 |
| Embedded fonts | 183 / 204 | 169 / 195 | 26 / 54 |
| Images | 243 / 410 | 124 / 181 | 140 / 303 |

The long fixture served only XHTML during its measured crossings. Most of its navigation time remained after scheme response completion, so waiting for an offscreen asset cannot explain the long-text result. The image and font probes show why earlier document readiness alone is insufficient.

## Document-start source helpers

The candidate installs the existing source helper definitions in the same isolated world when a navigation creates its document. Each layout call then uses those functions and omits the 15.7 KB helper source from its body. Selection queries continue to use their existing path. This tests whether moving helper setup out of individual layout calls improves crossings or retained turns.

The dispatch-to-entry interval includes IPC, Web process scheduling and script preparation. Both renderer processes read the same realtime clock, with millisecond resolution at JavaScript entry. Navigation, full bridge duration and end-to-end crossing time measure whether a lower entry interval translates into a faster frame.

| Fixture | Cold crossing | Navigation | Dispatch to script | DOM bridge | Warm dispatch to script | Warm turns |
|---|---:|---:|---:|---:|---:|---:|
| long | 1657 / 1753 → 1629 / 1682 | 1108 / 1227 → 1079 / 1126 | 201 / 237 → 202 / 223 | 418 / 465 → 431 / 449 | 3 / 3 → 2 / 6 | 335 / 367 → 330 / 366 |

Paired pixel and page metadata matched. The candidate reduced the long crossing median by 28 ms alongside a 29 ms shorter navigation interval. Cold dispatch to script stayed near 200 ms and the bridge median increased from 418 ms to 431 ms. Warm dispatch fell by about 1 ms at the median. The shorter layout script did not produce a meaningful reduction in the measured script-entry interval. Keep the existing helper submission.

## Review decision

Keep the existing renderer behavior. The final-column and readiness alternatives failed correctness checks. Removing the initial rectangle read and installing the source helpers at document start preserved the tested results, but neither produced a meaningful saving in its targeted bridge phase. The [combined comparison](WPE-DOM-M5.md) evaluates the candidates from the earlier milestones.

Validation used native WPE, Cortex-A9 emulation, headless Qt reader tests and the standalone renderer on the physical Forma. Both Forma payloads were removed, the installed renderer and WebKit checksums matched, and Nickel retained its PID.

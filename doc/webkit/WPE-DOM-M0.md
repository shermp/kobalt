# WPE cold spine baseline

DOM-M0 measures a single WebView before changing how the reader applies appearance, waits for resources, or lays out a spine item. The baseline includes the F2 same-spine layout reuse and F3 trailing-source optimization. Cold layouts take the current unbatched source-position path.

## Method

The existing book renderer emits `wpe-perf-navigation` for actual URI loads. The event adds monotonic timestamps for the load call, `LOAD_COMMITTED`, and `LOAD_FINISHED`. Existing DOM, geometry, renderer, resource, and wire events supply the other intervals. All tracing is opt-in. Scroll and the two-animation-frame freshness barrier, resource admission, cache model, and frame pool use the existing behavior.

The footprint probe uses the existing socket protocol at 1440 × 1920 with two shared frame slots. It starts a fresh renderer, visits four distinct spine items in order twice, and verifies two distinct column fingerprints and a stable return fingerprint at each stop. The forward sample uses transitions into spine items 1, 2, and 3. Initial open and the wrap back to spine item 0 are separate. The memory sampler collects the renderer tree outside the frame path and excludes the driving process. CPU is the reaped renderer's tree time for the whole session.

The fixtures include short and long text, embedded serif and italic fonts, eight undimensioned images per chapter, RTL with mixed Unicode, and links with footnotes. The printed resource counts cover completed requests served by the existing transport. Resource service can overlap navigation or DOM readiness. Medians from separate phases should not be summed, and a timing around a layout-triggering API does not count WebKit's internal layout passes.

## Forma baseline

The physical Forma ran firmware 4.38.23852 and the same checksum-verified WPE WebKit 2.54.2 `-O2` runtime as RV0b. Each workload ran in three fresh sessions, yielding 18 forward crossings and 48 same-spine turns per workload. Timings are milliseconds unless noted. CPU is the median total for one session. Peak PSS is the largest complete sample.

| Workload | Forward median / p95 | Warm median / p95 | CPU per session | Peak renderer tree | Median / p95 completed requests | Median / p95 resource bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Short | 395 / 409 | 156 / 191 | 5.16 s | 112.6 MiB | 1 / 1 | 5,537 / 5,537 |
| Long | 3088 / 3132 | 332 / 379 | 23.70 s | 113.6 MiB | 1 / 1 | 110,945 / 110,945 |
| Fonts | 586 / 614 | 183 / 222 | 6.84 s | 106.4 MiB | 1 / 1 | 6,443 / 6,443 |
| Images | 782 / 1020 | 152 / 218 | 7.47 s | 156.3 MiB | 5 / 9 | 202,103 / 568,668 |
| RTL | 703 / 743 | 203 / 263 | 8.05 s | 104.9 MiB | 1 / 1 | 7,300 / 7,300 |
| Links and footnotes | 633 / 673 | 287 / 352 | 8.14 s | 105.5 MiB | 1 / 1 | 8,024 / 8,024 |

The three long forward medians were 3082, 3094, and 3093 ms. The three pooled image medians were 779, 778, and 790 ms. Image forward crossings were 968 ms on the first traversal and 663 ms on the second, so both cohorts matter in any follow-up comparison. The font workload's initial open served four requests totalling 891,726 bytes at the median. Its forward crossings reused the stylesheet and font resources. Retain initial-open and crossing metrics separately when evaluating font readiness.

### Forward phase medians

| Interval | Short | Long | Fonts | Images | RTL | Links |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Load call → commit | 45 | 128 | 41 | 32 | 44 | 40 |
| Commit → load finished | 72 | 972 | 138 | 208 | 168 | 101 |
| Load call → load finished | 116 | 1105 | 180 | 249 | 208 | 149 |
| Load finished → bridge dispatch | 0.4 | 0.4 | 0.4 | 0.4 | 0.4 | 0.4 |
| DOM bridge wall time | 172 | 1880 | 293 | 455 | 388 | 336 |
| Viewport readiness | 2 | 2 | 2 | 2 | 2 | 2 |
| Appearance and initial forced layout | 73 | 1037 | 115 | 24 | 179 | 88 |
| Font readiness | 0 | 0 | 0 | 0 | 0 | 0 |
| Explicit image decoding and readiness | 1 | 2 | 1 | 323 | 1 | 1 |
| Column and tail calculation | 2 | 2 | 2 | 1 | 1 | 2 |
| Scroll and two-frame wait | 38 | 45 | 57 | 34 | 49 | 77 |
| First source position | 6 | 6 | 5 | 5 | 7 | 5 |
| Visible links | 1 | 2 | 1 | 1 | 1 | 15 |
| Trailing source position | 21 | 584 | 42 | 8 | 59 | 38 |
| Snapshot async interval | 54 | 60 | 73 | 63 | 63 | 112 |
| Snapshot finish → frame sent | 40 | 50 | 38 | 22 | 37 | 21 |

The long fixture spends about a second before load finish, then another second applying appearance and forcing layout. Its trailing-source pass makes 3,027 rectangle queries at the median with no cold-layout batches. This makes early CSS and cold source geometry concrete investigation targets. The recorded stages do not establish that WebKit laid out the document twice. They establish that the late appearance and forced layout interval costs about a second after document loading has already spent another second.

Images spend 323 ms in the explicit decode interval even though none of their completed resource requests occurs after bridge dispatch in these cold crossings. The trace distinguishes bytes delivered from image readiness. M2 still needs to prove that visible images are ready for the first snapshot without the explicit wait. The gap between DOM bridge wall time and the nine reported JavaScript intervals is 191 ms on long text and 61 ms on images. It can include dispatch, script setup, scheduling, and completion overhead. It does not isolate script compilation.

## Live forward navigation

The live pass used the imported four-chapter image-heavy EPUB, the existing Kobalt Qt host, and the trace-only single-view renderer. The operator advanced one page at a time with the forward button. The trace recorded three successful crossings and 28 same-spine turns.

| Live renderer metric | Result |
| --- | ---: |
| Forward crossing median and range | 1379 ms, 1308–1396 ms |
| Warm turn median / p95 | 190 / 306 ms |
| Load call → load finished, median | 672 ms |
| Appearance and initial forced layout, median | 49 ms |
| Image decode and readiness, median | 389 ms |
| DOM bridge wall time, median | 552 ms |
| Snapshot interval, median | 89 ms |
| Completed resource requests, median | 9 |
| Resource bytes, median | 554,251 |
| Peak sampled renderer-tree PSS | 148.7 MiB |
| Minimum MemAvailable | 177.7 MiB |

Live timing runs from the renderer's request receipt to its frame reply. It excludes the time before the Qt host sends the request and the time until the E Ink panel refreshes. The live pass used the EPUB's first traversal, the Qt host's resource path, and the reader's viewport and appearance, so its timings are a separate cohort from the standalone probe. Both the load interval and post-load decoding remain substantial in the live trace.

The Forma returned to Nickel, restored the original launcher and removed the temporary test files. Nickel kept its PID and the installed renderer and WPE library checksums matched. The standalone progress watcher timed out late in the third round. Recovery kept the completed sessions and reran the four incomplete cases with the same payload and fixture order.

## Validation

The native source-location and retained-view regressions passed. All six workloads passed the native cold-spine smoke run. The long, font-heavy, image-heavy, and RTL fixtures also ran under the stock Kobo loader with Cortex-A9 emulation. Page counts and pixels matched the uninstrumented ARM renderer on those four workloads.

## Proposed DOM-M1 comparison

See the [DOM-M1 stylesheet experiment](WPE-DOM-M1.md) for the paired results.

Use the same baseline and change only how the requested font size is supplied. Install it with the existing user stylesheet before requesting a new spine item while retaining the explicit geometry query for the first comparison. Measure whether the style phase gets shorter and whether any cost moves into navigation or initial font loading. Hold image decoding, resource readiness, source geometry, and snapshot timing constant.

The stylesheet experiment needs checks for appearance changes across a spine boundary, reflow, preview restoration, failed navigation, exact page counts, first and last column fingerprints, and embedded fonts. Test removal of the initial forced layout separately if the data still identifies it as redundant. DOM-M1 requires owner review.

## Reproduction

Build `wpe-book-renderer` and `wpe-footprint-probe` with the [minimal FW4 runtime](WPE-M8-validation.md#build-and-staging). Generate a fresh fixture directory with `sh platforms/webkit/footprint/make-cold-fixtures.sh NEW_DIRECTORY`. Run the renderer with its existing WPE runtime launcher and pipe standard error to a collector so the nonblocking trace descriptor can open.

```sh
KOBALT_WPE_PERF=1 ./wpe-footprint-probe ./run --cold FIXTURES/long \
    >long.log 2>trace-fifo
```

Create and drain the FIFO before starting the probe. Repeat for `short`, `fonts`, `images`, `rtl`, and `links` with fresh renderer sessions and profiles.

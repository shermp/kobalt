# WPE related WebView preemption and lazy retention

RV0b follows [the related WebView feasibility measurements](WPE-RV0.md). It tests cancellation under forced overlap and keeps the production reader on its existing single-view path.

## Preemption experiment

The standalone probe uses its existing parent process as the foreground request controller. A milestone in the adjacent view signals the parent, which timestamps and sends the request. A high-priority GLib source logs receipt in the renderer. The probe records the cancellation request, adjacent completion, foreground dispatch and shared-memory frame delivery. The difference between issue and receipt measures the renderer event-loop wait. Milestone-to-issue time is logged separately because the controller can be sampling memory. The interval from dispatch to frame includes WebKit and the frame copy. CPU is summed over the renderer and helpers for the trial.

The long workload uses the same 1440 × 1920 viewport, two shared frame slots, publication policy and retained layout bridge as RV0. Each trial starts with a populated foreground view and, for the overlap cases, creates a fresh adjacent view. The independent control uses a separate WebProcess in the same WebKit context. The probe compares keeping the adjacent request active, cancelling the operation's `GCancellable`, and cancelling plus calling `webkit_web_view_stop_loading()`.

| Phase | Probe boundary |
| --- | --- |
| Before resources | Immediately after queueing the adjacent URI on a fresh view |
| Resource retrieval | The admitted scheme request is held for 120 ms in this arm to guarantee outstanding I/O |
| Parsing window | WebKit sends `LOAD_COMMITTED`. The logs bracket it with response hand-off and load finish |
| Style and layout | An isolated-world message is sent immediately before the viewport's forced layout |
| JavaScript | A second message is sent before the synchronous source and link geometry pass |
| Snapshot preparation | The probe signals immediately before submitting the adjacent WebKit snapshot |

The parser and snapshot boundaries are observable API events. They do not directly timestamp the start of parser instructions or painting inside WebKit. The isolated-world messages mark the adjacent work and do not change the two-animation-frame barrier. A stop before the first resource may have no WebKit completion notification. If the fresh test view is discarded after the observation window, the logs mark that separately from an observed cancellation.

### Forma preemption results

The Forma ran firmware 4.38.23852 and the same checksum-verified WebKit 2.54.2 `-O2` runtime as RV0. Each arm ran twice with ten repetitions per phase. The table shows issue-to-frame p95 in milliseconds. The combined stop arm uses both the existing cancellable and `stop_loading()`.

| Phase | Related, no cancel | Related, cancel only | Related, combined stop | Independent, combined stop |
| --- | ---: | ---: | ---: | ---: |
| Quiet | 354 | 358 | 353 | 409 |
| Before resources | 1433 | 1431 | 354 | 521 |
| Resource retrieval | 335 | 326 | 320 | 352 |
| Parsing window | 3109 | 3097 | 3002 | 674 |
| Style and layout | 1910 | 1909 | 1891 | 618 |
| JavaScript | 734 | 727 | 731 | 707 |
| Snapshot preparation | 392 | 387 | 387 | 431 |

| Related combined stop | Issue to frame median / p95 / worst ms | Event-loop wait p95 ms | Cancel to observed median / p95 ms | Tree CPU median ms |
| --- | ---: | ---: | ---: | ---: |
| Quiet | 337 / 353 / 357 | 0.10 | — | 270 |
| Before resources | 335 / 354 / 358 | 3.97 | No WebKit completion in 20 trials | 270 |
| Resource retrieval | 290 / 320 / 325 | 0.24 | 7 / 13 | 325 |
| Parsing window | 1541 / 3002 / 3030 | 8.58 | 208 / 2887 | 1235 |
| Style and layout | 1862 / 1891 / 1898 | 0.39 | 1595 / 1624 | 2420 |
| JavaScript | 703 / 731 / 739 | 0.48 | 401 / 410 | 2425 |
| Snapshot preparation | 380 / 387 / 391 | 1.03 | 62 / 66 | 2490 |

During style/layout and JavaScript, the combined stop was sent before the phase-end marker in all trials. All cancellation completions arrived after that marker. In the combined-stop arm, the observed style/layout interval was 1.11 s at the median and the source-geometry interval was 404 ms. Receipt-to-dispatch p95 was 4.9 ms across phases. The remaining wait was inside rendering through shared-memory frame copy. The independent control shortened the largest shared-process stalls even when the neighbour's cancellation still took seconds. The combined stop worked promptly while the resource response was outstanding. Once WebKit committed the load, it was too late for a consistently responsive next turn.

**Gate 1 failed.** RV0b.2 was skipped. An idle delay and a paint acknowledgement can defer the start of speculative work, and neither interrupts the expensive work established here. The feasibility comparison continues with foreground-only lazy retention.

## Lazy retention experiment

The baseline reloads a destination in one view. The lazy arm starts with one view, creates a second view on the first requested crossing, and promotes it as soon as its requested frame is delivered. It keeps the most recently visited other document until navigation needs a third. Each request runs to completion before the next begins. A preview of a retained document restores its own confirmed position, and the selected reader position stays unchanged. The probe verifies preview pixels and source anchors, rejects an invalid destination column, cancels a destination snapshot, and checks that the displayed view survives both attempts.

The standalone comparison uses long and image-heavy content. The optional `wpe-lazy-book-probe` target also exposes the experiment over the existing reader wire for a live session. It compiles separately from `wpe-book-renderer`, uses the same resource transport and two-slot pool, and selects the previous view only on a requested navigation or preview.

### Forma lazy-retention results

Each workload and mode ran in three fresh sessions. The crossing time starts before selecting or creating the view and ends at the frame copy. The one-view return column uses the initial backward and forward revisits. The two-view return column covers the warm requested crossings. Memory peaks cover the ordinary navigation and preview sequence before the fault checks.

| Workload | Mode | Initial crossing median / max ms | Cold median / p95 ms | Return median / p95 ms | Preview median / p95 ms | Peak PSS MiB | Min headroom MiB | Idle → settled PSS MiB |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| Long | One | 2868 / 2879 | 2922 / 2976 | 2903 / 2926 | 5787 / 5821 | 131.4 | 221.6 | 121.2 → 121.2 |
| Long | Two related, lazy | 3039 / 3052 | 3103 / 3134 | 291 / 495 | 555 / 576 | 151.2 | 198.4 | 133.9 → 113.2 |
| Image-heavy | One | 838 / 861 | 679 / 858 | 617 / 716 | 1022 / 1158 | 177.7 | 177.5 | 160.6 → 160.6 |
| Image-heavy | Two related, lazy | 991 / 1003 | 1025 / 1073 | 142 / 308 | 336 / 473 | 177.0 | 176.8 | 163.9 → 144.1 |

The first crossing became 171 ms slower on the long workload and 153 ms slower with images. Later cold crossings also cost more, especially when an older retained slot had to be replaced. Returning to a retained document and previewing it were considerably faster. The two-view long workload peaked about 20 MiB above its one-view control. The image-heavy standalone peak was similar across the two arms. Both retain caches while navigating and the one-view arm reloads more documents, so a similar peak does not imply that adding a view is free.

After the last replacement, two retained views held approximately 134 MiB on long text and 164 MiB on images at the first idle checkpoint. Releasing the inactive view produced little immediate reduction. After 15 seconds, the totals were about 113 and 144 MiB. Invalid-column and cancelled-snapshot requests preserved the foreground position. The matched fingerprints and retained anchors passed and all process trees exited.

The native retained-view and source-location checks passed. The separate wire renderer also passed the footprint client and the existing Qt reader-session suite using the ARM renderer and the cached stock loader under Cortex-A9 emulation.

## Live reader measurements

The owner navigated the same image-heavy EPUB in the actual Kobalt reader on the Forma, in the order 1 → 2 → 1 → 2 → 3 → 2 → 4 → 1, then returned to the Nickel library. Both arms used the verified RV0 WebKit runtime and the two-slot pool. Kobalt's Qt reader runs inside the Nickel process, so its memory is reported with Nickel rather than as a separate process. The host delta uses each arm's own pre-open library PSS.

| Live image-heavy metric | One view | Two views, lazy |
| --- | ---: | ---: |
| Peak renderer-tree PSS | 140.3 MiB | 169.5 MiB |
| Peak renderer-tree private dirty | 84.1 MiB | 113.6 MiB |
| Nickel PSS, library median → reading peak | 126.5 → 175.8 MiB | 127.5 → 175.7 MiB |
| Peak combined Nickel and renderer-tree PSS | 298.9 MiB | 327.6 MiB |
| Minimum system MemAvailable | 175.2 MiB | 155.1 MiB |
| System MemAvailable 15 s after close | 311.2 MiB | 311.7 MiB |

The lazy arm handled three warm revisits and four cold crossings in the recorded sequence. It used one WebProcess and one NetworkProcess throughout. Most of the extra memory was in the WebProcess, whose peak private dirty increased from 77.6 to 107.3 MiB. There was no swap activity, observed OOM event or unexpected process termination. The original launcher was restored after each pass. Nickel kept PID 232 and the installed renderer and WebKit library hashes matched after cleanup.

The median sample intervals during reading were 380 ms for one view and 390 ms for two, so short peaks can fall between samples. An initial single-view pass omitted the network helper owing to a sampler PID-scope bug. The table uses the corrected repeat and the corrected two-view pass. This is one matched live navigation sequence on a single image-heavy book. The results support a two-view limit for an implementation trial, with extended reading and heavier publications still to validate.

## Architecture decision

| Architecture | Memory cost | Crossing benefit | Foreground contention | Complexity |
| --- | --- | --- | --- | --- |
| Single WebView | Live image-heavy peak 140 MiB renderer PSS and 175 MiB minimum headroom | Reloads a previously visited spine item | Quiet baseline | Lowest |
| Two views, lazy retention | Long standalone peak +20 MiB. Live image-heavy peak 170 MiB renderer PSS and 155 MiB minimum headroom | Long return median 291 ms. Image-heavy return median 142 ms | All document work follows a requested action | Low |
| Two views, deferred loading | RV0 added 11–30 MiB | Warm revisit benefit when a load finishes | Preemption gate failed around commit, layout and source geometry | Moderate |
| Three views, deferred loading | RV0 added 42–51 MiB and left a minimum 122 MiB on image-heavy content | Either adjacent direction could be warm if populated | Same shared-process preemption gate | Moderate |

Recommend two related views with foreground-only lazy retention for a reviewed implementation trial. It speeds returns and previews, and adds cost to new chapter crossings. The live image-heavy run retained 155 MiB of headroom. Use two as the experimental upper limit until extended reading and larger publications establish how memory varies. The production reader remains at one view pending RV1 approval.

## Reproduction

Build `wpe-related-view-probe` and `wpe-lazy-book-probe` in the same standalone FW4 WPE project used for RV0. Run the standalone probe with its existing runtime environment.

```sh
./wpe-related-view-probe two-related long FIXTURE_DIR --preempt stop 10
./wpe-related-view-probe two-independent long FIXTURE_DIR --preempt stop 10
./wpe-related-view-probe one images FIXTURE_DIR --lazy
./wpe-related-view-probe two-related images FIXTURE_DIR --lazy
```

Use `none` as the no-cancellation control and `cancel` for the `GCancellable`-only arm. The image workload uses the three PNG inputs documented for RV0. Keep the same runtime build, fixture, viewport and display path across a comparison.

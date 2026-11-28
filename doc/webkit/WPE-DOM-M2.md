# WPE image decoding on cold spine loads

This experiment holds [early reader appearance](WPE-DOM-M1.md) and the two-animation-frame snapshot barrier fixed. All arms use the same renderer build and resource path. The Forma runs use the installed WPE WebKit 2.54.2 runtime and temporary renderer binaries.

## Decode strategies

| Mode | Image readiness before snapshot |
|---|---|
| All | Await `HTMLImageElement.decode()` for every XHTML image before measuring pagination. |
| None | Let WebKit load and paint images normally. Preserve the existing frame barrier. |
| Visible | On a fresh document with completed eager resources and nonresponsive image sources, measure pagination from known intrinsic sizes. After scrolling, await decoding for image boxes that intersect the viewport, then wait for both frames. Apply the same viewport check on retained-layout turns. Full reflows, rollback reloads, cross-spine preview loads, pending resources, lazy images and responsive sources use all-image decoding. |

The experiment selects the mode through `KOBALT_WPE_IMAGE_DECODE`. The default is `all`. The cold probe uses `--cold FIXTURE_DIR --image-pages` for the extended correctness check.

The document load event and `image.complete` describe the resource load. Completed eager images can supply their intrinsic dimensions to layout before decoded pixels are ready to paint. Failed images can also report completion. The visible strategy keeps WebKit's fallback layout for those images and settles rejected decode requests as the renderer normally does.

## Correctness

The no-decode arm lost the expected responsive image after a viewport resize in the retained fixture, both natively and under Cortex-A9 emulation. The two-frame barrier was insufficient for this case. The all-image and visible arms matched the retained fixture, including reflow, preview, rollback and source hit testing.

In the cold probes, both of those arms produced matching pixel and page-metadata fingerprints natively and under Cortex-A9 emulation. The probe checks initial, second, middle, final and revisited first columns across successive spine loads. The edge fixture uses images without declared dimensions, a truncated PNG with a valid signature and an unsupported resource that the fixture declines to serve, as the reader does. The fingerprints cover column counts, source anchors, page-end anchors and links in addition to pixel output. The native real-book, RTL, mixed Unicode, overflow, deep geometry and link fixtures also matched. The headless Qt reader-session test passed with the visible ARM renderer under emulation.

## Forma results

Each mode starts with fresh renderer processes and a private profile. The three matched trials reverse the arm order in the middle trial. Forward timing excludes initial book open and wraparound to the first spine item. Warm timing covers retained turns. The viewport-image interval includes selecting the intersecting images and waiting for their decode promises. The trace records this interval inside the scroll phase.

| Workload | Mode | Cold forward ms | Warm turns ms | First open ms | All-image readiness ms | Viewport-image readiness ms | CPU/session ms | Peak PSS MiB | Resource requests / bytes per crossing |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| images | all | 758 / 953 | 150 / 204 | 1470 | 311 | 0 | 7210 | 157 | 5 / 202103 |
| images | none | 434 / 622 | 147 / 218 | 1106 | 1 | 0 | 5320 | 132 | 5 / 202103 |
| images | visible | 433 / 620 | 147 / 286 | 1129 | 1 | 1 | 5460 | 136 | 5 / 202103 |
| image-edges | all | 780 / 983 | 151 / 225 | 1485 | 305 | 0 | 7340 | 161 | 7 / 202223 |
| image-edges | visible | 485 / 718 | 150 / 288 | 1135 | 1 | 2 | 5640 | 134 | 7 / 202223 |

Cold and warm values show median / p95. Other timings and resource counts show medians. Peak PSS is the maximum sampled renderer process-tree footprint across trials.

Visible-only decoding cut median forward crossing latency by 325 ms (43%) on the image-heavy fixture and 295 ms (38%) on the edge fixture. Every paired trial improved. The image-heavy run used 24% less renderer CPU per session and 21 MiB less peak PSS. Resource counts and bytes were identical. Pixel and page-metadata fingerprints matched for every paired Forma run, including the no-decode arm on the standard image fixture.

The warm median stayed near 150 ms, while warm p95 rose by 82 ms on the standard fixture and 63 ms on the edge fixture. The viewport-image interval on warm turns reached 128–133 ms at p95. This is consistent with moving work from the spine crossing to the first turn that shows an image. The physical measurements are standalone renderer timings. The full reader check ran under ARM emulation.

The temporary payload was removed and all benchmark renderer processes exited. The installed renderer and WebKit checksums matched after the run, and Nickel retained its PID.

## Review decision

Keep visible-only decoding as an opt-in candidate for review. Reject blanket removal after the resize regression. Keep all-image decoding as the default while evaluating the effect of the slower first visit to some image pages. The [source geometry comparison](WPE-DOM-M3.md) holds all-image decoding fixed.

# WPE related WebViews feasibility

Keep one view. On the Forma, retaining neighbours sped up warm chapter crossings. Loading a related view during a foreground turn raised long-document p95 from 339 ms to 3.17 s. Three related views also left as little as 122 MiB of system headroom in the image-heavy workload. The review gate stays at RV0.

[RV0b](WPE-RV0b.md) investigates foreground preemption and foreground-only lazy retention.

## Probe

`wpe-related-view-probe` is an opt-in standalone target in the existing WPE fixture build. Each run uses a fresh renderer process, one WebKit context and two shared frame slots at 1440 × 1920. The independent control uses separate ephemeral network sessions within the same context. The related arms inherit their context, display and network session through the `related-view` construction property. The probe uses the retained layout and snapshot code, including the two animation frames, and shares the book renderer's stylesheet and content security policy. It serves only the fixture's exact publication URLs. See the [WebKit related-view API](https://webkitgtk.org/reference/webkit2gtk/2.40.0/property.WebView.related-view.html).

The four workloads use distinct XHTML spine items with short text, long text, a font and illustrations, or eight illustration sections. The image-heavy spine items use distinct resource URLs and rotate three PNGs extracted from checked-in EPUBs. The renderer checks source hrefs and page counts, rejects identical fingerprints on distinct pages, and verifies fingerprints when revisiting pages.

The contention runs start after a foreground snapshot has been copied to the frame pool. They measure ordinary foreground turns followed by turns during one adjacent layout at a time. The second neighbour is populated after the first. Reloads supply additional overlap samples. Adjacent snapshots are captured in separate steps. Frame time ends at the shared-memory copy, so these timings cover neither Qt painting nor E Ink refresh. The one-view arm also records sequential cold chapter crossings for comparison with warm captures when selecting an already laid-out view.

The sampler runs outside the renderer and records its process group with a 100 ms polling delay and at each named checkpoint. It reads PSS, RSS, private dirty and swap from `smaps_rollup` or `smaps`. It also logs helper PIDs, parent PIDs, device memory availability, swap counters, OOM kills where exposed, and CPU ticks. The checkpoint chain covers startup, each loaded and laid-out document, snapshots, repeated navigation, reloads, idle, release, a second idle and shutdown. The force-termination check is a separate run so it cannot affect the recovery comparison.

## Physical Forma measurements

The Forma ran firmware 4.38.23852 on ARMv7 Linux 4.1.15 with the minimal WPE WebKit 2.54.2 `-O2` runtime. The four configurations ran in rotated order over three fresh sessions per workload on `ebfc4e8b` with the RV0 probe changes. Peak PSS is the largest complete sample from those sessions. Including collection time, periodic samples were 159 ms apart at the median and 190 ms apart at p95. Increments are medians of the paired peak differences by trial. Minimum `MemAvailable` is device-wide with Nickel running. The standalone probe excludes the Qt reader. F0's roughly 102–115 MiB is historical context because its socket renderer and sampling differed.

| Workload | Configuration | Peak tree PSS MiB | Increment over one MiB | Minimum MemAvailable MiB | Web / network processes |
| --- | --- | ---: | ---: | ---: | ---: |
| Short | One | 119.5 | Baseline | 216.8 | 1 / 1 |
| Short | Two independent | 149.4 | +30.0 | 192.9 | 2 / 1 |
| Short | Two related | 145.0 | +20.0 | 188.3 | 1 / 1 |
| Short | Three related | 164.3 | +44.9 | 173.6 | 1 / 1 |
| Long | One | 129.8 | Baseline | 205.6 | 1 / 1 |
| Long | Two independent | 160.5 | +39.8 | 175.8 | 2 / 1 |
| Long | Two related | 152.6 | +29.8 | 186.6 | 1 / 1 |
| Long | Three related | 169.9 | +49.6 | 171.0 | 1 / 1 |
| Fonts and images | One | 118.2 | Baseline | 218.2 | 1 / 1 |
| Fonts and images | Two independent | 156.0 | +37.8 | 180.0 | 2 / 1 |
| Fonts and images | Two related | 147.7 | +18.9 | 192.0 | 1 / 1 |
| Fonts and images | Three related | 160.4 | +42.4 | 176.5 | 1 / 1 |
| Image-heavy | One | 160.8 | Baseline | 171.0 | 1 / 1 |
| Image-heavy | Two independent | 210.6 | +46.4 | 123.2 | 2 / 1 |
| Image-heavy | Two related | 174.9 | +11.4 | 160.4 | 1 / 1 |
| Image-heavy | Three related | 212.9 | +50.5 | 121.9 | 1 / 1 |

The third related view added a paired median of 24.7 MiB for short text, 19.8 MiB for long text, 23.5 MiB for fonts and images, and 39.1 MiB for image-heavy content. Two periodic samples were incomplete during renderer shutdown in the long workload and were excluded from peak PSS. Every named shutdown checkpoint reported zero remaining members.

### Process sharing and memory recovery

Each related arm exposed one WebProcess PID over the entire run, including neighbour loading, repeated navigation and reloads. Related views reported the same ephemeral network session object. The independent views reported different session objects and separate WebProcess PIDs, while the runtime used one NetworkProcess for both. In separate forced-termination runs, terminating the foreground WebProcess affected every related view and only the foreground independent view.

The image-heavy three-view arm illustrates where the resident memory went. These are per-process medians from the first idle checkpoint and the checkpoint 15 seconds after extra views were released.

| Process | Idle PSS / RSS / private dirty MiB | After release and idle PSS / RSS / private dirty MiB |
| --- | ---: | ---: |
| Probe renderer | 33.1 / 45.6 / 24.3 | 33.1 / 45.6 / 24.3 |
| WebProcess | 162.2 / 175.1 / 129.8 | 109.5 / 122.5 / 77.1 |
| NetworkProcess | 7.3 / 16.9 / 2.4 | 7.3 / 16.9 / 2.4 |

Immediate release left sampled PSS nearly unchanged. After 15 seconds, paired median recovery ranged from 21 to 32 MiB for two related views, 42 to 53 MiB for three related views, and 31 to 80 MiB for two independent views. Three related views retained a median 1–21 MiB more at the reload checkpoint than at the initial capture checkpoint, depending on workload. Those extra allocations drained with the process tree at shutdown. The run had no configured swap or swap activity and no observed spontaneous crashes or kernel OOM events. Nickel kept its PID and the installed renderer and library hashes matched after the run.

### Foreground contention

Times are median / p95 in milliseconds. Quiet results come from each arm's initial foreground-only turns. Competing results include turns across the initial adjacent loads and repeated loads of different spine documents. Cold crossings require the one-view arm to load and lay out the destination spine item. Warm crossings select an already populated related view at column zero.

| Workload | Two related quiet → competing | Three related quiet → competing | Cold → warm crossing |
| --- | ---: | ---: | ---: |
| Short | 152 / 168 → 322 / 465 | 154 / 167 → 344 / 475 | 372 / 400 → 158 / 193 |
| Long | 301 / 341 → 1660 / 3067 | 302 / 339 → 1716 / 3170 | 2910 / 2957 → 290 / 322 |
| Fonts and images | 153 / 171 → 304 / 550 | 152 / 174 → 301 / 581 | 450 / 498 → 164 / 212 |
| Image-heavy | 141 / 155 → 161 / 823 | 139 / 152 → 165 / 832 | 670 / 876 → 142 / 155 |

The long independent control reached 562 / 653 ms under competing loads, compared with 305 / 341 ms quiet. In the long three-related arm, a quiet turn took about 234 ms of tree CPU. A competing phase took a median 2540 ms of tree CPU for the adjacent load plus any overlapping turns, and peaked at 169.9 MiB PSS. Short, font-and-image, and image-heavy phases took 490, 525 and 935 ms of tree CPU respectively. The raw logs retain each load interval, overlap, CPU phase and process sample. All cross-run page fingerprints matched within each workload.

The native retained-view and source-location checks passed, and the ARM probe passed the three-view short workload under Cortex-A9 emulation with the stock Kobo7 loader. The hardware latency comparison ends at frame copy. Page paint, E Ink refresh and cancellation when a turn arrives require measurements in the reader.

### Cache model comparison

The same binary and fixtures ran the long and image-heavy workloads again with `DOCUMENT_VIEWER`. Results below show default → document viewer. Layout and reload times are medians. Repeated navigation p95 includes both columns on populated neighbours, or same-spine turns for the one-view control.

| Workload | Configuration | Peak PSS MiB | Minimum MemAvailable MiB | Initial adjacent layout ms | Adjacent reload ms | Repeated navigation p95 ms | Competing p95 ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Long | One | 129.8 → 126.7 | 205.6 → 212.8 | — | — | 344 → 343 | — |
| Long | Two independent | 160.5 → 159.6 | 175.8 → 179.6 | 6343 → 6359 | 5739 → 5778 | 372 → 373 | 653 → 658 |
| Long | Two related | 152.6 → 139.5 | 186.6 → 198.8 | 3364 → 3218 | 3255 → 3238 | 301 → 301 | 3067 → 3125 |
| Long | Three related | 169.9 → 176.7 | 171.0 → 164.8 | 3217 → 3280 | 3138 → 3199 | 314 → 319 | 3170 → 3077 |
| Image-heavy | One | 160.8 → 171.5 | 171.0 → 169.0 | — | — | 148 → 151 | — |
| Image-heavy | Two independent | 210.6 → 215.4 | 123.2 → 126.4 | 2163 → 2145 | 1168 → 1401 | 164 → 164 | 438 → 367 |
| Image-heavy | Two related | 174.9 → 198.4 | 160.4 → 136.2 | 1244 → 1246 | 1099 → 1191 | 168 → 155 | 823 → 788 |
| Image-heavy | Three related | 212.9 → 260.1 | 121.9 → 77.8 | 1266 → 1283 | 1033 → 1195 | 195 → 165 | 832 → 873 |

With three related views, the cache model raised the image-heavy peak by 47 MiB and reduced minimum headroom to 78 MiB. Long-document competing p95 stayed around 3.1 s. The alternate cache model offers no basis for adopting related views. The default-cache warm crossing gains are real, and revisiting the architecture would require device evidence that a requested foreground turn can remain responsive while adjacent work is active.

Both matrices completed with clean renderer shutdowns and matching frame fingerprints across cache models. Nickel retained PID 232 and the installed renderer and library hashes matched after cleanup. The run directories were removed and `/tmp` returned to its pre-run usage.

## Device payload and commands

Build the target from the standalone WPE CMake project using the minimal FW4 `-O2` library and the embedded GLib setup in [WPE-M8 validation](WPE-M8-validation.md#build-and-staging). Package from the worktree with the same WebKit and dependency entries.

```sh
cmake --build build/rv0/fw4 --target wpe-related-view-probe
sh platforms/webkit/footprint/package-fw4.sh "$WEBKIT_ENTRY" \
    build/rv0/fw4/wpe-related-view-probe "$GLIB_ENTRY" "$TEXT_ENTRY" \
    "$RUNTIME_ENTRY" build/rv0/forma --related-view-probe
```

The payload is `build/rv0/forma/wpe-fw4.tar.gz`. The tested bundle has SHA-256 `dff769a1cd55e7b0666d9126597814e417dd53b284a41a7d929ccf28578cbc7b`. It contains the standalone ARM probe, private WebKit library and helpers, injected bundle, fonts, fixture images and font, launcher, runner, notices and checksum manifest. After owner approval, confirm firmware `4.38.23852` and space on `/mnt/onboard` and `/tmp`. Use a new `/mnt/onboard/.adds/kobalt-rv0` directory for the archive and its contents. Use new `/tmp` directories for matrix logs and scratch profiles.

```sh
# On the Forma, after copying the selected archive into the new staging directory.
cd /mnt/onboard/.adds/kobalt-rv0
printf '%s  wpe-fw4.tar.gz\n' "$APPROVED_SHA256" | sha256sum -c -
tar -xzf wpe-fw4.tar.gz
(cd wpe && sha256sum -c SHA256SUMS)
sh wpe/run-related-matrix.sh /tmp/kobalt-rv0-results
```

If the default-cache results leave the three-view decision borderline, repeat with only the cache model changed.

```sh
sh wpe/run-related-matrix.sh /tmp/kobalt-rv0-document-viewer --document-viewer
```

Use a fresh private profile for each separate `--verify-failure` run. Compare the independent control with the related arms by substituting `two-independent`, `two-related` and `three-related` for the mode. Return the logs before removing the approved staging directory and temporary profiles.

```sh
profile=$(mktemp -d /tmp/kobalt-rv0-fault.XXXXXX)
XDG_RUNTIME_DIR="$profile" XDG_CONFIG_HOME="$profile" \
    XDG_CACHE_HOME="$profile" XDG_DATA_HOME="$profile" \
    ./wpe/run three-related short "$PWD/wpe/fixtures" --verify-failure \
    >/tmp/kobalt-rv0-three-related-fault.log 2>&1
```

# WPE reader appearance before navigation

Installing the requested font size before navigation reduced the Forma's median long-text crossing from 3.08 s to 2.03 s. The [combined comparison](WPE-DOM-M5.md) documents a broken-image pixel difference and tests a guarded variant. The paired experiment follows the [cold spine baseline](WPE-DOM-M0.md).

## Comparison

Both arms use the same renderer binary. The late arm uses the existing appearance path. The early arm sets `KOBALT_WPE_EARLY_APPEARANCE=1` and installs a small top-frame user stylesheet supplying the existing `--reader-font-size` property before `load_uri()`. It keeps the existing inline assignment, geometry query, pagination rules, resource waits, source-position behavior and two-frame snapshot barrier. This tests whether supplying the final size earlier avoids later work while retaining the existing final style semantics.

The renderer replaces only its own appearance stylesheet when a new navigation needs a different size. Same-spine reflow still sets the requested size in the layout bridge. Cross-spine preview and failed navigation use the existing restoration path, which reinstalls the confirmed appearance when reloading that document. The experiment is opt-in while it awaits review.

## Verification

The native oracle compared exact frame metadata and pixels between arms on the real-book, RTL, mixed Unicode, overflow, deep geometry and link fixtures. The retained fixture also exercised a cross-spine preview and a failed cross-spine preview at a different font size, followed by hit testing in the restored view. The retained fixture passed natively and under Cortex-A9 emulation. The existing headless Qt reader-session test also passed with the early-appearance ARM renderer under emulation. The emulated long, font-heavy and image-heavy cold probes matched page counts and fingerprints between arms.

The Forma comparison uses the same minimal WPE WebKit 2.54.2 runtime, fixtures, frame slots and trace events as DOM-M0. It alternates arm order and uses three fresh renderer sessions per arm and workload. Forward timings exclude initial book open and the wrap back to the first spine item.

## Forma results

Values are late → early. Both timings and memory results are sampled. Each arm and workload recorded 18 forward crossings and 48 same-spine turns across three sessions.

| Fixture | Cold forward median / p95 ms | Load call to finished, median ms | Style phase, median ms | DOM bridge, median ms | Warm median / p95 ms |
|---|---:|---:|---:|---:|---:|
| Long | 3078 / 3140 → 2026 / 2066 | 1092 → 1085 | 1036 → 5 | 1864 → 818 | 335 / 381 → 337 / 381 |
| Embedded fonts | 577 / 601 → 431 / 466 | 175 → 176 | 115 → 6 | 291 → 144 | 182 / 222 → 180 / 216 |
| Image-heavy | 786 / 980 → 763 / 970 | 238 → 244 | 26 → 5 | 459 → 402 | 150 / 204 → 154 / 202 |

| Fixture | CPU per session, median ms | Peak PSS, max MiB | First open, median ms | Resource requests per crossing, median | Resource bytes per crossing, median |
|---|---:|---:|---:|---:|---:|
| Long | 23540 → 17220 | 114 → 113 | 3579 → 2468 | 1 → 1 | 110945 → 110945 |
| Embedded fonts | 6840 → 6010 | 106 → 106 | 1517 → 1528 | 1 → 1 | 6443 → 6443 |
| Image-heavy | 7440 → 7200 | 158 → 158 | 1506 → 1471 | 5 → 5 | 202103 → 202103 |

Long-document forwarding improved in each paired session by roughly 1.04–1.06 s. The median style interval fell from 1036 ms to 5 ms, and the load-to-finished median held near 1.09 s. Both signals support eliminating most of the measured late appearance and forced-layout work. They do not establish an internal WebKit layout-pass count. Total renderer CPU for the long session fell by 27%. Same-spine timing stayed close to the late arm.

The embedded-font fixture gained 146 ms at the median crossing. Its first open served four resources totalling 891,726 bytes in both arms. Font-readiness p95 during forward crossings was 1 ms in both. The image-heavy fixture gained 23 ms at the median. The requested size arrives earlier, while the existing image decode wait still runs.

Page counts and fingerprints matched for every paired physical run. All renderer processes exited. The temporary device payload was removed, Nickel retained its PID, and the installed renderer and library checksums matched.

## Review decision

Retain early appearance as the DOM-M1 candidate. The gain is strongest for long text and embedded fonts, with unchanged resource delivery in these fixtures. Keep it opt-in pending review. The [image decoding comparison](WPE-DOM-M2.md) measures the remaining explicit decode work independently.

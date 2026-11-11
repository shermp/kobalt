# M4 reading pace

WebKit uses Kobalt's local word-based estimator after a qualifying forward
turn. The estimate uses chapter word counts and the current page layout.
The chapter graph follows source order for remaining-time calculations and
the Contents list follows publisher order. The shared estimator filters
rapid paging, jumps, backward turns, pauses and reflow, including a reflow
with the same page count.

Stats initially shows the book's word total. After an accepted word sample,
the existing Book cell can show the estimated time left. The estimate
starts with a 250 words-per-minute prior and adapts as accepted reading
time accumulates. When the word estimate is unavailable, the reader can
use its in-memory page-rate estimate.

The profile stores accepted word and time samples in
`reading-speed-webkit`. Routine and close checkpoints use the existing
activity transaction. WebKit's estimate comes from its own profile record.
CREngine on Kobo continues to use Nickel's reading estimates.

## Verification

The native progress lane covers the shared pace calculations. The WebKit
proxy check verifies source order for a TOC with reordered entries, including
after reflow. The profile check rolls back position, speed and history for
each pace record and verifies that updating one record preserves the other.
Both pinned Kobo SDKs passed their adapter checks. The host syntax check
requires `clang++`, which is unavailable on this host.

The ARM reader session learns one sample from the Trevelyan fixture, preserves
the standalone record, and verifies the restored estimate after a cold open.
The installed Libra Colour check captures the fresh word total and the learned
estimate after reopening. It uses the existing Stats workflow.

```sh
python3 tests/webkit/run-firmware.py \
  --virtualnickel /path/to/VirtualNickel \
  --installed-only --installed-stats \
  --allow-unsandboxed-experiment
```

The paired screen review is in `build/review/webkit-m4-pace/`. The Stats
entry and cell captions match CREngine's rendered pixels and widget bounds.
The remaining-time captions share the same bounds. The fixture displayed
7,316 words before learning and approximately 29 minutes left after
reopening. See [recorded results](m4-pace-results.json).

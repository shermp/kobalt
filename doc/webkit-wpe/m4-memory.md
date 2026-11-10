# M4 metadata scans and memory headroom

This continuation follows the text-query/Search commit `928a9b2`. It removes
page-buffer transfers from pagination/TOC scans and sets an explicit WebKit
memory-reclamation policy. It remains an experimental M4 slice, with feature
parity, physical-device performance and power use still outstanding.

The user approved committing this continuation in chat.

## Metadata without page buffers

Opening and reflow previously captured and copied every spine's first page and
every TOC destination, even though those scans only needed counts and source
locators. They now use `QUERY` with `kind: "layout"`. An optional fourth
argument carries a base64-encoded fragment identifier:

```text
KWRP/1 <id> QUERY <spine> <page> <base64-json> [<base64-fragment>]
```

The helper still loads resources and paginates the requested DOM. It returns
layout metadata through the existing JSON reply, without taking a snapshot,
creating frame files or consuming a frame slot. Missing fragments retain the
existing nonfatal navigation-error path, including restoration of the helper's
previous page. Metadata queries work while both frame slots are held.

The proxy validates the source URL, spine, page count, viewport and versioned
locator before using a result. Scans preserve the displayed frame and logical
source anchor. Relative publication links restore the displayed source before
asking the helper to resolve them, so a preceding scan/search cannot change
their base document. Saved positions, parser/cache formats and profile records
are unchanged. These operations stay on the document worker.

The native regression enforces a small visible-frame allocation budget for
opening and reflow, alongside the existing TOC, position, immutable-frame,
cancellation and saved-data checks. The actual ARM protocol check covers
metadata queries and missing fragments with held frames.

## Memory policy

Removing transfers alone did not reduce the measured peak below the container
limit. The helper now constructs its WebKit context with an explicit
`WebKitMemoryPressureSettings`: the smaller of WebKit's platform default and
512 MB, with a two-second poll interval. The existing conservative and strict
fractions remain unchanged, and no process-kill threshold is enabled.

This uses WebKit's public application API, without changing the prepared SDK.
The API's default limit derives from system RAM, up to 3 GB, and its default
poll interval is 30 seconds. Its thresholds trigger reclamation; this is not a
hard allocation cap or a guarantee that a process stays below the base value.
See [memory limit](https://webkitgtk.org/reference/webkit2gtk/stable/method.MemoryPressureSettings.set_memory_limit.html),
[poll interval](https://webkitgtk.org/reference/webkit2gtk/2.42.3/method.MemoryPressureSettings.set_poll_interval.html)
and the [context property](https://webkitgtk.org/reference/webkit2gtk/stable/property.WebContext.memory-pressure-settings.html).

## Reproduction and scope

The previous combined Search/keyboard/rotation run exited 137. A fresh,
instrumented baseline completed its rotations, so the original failure was
not reproduced or proven to be an OOM kill. However, both that baseline and
the metadata-only candidate reached the 768 MiB cgroup ceiling. The new policy
creates headroom in the same scenario; the results file records the final run.
This mitigates the observed pressure without claiming a root-cause proof for
the earlier kill.

Use the existing disposable firmware runner:

```sh
python3 tests/webkit/run-firmware.py \
  --virtualnickel /path/to/VirtualNickel \
  --installed-only --installed-search --installed-search-rotation \
  --allow-unsandboxed-experiment
```

The action types through the native keyboard, follows a search result and its
return control, opens Chapters, turns pages, cycles orientation, compares
CREngine and cold-reopens WebKit. It records cgroup current/peak/limit/events,
process RSS and container state under `installed-results/`. It requires at
least 32 MiB of peak headroom for this scenario and rejects a recorded OOM kill.
That is an experimental regression bound, not a device RAM requirement.

The container stays at 768 MiB. The runner removes its owned device and helper
tree on completion or failure. It does not touch the retained manual device.
The runtime remains an explicit unsandboxed experiment with offline restrictions.
Memory figures include the firmware and QEMU; they cannot establish native
Kobo memory use, latency or battery cost. Those measurements remain M4 work.

[Recorded results](m4-memory-results.json) distinguish the baseline, metadata
candidate, policy experiment and final verification. The earlier text-query
results remain a historical record. The focused native and Kobo checks are
reused after helper-only policy edits; full native and gallery validation are
not repeated for this backend-only change. Captured Search controls and reading
frames are compared with the approved slice to check for unintended UI changes.

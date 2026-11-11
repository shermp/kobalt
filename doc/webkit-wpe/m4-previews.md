# M4 page previews

The existing Navigate panel can now show the selected WebKit page and its
neighbors. The document worker requests the desired page from the helper
through the existing frame protocol. It checks that the reply matches the
requested spine and page, copies the pixels, and releases the helper buffer.
The reading frame and source locator remain available as the user previews
other pages.

The session scales the thumbnail and paints saved highlights through the
shared renderer. The scrubber retains its bounded strip of thumbnails and
reuses overlapping cards when moving one page. Off-screen full-size frames
are temporary. Accepting the middle card performs a regular navigation and
enables Return to the source point the reader left.

Layout revisions invalidate the strip even when the viewport and page count
match the earlier layout. Changes to saved highlight ranges or appearance
also refresh the strip. Pending requests are discarded when navigation,
layout or saved marks supersede them. Preview work stays on the document
worker, and an obsolete request can finish its current helper operation
before yielding to the latest request.

## Verification

The native proxy check covers off-screen pixels, unchanged reading frames
and source positions, relative links after previewing, wrong-page rejection
and dimensions after reflow. The Navigate check covers thumbnail invalidation
at the same viewport and page count. The native input lane exercises shared
touch and navigation behavior. Both prepared Kobo SDKs and the Qt6 package
check compile and verify the application. The host syntax check requires
`clang++`, which is unavailable on this host.

The production ARM session compares a highlighted off-screen thumbnail to
the original page, checks cancellation and then continues through the
existing layout and saved-position regressions. The installed diagnostic
uses the Trevelyan fixture and checks the Navigate entry, three-card strip,
overlapping card reuse, acceptance and return in both backends.

```sh
python3 tests/webkit/run-firmware.py \
  --virtualnickel /path/to/VirtualNickel \
  --installed-only --installed-previews \
  --allow-unsandboxed-experiment
```

The screen review compares the Navigate entry, strip, cover, promoted card
and accepted page in both engines. The menu and arrow controls match in
geometry and rendered pixels. The caption text occupies the same bounds,
with the page count digit reflecting each engine's pagination. The slider
and side cards share the same dimensions. WebKit's smaller cover has wider
margins, which can fill the visible part of its side card. Promoting the
cover shows the complete image.

The review is in `build/review/webkit-m4-previews/`. The user authorized the
commit in chat when the round recorded four seen screens, one unseen and no
requested changes.
Physical-device preview latency and e-ink behavior remain unmeasured.
See [recorded results](m4-previews-results.json) for the validation evidence.

# M4 vertical margins

The Layout panel's Vertical margins control now adjusts WebKit's page
height independently of the side margins. WebKit accepts separate top
and bottom settings, including older records with only a side margin.
Missing values use the shared reader default. The existing preview
reflows the current section, then apply repaginates at the same source
point. Page geometry, link and object rectangles, text selection and
hit testing use the effective content box.

The helper advertises `vertical_margins` in its frame metadata. The
extended `STYLE` command appends top and bottom insets in CSS pixels.
The original command applies its single inset on all sides. Kobalt uses
that command when the helper lacks the capability. If a smaller viewport
cannot fit the requested insets, the helper fits them inside the viewport
and retains the requested values for the next larger layout.

The standalone EPUB check verified asymmetric insets, clipping, a narrow
resize and source restore. The ARM reader session verified the existing
preview and apply path. Installed VirtualNickel captures show matching
vertical-margin controls and captions in both engines. The isolated
installed run peaked at 689 MiB with no main-thread blocks. A concurrent
firmware run reached the 768 MiB limit without an OOM and included an
additional firmware process.

The paired review is in `build/review/webkit-m4-margins/`. See
[recorded results](m4-margins-results.json).

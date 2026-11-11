# M4 writing directions

WebKit lays out reflowable EPUB chapters in horizontal or vertical columns
according to the publisher's CSS and document direction. Pages advance in
source order. The axis used to translate a page comes from the chapter's
writing mode. The existing reader uses the OPF spine's
`page-progression-direction` for horizontal swipes and the default tap
zones. Logical Next and Previous actions keep their meaning, including
when successive chapters use different writing modes.

Source locations, fragment destinations, page previews and search use the
same column geometry. Vertical selection handles follow the horizontal
edge of their terminal glyph. A handle is shown only where the helper
can identify that glyph edge inside the reading area.

The focused firmware fixture exercises Arabic and Japanese text, both
vertical block directions, and reverse inline flow. The existing EPUB
diagnostic also checks navigation and source restoration in the
representative book. The native proxy regression covers vertical handle
validation and the progression flag passed to the shared controls.
The existing CREngine layout warning and fallback remain in use on the
CREngine path. Fixed-layout EPUBs are outside this reflowable layout support.

See [verification results](m4-writing-directions-results.json). Run the
helper regression with the existing firmware runner's `--epub-only`
action. The native layout label and both Kobo SDK adapter checks also
passed. The firmware run used an offline VirtualNickel device in explicit
experiment mode.

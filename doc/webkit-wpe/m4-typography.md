# M4 typography

The existing Text panel sends alignment, hyphenation and outline thickness
to the WebKit appearance draft. Applying a draft preserves the source
position. Justification and left alignment affect ordinary prose and retain
publisher-centered text, headings and figure captions. Auto restores the
publisher's alignment. Thickness adds a centered glyph stroke without
changing the glyph advances, page count or saved locator.

Hyphenation Auto uses the publisher's CSS. Off disables hyphenation,
including discretionary soft hyphens. On asks WebKit for automatic
hyphenation and uses discretionary breaks already in the text. Automatic
language patterns need libhyphen dictionaries in WebKit's runtime search
paths. The current Kobo helper bundle includes no such dictionaries. Kobalt's
CREngine XML dictionaries use a different format.

The new helper advertises these controls in frame metadata. The proxy
retains compatibility with both earlier `STYLE` forms. No saved-data
format changes are required. Outline thickness uses WebKit's CSS text
stroke. CREngine's variable-font selection, optical-size axis and specialized
line-spacing algorithms remain backend-specific.

The firmware EPUB regression measures prose, heading, dedication and
caption rectangles and checks discretionary breaks. It compares the page
image before and after thickness while asserting equal pagination and
locators. The ARM reader session checks that the isolated draft repaints,
then applies the same frame and source point through the production session.
The installed check uses the original Text panel and compares the thickness
row with CREngine. The row matches in size, position and pixels. The offline
VirtualNickel session logged no main-thread blocks. See
[verification results](m4-typography-results.json).

The local review at `build/review/webkit-m4-typography/` contains the paired
Text captures, rendered typography samples and writing-direction pages.

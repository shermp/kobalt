# M4 saved-position import

On a Kobo opening without a matching WebKit checkpoint, the reader can
resume from a verified Nickel location or a matching CREngine XPointer.
An existing WebKit checkpoint has priority. An explicit restart bypasses
import. An XPointer from a different publication fingerprint is ignored.

For an XPointer, CREngine opens its original DOM version on the document
worker and resolves the saved point. The existing Nickel codec exports a
location and decodes it again. Import proceeds only when the decoded point
matches the original. This rejects a chapter-start approximation from the
encoder. CREngine releases its document before the WebKit helper starts.

The proxy finds the unique spine resource and the helper resolves the
original XHTML location. Nickel ID offsets count UTF-16 code units. Structural
`point()` offsets count UTF-8 bytes, and the path counts text and comments
among child nodes. WebKit rejects duplicate spine resources or IDs, missing
or hidden targets, out-of-range offsets and offsets inside a Unicode
character. It builds a versioned locator with text assertions, then
restores that locator through the normal source-position path.

An unresolved import leaves the ordinary WebKit opening position available.
The diagnostic records the outcome when WebKit receives a candidate. Subsequent
saves store a WebKit source checkpoint through the existing background
writer. The original CREngine and Nickel records remain available when
switching back. WebKit progress, bookmarks and annotations continue to use
their own saved positions.

The firmware EPUB diagnostic checks equivalent ID and structural positions
after a supplementary Unicode character, as well as rejected malformed
offsets and ambiguous targets. The ARM reader session uses the production
Nickel codec to verify that a native location and its saved CREngine
XPointer resolve to the same WebKit point. It also checks WebKit checkpoint
precedence and rejection of a changed-edition fingerprint.
See [verification results](m4-position-import-results.json).

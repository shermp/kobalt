# WPE-M7 word selection

On a held word, the retained renderer returns the word, two source positions and its clipped rectangle. The DOM query uses WebKit caret hit testing and Unicode word segmentation within one text node. Kobalt discards results from stale frames and uses the existing dictionary panel and source positions. Book links take priority on a hold. Multiword selection, selection across inline elements and annotation editing are separate work.

The query publishes no pixels. It names the confirmed renderer revision. The host rejects replies outside the query viewport. A turn or reflow can invalidate a query, and renderer shutdown drains it before releasing the view. Install the host and renderer together.

The native WPE ReaderSession regression hit a known word in a real EPUB, verified the rectangle covers the hit and checked both counted offsets against the publication. Native session, dictionary, links and annotation input regressions passed. The native and FW4 ARM renderers built, the retained-view source-location fixture passed and both Kobo SDK syntax checks passed.

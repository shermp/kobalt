# WPE-M7 search

WPE uses the existing reader search panel for the whole book or current spine item. The publication worker indexes one admitted XHTML item at a time and sends bounded batches with excerpts and fingerprint-bound source positions. The renderer stays on the confirmed page until the user selects a result. Result captions show the spine item while the whole-book page number is unknown. The dictionary can open search for the selected word.

The initial WPE search matches case-insensitively within XHTML text runs. Phrases split across markup, CSS visibility and the before-page and after-page scopes are outside this slice. A source that exceeds the indexing bounds stops the scan with the existing partial-progress presentation. Saved book state and storage formats are unaffected.

The native WPE ReaderSession fixture checked bounded batches across chapters, cancellation, case-insensitive matches, exact source navigation, unknown page numbers and unchanged reading position before selection. Both Kobo syntax lanes and the repository source checks passed. Native search, links, dictionary and reader-session regressions were run in the pinned Linux SDK.

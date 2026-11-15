# WPE-M7 annotations

The existing annotation mode can create, adjust and recolor highlights using fingerprint-bound WPE source locations. Drag selection orders complete words and supports handle adjustment within the active spine item. The renderer returns clipped highlight rectangles and endpoint edges for the confirmed frame. The existing writer rejects overlap and preserves records whose positions the active engine cannot resolve. Those records remain editable with their original engine.

The publication worker indexes a bounded XHTML spine item for excerpts and overlap checks. It records source offsets without relying on rendered page counts. WPE reuses the existing annotation store and writer, with no saved-data schema change. The reader restores the overlay through its normal markup painting path.

The native WPE fixture checked reverse selection of a two-word passage, visible endpoint directions, save and recolor, overlap rejection, reopen and overlay redraw. The existing desktop selection and cross-page drag checks, highlight session and storage checks, and both Kobo syntax lanes passed. WPE cross-page handle continuation and full legacy-anchor interoperability remain distinct from the current-spine selection path.

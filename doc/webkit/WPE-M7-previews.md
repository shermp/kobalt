# WPE-M7 navigation previews

Navigate uses the existing chapter bar and adjacent-page thumbnails. The current thumbnail scales the confirmed image on the worker. To produce an adjacent thumbnail, the retained renderer snapshots the requested page in the free shared slot and restores the confirmed document and scroll position before replying. No preview advances the source position or changes the authoritative frame. A foreground turn supersedes a queued thumbnail, and closing drops thumbnail callbacks. The chapter bar omits whole-book progress until it is known.

The native WPE ReaderSession regression compared the thumbnail with the frame from the actual forward turn. It checked that previewing left the initial pixels and source anchor intact and that cancellation suppressed the thumbnail while a queued turn completed. Retained source locations, native reading and link regressions, both Kobo syntax lanes, and the native and FW4 renderer builds passed. The preview protocol requires matching Kobalt and renderer binaries.

## Whole-book pagination

An accepted WPE frame records the page count of its spine item for the current layout. Kobalt can show an exact page number after it has laid out the preceding items, and exact whole-book progress after observing every spine item. A different viewport or typography starts a new count. Previews contribute no observations.

The native fixture checked that total pages became known after visiting the full spine and returned to pending after a resize.

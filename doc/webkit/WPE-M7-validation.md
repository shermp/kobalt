# WPE-M7 validation

Production changes through WPE `fa55777c` were built against Blaze `55870b001b401cb52b73bac53dedb0c39c269fe9`. The pinned Qt5 and Qt6 Kobo SDKs passed their package checks and adapter suites. The native and FW4 ARM WPE renderers built and the retained source-location fixture passed.

In native strict-main-thread testing, the reader opened a 180-entry EPUB, jumped deep into the book, turned repeatedly in both directions, and restored the exact source position and pixels. Repeated reopen and close cycles included closing with a preview outstanding. The renderer exited, the callback stayed retired and the host did not accumulate descriptors. The same suite covers a killed renderer with a turn pending, preservation of a held image, and preview cancellation followed by a confirmed turn.

The combined Qt5 build ran in a disposable VirtualNickel Touch C with firmware 4.38.23852 and a 512 MiB container cap. Holding a word opened the dictionary. Searching for the word reached a match in the second chapter. Navigate opened without a fabricated whole-book progress bar. Closing returned to responsive Nickel and removed the renderer, WebProcess and NetworkProcess. The instance and volume were removed. The cap describes the emulated run and does not measure physical memory pressure or establish a peak.

## Supported and deferred

- The WPE reader supports current-page EPUB footnotes and internal links, single-word lookup, whole-book and current-spine search, temporary match highlights, and adjacent-page previews.
- Persistent highlight editing and multiword drag selection remain deferred. They require range endpoint geometry, overlap checks and saved-overlay redraw against the existing annotation transactions. Current WPE queries cover the active spine item and the tested transient geometry.
- Search across markup boundaries, before-page and after-page scopes, and exact whole-book progress remain unavailable on WPE. The controls expose the supported search scopes.
- The Qt6 host package and adapters built and passed tests. Firmware 5/6 rendering and physical Forma stress remain unverified for this M7 build.

# Provisional integration architecture

No production integration or refactor was performed. M0 verifies the plan's separate helper can render through public snapshots in firmware 6.x. Keep its production protocol and lifecycle provisional; M1 still requires approval.

## Existing boundaries

- `src/engine/crengine/engine_document.h`: concrete CREngine adapter, owned by one engine thread. It owns parsing, page layout/rasterization, logical positions, chapter/navigation data, search, selections and annotation geometry. Page images own their pixels.
- `src/reading/reader_session.h` and `.cpp`: GUI-facing asynchronous facade, worker-owned engine access, cancellation/source leases, adjacent-page preparation, request revisions and stale-result rejection. It directly depends on CREngine types today; do not extract a generic interface prematurely.
- `ReaderFrame` in the session header: image plus immutable state, resource statistics, links and chapter data. Markup carries generation and image identity; damage refers to a specific previous image. Preserve these associations across any process boundary.
- `src/publication/publication.*`: bounded archive/member inspection, protection handling, sizes and fingerprints. `resource_source.h`, `ResourceCache`, local/native adapters and source leases supply owned, bounded resource access. Reuse this infrastructure instead of adding another EPUB ZIP/parser stack.
- `src/platform/api/position_codec.h`: host position conversion currently takes `EngineDocument`. A later abstraction must preserve native records and CREngine XPointers rather than assuming DOM offsets replace them.

## Later design constraints

Use asynchronous versioned requests with document, layout and request identities. Keep all renderer IPC and buffer waiting on workers. Publish a frame only when its source lease and request remain current. Transfer owned pixels or a reference-counted shared-memory lease whose reuse cannot change an already published image. Queue persistence away from the reading main thread; do not add a synchronous IPC wait to make the existing synchronous engine API appear interchangeable.

Use a helper-specific environment, private libraries and independent subprocess lifecycle. Do not load modern GLib/libstdc++ into Nickel. Preserve WebKit's subprocess sandbox and reject startup failure explicitly. The authorized simulator experiment exception must remain outside production defaults. Own and reap the entire subprocess tree: the firmware proof required a native supervisor to collect children that otherwise remained zombies after the UI helper exited. Start with the public snapshot API before designing a custom WPE buffer backend. Measure snapshot latency/copying before selecting shared-memory optimization.

Keep CREngine and its reading data operational. Source positions, EPUB resource origins, trusted-script isolation, pagination, annotation mapping, crash recovery and physical-device compatibility remain future work. No backend-independent position schema or migration is approved here.

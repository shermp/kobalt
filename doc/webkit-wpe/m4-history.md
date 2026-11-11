# M4 reading history

WebKit records active reading intervals through the shared history recorder
on SQLite profiles. Start and end anchors use version-1 source tokens and
the same content-bound book key as WebKit annotations. This lets the reader
preserve history from both engines for a publication. If an anchor is too
long or unavailable, the recorder saves the duration with an empty anchor.

Routine activity and close commit the interval snapshot and WebKit source
position together. The background writer uses the shared retry and durability
rules. Sleep pauses the existing active clock. A short sleep retains the
interval UUID, and the session timeout can complete an interval before a new
one starts on wake. WebKit uses a session pace estimate in memory.

See [Reading sessions](../storage.md#reading-sessions) for profile ownership,
backup and crash recovery.

## Verification

The native history, reading-book and lifecycle checks cover the shared
transaction, retry and suspend behavior. The history model exercises source
and XPointer intervals for the same publication and checks that an oversized
source token leaves an empty anchor. The ARM session test opens the Trevelyan
fixture, navigates between two source positions, pauses the interval over
short sleep and verifies the completed row and saved source position through
the profile writer. The test also checks that the existing CREngine record
fields survive the close.

The installed Libra Colour run exercises Nickel sleep, wake, close and cold
source-position restore with the Qt6 diagnostic package. Its diagnostics
contain no main-thread blocking or failed history, activity or close saves.
Both pinned Kobo SDK checks passed. The host syntax check requires
`clang++`, which is unavailable on this host.

See [recorded results](m4-history-results.json).

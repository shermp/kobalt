# M4 renderer recovery

The WebKit proxy can recover an active book after the helper exits, the
content process terminates or a render request times out. It saves the last
complete display in memory before navigation, reflow or a metadata query.
On a recoverable failure the document worker closes the helper, opens the
same publication and reapplies the viewport and appearance. It restores the
last source locator and retries the interrupted operation once. The reader
can continue displaying its current page throughout this work.

The worker rechecks the publication fingerprint from the original inspection
before reopening. A cancelled lease blocks the restart. The restored source
point and pagination must agree with the last display. When the previous
bitmap is retained, the restored pixels must agree too. This lets a recovered
metadata query keep the same displayed image and its geometry.

Navigation and reflow only become visible after the operation completes. A
failed recovery retains the previous image, locator and layout together and
reports the error through the existing reader path. A later navigation can
attempt another reopen. Frame capture after a failed turn uses the retained
display. A superseded background query allows later selection on the same
page. Invalid replies and ordinary link or source rejections surface as
errors. Initial opening can fall back to CREngine through the existing path.

The source checkpoint format and annotation namespace apply after recovery.
Reopening uses the in-memory locator on the worker. Only a completed operation
can publish a new reading position for the background checkpoint writer.

## Verification

The native proxy regression covers a crash after a frame reply and before its
release acknowledgement, a query failure in another spine, successful and
failed reflow recovery, and a second failure that keeps the previous display.
It also checks cancellation before restart, selection after an abandoned
query, rejection of invalid protocol replies, and refusal to reopen a
publication that changed on disk. Focused session and main-thread checks
exercise the surrounding worker and save contracts. The native lifecycle lane
covers the existing reader behavior.

Both prepared Kobo SDK checks and the Qt6 diagnostic package check cover the
final application code. The standalone host syntax script could not run
because this host lacks `clang++`. The prior ARM helper bundle is reused.

The installed check uses the Trevelyan fixture on Libra Colour firmware in
VirtualNickel. It stops the content process belonging to the disposable
reader and checks that the next tap produces exactly the requested page. It
then returns to the original page and compares the bitmap payload. The test
checks the Nickel boot identity, one recovery attempt and helper cleanup.
The production ARM session also exercises normal suspend and resume.

```sh
python3 tests/webkit/run-firmware.py \
  --virtualnickel /path/to/VirtualNickel \
  --installed-only --installed-recovery \
  --allow-unsandboxed-experiment
```

The earlier stop of the whole emulator during same-boot reopening remains
unresolved. This check covers renderer failure while Nickel stays alive.
Physical-device recovery timing and power remain unmeasured.

See [recorded results](m4-recovery-results.json) for the validation evidence.

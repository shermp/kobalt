# WPE-M7 renderer recovery

After a transport failure, the next WPE reading operation starts a renderer with its own socket and frame pool. It takes its position and chapter pagination from the last frame that ReaderSession accepted. The failed process and any outstanding resource work drain separately. Image leases from the failed process remain readable while the replacement writes to new mappings. An interrupted operation does not advance the confirmed position.

The native strict-main-thread ReaderSession WPE regression stops the renderer before submitting a turn, kills it while the turn is outstanding, and verifies that the prior image and source position remain current. It then turns forward and back through a new renderer and checks the retained old image and the restored page. The regular reader, bookmark and reading-book session suites passed. The Kobo Qt5 and Qt6 syntax checks passed. This run did not inject faults into Nickel or exercise a low-memory device.

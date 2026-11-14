# WPE-M2.2 physical Forma check

The Forma passed the physical rendering gate with Bubblewrap disabled for the standalone fixture. The same five M1 binaries produced the expected results and exited with status zero.

The first device run failed before WebKit could start its helpers. The exact stripped M1 fixture ran from the Forma’s onboard VFAT volume and loaded its private WPE library. It then exited with status 133 (SIGTRAP) when its compiled-in launcher could not execute `/usr/bin/bwrap`. The device had no file at that path. No snapshot was produced.

One Codex agent ran the check using GPT-6.1 Sol. The source base was `1ee09151206c021eba34a73c2336ff005ed8fd46` and the local Blaze reference was `a4bb4d1cf86acaa404064dca184a0e6708a43b00`. The Forma ran firmware 4.38.23852 on Linux `4.1.15-00287-gc98fdced63d6`, ARMv7 Cortex-A9 and Kobo glibc 2.19. The M1 Nia simulator used the same firmware generation and glibc with a QEMU Cortex-A9 CPU and reported a container sandbox fallback. The Forma took the Bubblewrap launch path.

The [M2.1 package](WPE-M2.1.md) supplied the exact five hashed M1 ELFs (94,430,884 bytes). The device verified both package manifests. The firmware loader at `/lib/ld-linux-armhf.so.3` resolved the fixture’s WPE library under the private install and system libc, libm, libpthread, libdl, librt and libresolv under `/lib`. The loader also reported the firmware’s `/lib/libarmmem-v7l.so`. The injected bundle was not observed in a running WebProcess. The static archive inventory and two private shared objects are recorded in the [M1 closure](WPE-M1.md).

## First run with the default sandbox

After the approved installation, run the following from the device shell. The captured run used a separate process group with a 40-second watchdog. It sampled package processes through `/proc` and recorded the log and resource checks before uninstalling.

```sh
cd /mnt/onboard/.adds/kobalt-webkit
sha256sum -c tests/SHA256SUMS
sha256sum -c tests/LAUNCHER-SHA256SUMS
LD_LIBRARY_PATH="$PWD/lib" /lib/ld-linux-armhf.so.3 --list tests/wpe-snapshot-fixture
setsid bin/run-fixture > logs/fixture.log 2>&1
status=$?
cat logs/fixture.log
printf 'fixture_exit=%s\n' "$status"
```

Local evidence is under `build/webkit-m2.2/`, including `supervise.sh`, `forma-fixture.log`, `socket-probe.log` and `forma-uninstall.log`. The launcher reported `Unable to spawn a new child process: Failed to execute child process … /usr/bin/bwrap … (No such file or directory)`.

## First-run checks on the Forma

- The files copied to the isolated VFAT directory as regular files and the fixture executed directly there. The embedded build RPATH directory was absent.
- A separate transient ARM probe bound an AF_UNIX pathname under the device’s `/tmp` tmpfs, then removed its socket and scratch directory.
- The failed launch left no live fixture or helper process. The launcher removed its run directory. Uninstall removed the isolated payload and diagnostic log after retaining the evidence locally.
- Free space on `/` stayed at 43,772 KiB throughout. Onboard free space returned to 6,135,616 KiB after uninstall. Nickel remained at the original PID throughout the checks.

Helper startup, physical A→B→A pixels, frame and FD behavior, clean exit after rendering, and meaningful renderer memory or timing measurements remain unverified. Sampling caught the fixture while it was exiting and yielded no meaningful resource baseline.

## Retry with Bubblewrap disabled for the fixture

The approved retry uses the same checksum-verified fixture payload and launcher. WPE 2.54.2 reads the environment override in its WebProcess pool and skips sandbox setup for the fixture's process tree. Use it only with this fixture's built-in HTML while running as root. The override lives in the invocation and does not carry into later device sessions.

```sh
cd /mnt/onboard/.adds/kobalt-webkit
WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 setsid bin/run-fixture > logs/fixture.log 2>&1
```

The fixture started both `WPENetworkProcess` and `WPEWebProcess` from the private VFAT install. All six BGRA8888 frame records matched M1 byte for byte in dimensions, stride and SHA-256. The run covered red → blue → red, resize, delayed mutation, cancellation and a fresh snapshot after cancellation. The fixture exited with status zero. The supervised process group had no live members, and the launcher's temporary directory was gone.

| Process | FDs in the sample with both helpers | RSS (KiB) | PSS (KiB) |
| --- | ---: | ---: | ---: |
| Fixture | 13 | 21,632 | 11,136 |
| NetworkProcess | 11 | 17,088 | 7,773 |
| WebProcess | 19 | 45,056 | 32,452 |

The sample summed to 51,361 KiB PSS. The supervisor read each process in sequence, so this is a short-run sample rather than a measured peak. The supervised run took approximately 1.06 seconds, including monitoring. The fixture and network helper had live socket FDs and mapped the private WPE library. The short sample did not capture the WebProcess mappings or establish individual helper exit statuses. The M1 fixture checks owned pixel bytes in memory. FD-backed frame handoff and consumer lifetime are scheduled for M3.

The retry left root free space at 43,772 KiB and removed the isolated installation. Onboard free space was 6,135,612 KiB after the retry, one 4 KiB allocation unit below the first run's baseline. Nickel retained the same PID. The retry log, supervisor and uninstall evidence are under `build/webkit-m2.2/` in `forma-unsandboxed.log`, `supervise-unsandboxed.sh` and `forma-unsandboxed-uninstall.log`.

Fontconfig still reported a missing default config. This fixture validates its built-in colors, and text rendering awaits a text fixture. Bubblewrap compatibility and any use of the unsandboxed runtime with external content remain outside this result.

Staged: NO. Committed: NO. Pushed: NO. Next milestone started: NO.

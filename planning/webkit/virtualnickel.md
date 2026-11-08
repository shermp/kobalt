# Simulator findings

Inspected the local private checkout only. Nothing was fetched from its private GitHub repository or edited in tracked source in that checkout. Existing named devices were neither launched nor changed. The ABI/security tests mounted one existing firmware 6.0.274403 system volume **read-only**, in temporary `--rm`, `--network none`, `--read-only` containers, using the existing `vn-offline` guard.

## Child executable limitation

`internal/launcher/command.go:nickelEnv()` sets `QEMU_LD_PREFIX` only when `Generation() != "5"`. This project's generation `5` includes firmware **5.x and 6.x**. Its root is `/firmware/rootfs`.

`src/observer/control.cpp:runsInGuest()` refuses to wrap ARM ELF programs when that variable is absent. `guest_exec.h` constructs an explicit `qemu-arm -cpu max -L <prefix>` command; the exported `execve`, `execv` and `execvp` wrappers use it. They do not establish coverage for raw syscalls or GLib's internal spawn implementation in a separate WebKit process. Preloading the Qt-dependent observer into WebKit would also defeat dependency isolation and has not been proposed.

A standalone fork/exec probe without an inherited prefix failed with `arm-binfmt-P: Could not open '/lib/ld-linux-armhf.so.3'`. Passing `QEMU_LD_PREFIX=/firmware/rootfs` made that probe pass on this binfmt-enabled host. The initial ABI probe alone was not a running-Nickel test. The later full WebKit experiment below verifies subprocesses alongside Nickel on this WSL2 host; neither establishes portable macOS behavior.

The smallest **launch-environment** remedy is to export the prefix for Qt6 as well. A separate [candidate diff](virtualnickel-child-prefix.candidate.patch) includes the existing Go test's new expectation. It is not applied or approved. Existing `tests/check_guest_exec.py` calls a native `/bin/true` child and therefore does not verify nested ARM child execution. Extend it with an ARM executable before declaring this gap fixed. Run the normal checks and existing firmware 4/6 integration checks with both display backends after an approved change.

## Separate sandbox blocker

Under QEMU **10.0.13**, using firmware glibc 2.35:

| Kernel operation | ARM guest result | Native result in same container policy |
| --- | --- | --- |
| `unshare(CLONE_NEWUSER)` | EINVAL | Success |
| `unshare(CLONE_NEWUSER | CLONE_NEWNS)` | EINVAL | Success |
| `prctl(PR_SET_NO_NEW_PRIVS, 1, ...)` | Success | Success |
| `prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, ...)` | EINVAL | Success |
| `seccomp(SECCOMP_SET_MODE_FILTER, ...)` | ENOSYS | Success |

The probes add only allow-only filters to disposable processes; they cannot relax inherited restrictions. Both run behind `vn-offline`. Logs are `build/webkit-m0/logs/sandbox-arm.log` and `sandbox-native.log` in the feature worktree; `sandbox-arm-trace.log` also records guest syscall results. Host user namespace creation also passed. These results localize the problem to the guest execution path rather than a blanket host/container prohibition; they do not test a real Bubblewrap executable.

`Source/WebKit/UIProcess/Launcher/glib/BubblewrapLauncher.cpp` constructs a libseccomp policy, supplies `--seccomp`, and requests namespaces. An ARM Bubblewrap helper cannot straightforwardly perform these measured failing operations. A native helper cannot blindly load an ARM-architecture BPF policy into a native QEMU process. The child-prefix fix alone does not solve this.

A sandbox-preserving integration still requires review of native namespace/seccomp mediation or another supported approach. For M0, the user authorized a simulator-only per-run `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` exception to investigate rendering. Keep `ENABLE_BUBBLEWRAP_SANDBOX=ON`, require an explicit experiment argument, and preserve `vn-offline`, the ordinary container policy and external-network restrictions. Native runs retain sandboxing. Privileged containers and `seccomp=unconfined` remain outside scope. This exception does not validate WebKit sandbox compatibility or constitute a complete security remedy.

## Existing infrastructure to reuse

`tools/device.py`, the CLI/control stream and viewer API already supply isolated-device lifecycle, condition waits, input and frame captures. Runtime logs include per-boot files and main-thread waits. The launcher supports CPU/cpuset and memory caps; cgroup usage includes QEMU and charged caches. Stop and delete only newly created test devices after copying useful evidence. No new simulator automation framework is needed.

This simulator runs userspace against the host Linux kernel; it does not boot the Kobo kernel. Successful future sandbox tests here will not establish physical-device kernel compatibility, e-ink timing or battery consumption.

## Running-device proof

The local simulator checkout was `154ff91578775409b40f6a3e98ff49f11c455a77`. Its normal prepare step refreshed a stale runtime image and observer/offscreen plugins; the existing prepared Qt 6.5.2 build was reused, with no Qt SDK rebuild. The refreshed runtime image is `2dc8b8a34e6c326cdc3ade9deedf8574e624e153c3937bd1961cd87d105bf9d6`, still QEMU 10.0.13. Earlier read-only ABI probes used runtime image `c52886cd61495cda7e61a7daa1fa5625384a39f26c8b8dfa004be3f232b2716d`.

A temporary driver used `tools/device.py`: prepare a uniquely named Libra Colour 6.0.274403 instance, `start(wait=False)`, `wait_library_ready`, copy the experiment bundle into that device's `artifacts/`, capture stock Nickel, execute the probe, verify a command ping, capture Nickel again, then stop and delete the exact owned instance in `finally`. No saved device was booted or reset. The initial failed-cleanup instance and all supervised instances were deleted, and their directories and system volumes were checked absent.

The helper runs through the existing native `vn-offline` executable, with normal container security and the launcher's external-network restrictions. `qemu-arm -U LD_PRELOAD` removes the Qt observer preload from the helper; a private `LD_LIBRARY_PATH` applies only to the QEMU guest. `QEMU_LD_PREFIX=/firmware/rootfs` and `QEMU_CPU=max` are inherited by WebKit's own ARM children. The per-run sandbox-disable variable and explicit probe argument are supplied together. Nickel's environment is not changed.

The fixed WebKit prefix is `/instance/artifacts/kobalt-wpe-m0`, already inside the launcher's existing artifact mount. This release ignores `WEBKIT_EXEC_PATH` in non-developer builds, so the cross-build uses that fixed experiment prefix and DESTDIR staging. Mandatory subprocesses and the injected-bundle module are found without adding mounts or changing the simulator launcher.

The first successful render exposed a reaping issue: both WebKit children exited but remained zombies after the UI helper returned. A native run-owned supervisor sets `PR_SET_CHILD_SUBREAPER` before launch and waits for adopted children after the helper exits. Normal completion and intentional WebProcess SIGKILL then leave no WebKit processes or zombies. The supervisor does not alter network or sandbox policy. Production process supervision remains future work.

Repeated capture, DOM repaint, crash containment and cleanup passed with one CPU and both 768 MiB and 512 MiB memory caps. A post-crash fresh start also passed at 512 MiB. Nickel remained responsive; no OOM events were recorded. The 512 MiB whole-container peak was close to its cap, so do not extrapolate to larger books. See [rendering.md](rendering.md) and the retained resource records.

The final diagnostic also selected `cortex-a7` for the helper and its inherited WebKit children, without changing Nickel's `max` model. The same rendering, repaint, crash and restart checks passed with identical pixels at 768 MiB. This is a targeted ISA check, not full emulation of a specific physical Kobo. Its resource evidence is recorded separately from the `max` runs.

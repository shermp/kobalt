# Kobo sandbox compatibility

Stock Kobo kernel features are a compatibility constraint for this backend.
Firmware 6 userspace and glibc compatibility do not establish namespace or
seccomp support. VirtualNickel runs ARM userspace through QEMU on the host
kernel; it does not boot a Kobo kernel.

## Published kernel evidence

The [Kobo source repository](https://github.com/kobolabs/Kobo-Reader/tree/7a762964e7fa71ad8e8df50dd1b6eee721a43217/hw)
contains these configurations:

| Source configuration | Namespaces | Seccomp filters |
| --- | --- | --- |
| Libra Colour / MediaTek `aud8113tp1_defconfig` | `CONFIG_NAMESPACES` explicitly disabled | `CONFIG_SECCOMP=y`, `CONFIG_SECCOMP_FILTER=y` |
| Libra 2 `imx_v7_kobo_defconfig` and `imx_v7_ntx_defconfig` | `CONFIG_NAMESPACES` explicitly disabled | `CONFIG_SECCOMP` explicitly disabled |

The MediaTek `aud8113tp1-kobo_defconfig` is a symlink to `aud8113tp1_defconfig`.
The [audit record](kernel-audit.json) retains the repository commit, archive Git
blob identities, archive/config SHA-256 values and exact internal paths.
Archives were checked against their published Git blob identities before
inspection. These are published build inputs, not the configuration of an
observed running device. Updates or other models may differ. Omitted defconfig
options remain unspecified; they are not recorded as disabled.

These configurations cannot support the current namespace sandbox as written.
Bundling Bubblewrap cannot supply missing kernel functionality. The current
required sandbox therefore has a physical-device compatibility blocker. The
explicit device opt-in described below permits development use without that
sandbox; actual hardware operation still requires validation.

## Required operations and policy

The pinned WebKit release's `BubblewrapLauncher.cpp` uses mount, UTS, PID, IPC
and network isolation for the WebProcess, plus a seccomp BPF filter exported
through a memfd. The standalone supervisor also requires child-subreaper support.
The NetworkProcess is outside that WebProcess sandbox in upstream WebKit; a
successful primitive check is not a claim that every renderer process is isolated.

User namespaces need separate interpretation. A sufficiently privileged caller
may create the other namespaces without a user namespace, while an unprivileged
caller may need one. A missing `CONFIG_USER_NS` or a restrictive user-namespace
sysctl alone is not a universal compatibility verdict. Bubblewrap's [namespace
model](https://github.com/containers/bubblewrap/blob/v0.6.1/README.md) also depends
on its build, caller privileges and the requested options.

Normal startup requires WebKit's sandbox. Its unavailable-sandbox path refuses
the child; it never retries with the sandbox disabled. The user separately
authorized an explicit device opt-in on 2026-11-09. Mode selection applies to one
launcher invocation and is not saved in a profile:

| Selection | Startup protocol | WebKit internal sandbox |
| --- | --- | --- |
| No mode flag | `READY sandbox-required` | Required; unavailable startup fails |
| `--allow-unsandboxed-experiment` | `READY experiment` | Disabled for the explicit simulator experiment |
| `--allow-unsandboxed-device` | `READY device-unsandboxed` | Disabled by the explicit device opt-in |

Launch the installed standalone helper in the selected device mode with:

```sh
/mnt/onboard/.adds/kobalt-webkit/bin/kobalt-webkit --allow-unsandboxed-device
```

The launcher logs the selected mode to stderr and identifies the device opt-in
as disabling WebKit's sandbox. Conflicting mode flags are rejected before a
worker starts. The launcher clears inherited mode/override variables and sets
one owned mode together with the matching WebKit override. The worker rejects
missing, unknown or inconsistent mode/override combinations before initializing
WebKit. The normal mode cannot be changed by inherited variables.

Device mode runs with the caller's existing OS permissions, without WebKit's
filesystem, syscall and network isolation. Environment sanitation, private
libraries, process ownership, crash handling and run-directory cleanup remain.
It supplies no alternative sandbox and installs no kernel, sysctl, setuid helper
or persistent device configuration. This M1 helper still loads only its packaged
trusted fixture; it adds no EPUB loading, reader integration or book-script
policy. A separate process or disabled book JavaScript does not replace a sandbox.

For later reader integration, unavailable required-mode startup must preserve
the existing reader and report the reason, without a restart loop. Device mode
must remain an explicit choice; a probe failure or renderer crash must never
select it automatically. The diagnostic's `unavailable` summary describes the
required sandbox primitives, rather than prohibiting an explicitly selected
unsandboxed attempt. The opt-in does not supply missing ABI, child-process or
other runtime features. Hardware validation must record the selected mode.

## Running-device diagnostic

The package now includes `bin/kobalt-webkit-probe`, which links libc alone and
loads neither WebKit nor the reader profile. Run it through a device shell or an
external runner with the same UID, groups and capabilities intended for the
renderer:

```sh
/mnt/onboard/.adds/kobalt-webkit/bin/kobalt-webkit-probe --context physical-device
```

Capture stdout, stderr and the exit status, together with device model and
firmware version. Use the package's checksum verification before deployment.
The diagnostic reports the kernel release/machine and effective UID. The context
label is supplied by the caller; it is not a hardware attestation. QEMU users
must select `--context qemu-user`; those results describe host-kernel operations
through QEMU's syscall translation.

Each operation runs in a disposable child. Namespace tests first try the caller's
privileges, then try a user namespace on permission denial. The report marks a
successful user-namespace path. The seccomp test installs a small filter and
verifies a denied syscall. It reports errors and timeouts without changing the
parent's namespaces or privilege state. It writes only its output pipes and
creates no files, mounts, persistent settings or background workers.

| Exit status / summary | Meaning |
| --- | --- |
| `0` / `primitives-available` | The tested operations succeeded in this context. Actual sandbox startup remains to be verified. |
| `77` / `unavailable` | A required operation failed. Inspect the individual errno and context. |
| `2` / `probe-error`, or invalid invocation | The diagnostic could not complete reliably. No support verdict can be inferred. |

`ENOSYS` identifies an unavailable syscall; `EINVAL` can indicate an unsupported
operation or flag. `EPERM`/`EACCES` can come from credentials, a container, seccomp
or another security policy. None alone identifies a kernel configuration option.
The report always has `sandbox_verified: false`: it does not reproduce WebKit's
full filter, UID mapping, mounts or subprocess setup. Private target Bubblewrap
and D-Bus proxy packaging, actual sandbox launch and denial tests are still open.

Run the focused native regression with `ctest --preset native-supervisor -R
platform-probe` from `platforms/webkit/`. Tests inject missing-syscall and
permission failures and a hung child through a test-only preload library. For the ARM diagnostic
alone in a disposable simulator, without a WebKit sandbox exception:

```sh
python3 tests/webkit/run-firmware.py \
  --virtualnickel /home/sherman/kobo-dev/VirtualNickel --platform-probe-only
```

This reuses the existing Device API, verifies package checksums and Nickel
responsiveness, saves the JSON report and deletes the device and system volume.
It is not a physical-kernel test. A future Qt caller must launch diagnostics and
renderer processes asynchronously; these standalone tools do not belong on the
reader's main thread.

The [diagnostic result](kernel-results.json) records the current native and ARM
checks. Firmware userspace returned `EINVAL` for namespace creation and seccomp
filter installation under QEMU; this is recorded as unavailable in that context.
Nickel remained responsive and the disposable device was removed. The supervisor,
worker and private libraries were byte-identical to the preceding validated
package at that probe-only checkpoint. The subsequent [device-mode result](device-mode-results.json) records rendering,
crash/restart, mode-label and invalid-input validation with the explicit device
opt-in under firmware userspace. A physical device probe has not been run.

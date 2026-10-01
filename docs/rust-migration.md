# FLU 1.5: Rust backend and QML interface

## Architecture

FLU's desktop controller, settings persistence, package planning and removal
policy, privileged helper, persistent worker, HTTPS transport, signing-key
verification and security diagnostics are implemented in Rust.

- `rust/flu-core`: Qt-independent state, process and file I/O, bounded HTTPS
  downloads, repository-scoped key recovery, diagnostics and presentation data.
- `rust/flu-bridge`: the Rust `UpdateBackend` QObject exposed through CXX-Qt
  0.10.0. It queues background results onto the Qt UI thread.
- `rust/flu-service`: the Rust helper and worker executables, using the existing
  polkit action and systemd service.
- `src/ui/main.qml`: the existing interface. Bindings now reference the Rust
  backend; layout, controls, Close focus and touchpad momentum code are retained.
- `src/fluffupdates.*` and `src/nativeqt.*`: small native KDE plugin and Qt
  interoperation adapters for host windows, translation, clipboard, URLs and
  network information. They contain no update or signing-key decisions.
- `rust/flu-test-bridge` and `tests/support`: regression-test adapters only.
  They are not linked into the shipped plugin or installed package.

The superseded C++ controller, helper, worker and security implementation have
been removed. CMake builds the Cargo workspace and stages the normal package.
The source ZIP contains no compiled tests, fake repositories or debug switches.

## Preserved integration

The Qt minimum remains 6.9. The application remains a System Settings KCM and
retains its application launcher, native Plasma controls, translations, package
policy, helper paths, systemd service and state/log formats. The Cargo lockfile
pins dependencies; Rust 2024 and Rust 1.85 or newer are required.

The helper still previews a real Pacman transaction, accepts replacements, and
declines its final install prompt. Pacman's exit code 1 for that deliberate
decline is accepted only with the preview prompt and a complete size summary.
Actual installation remains a separate, authorized worker operation.

Key recovery remains limited to an explicitly identified FluffNet repository.
It validates the official HTTPS fingerprint and certificate, checks the exact
requested signing key and bindings, imports only validated public material,
and preserves the existing rollback rules. Arch and ambiguous repository
errors do not enter FluffNet recovery.

## Sleep protection during installation

The Rust worker acquires a native logind `sleep`/`block` inhibitor before its
first Pacman command. Its owned file descriptor remains alive throughout
preparation, downloading, installation, package hooks and recovery retries.
KDE PowerDevil resolves the `org.flufflinux.update.worker` desktop identity to
**Fluff Linux Update** and its `system-software-update` icon in the power/battery
interface. The hidden identity file does not add a launcher or change the existing
translated launcher. The short reason is "Updating". Closing the panel does not
release it, because the system-service worker owns the descriptor.

Normal completion and errors close the descriptor; cancellation and process
termination also release it through kernel descriptor cleanup. No persistent
power settings are changed. Screen blanking and locking remain available;
shutdown and low-level hardware-key handling are not inhibited. A forced
administrator override or power loss cannot be prevented by an inhibitor.

If logind inhibition cannot be acquired, the worker publishes
`SLEEP_INHIBITOR_FAILED` and does not start Pacman. The UI uses the existing
localized startup-error message and permits retry; the diagnostic log records
the D-Bus error. The D-Bus method has a five-second timeout. The Cargo lockfile
pins the Rust D-Bus implementation to an MSRV-compatible version.

References: [systemd inhibitor API](https://systemd.io/INHIBITOR_LOCKS/) and
[KDE PowerDevil's logind integration](https://github.com/KDE/powerdevil/blob/master/daemon/powerdevilpolicyagent.cpp).

Two safety checks were strengthened during the migration:

- When dependency blockers have different policy classes, protected blockers
  are handled before any automatic removal.
- File-conflict recovery checks Pacman ownership before preserving a file.
  Package-owned files and inconclusive ownership checks stop the update;
  only explicitly unowned files may be renamed without overwriting anything.

## Reference implementations

The following repository snapshots were inspected on 1 October 2026:

- [FluffInstall, `2304e22`](https://github.com/FluffNet/fluffinstall/tree/2304e22bc66c5b7b532044fa07bcb48a4ffdccb4):
  Rust 2024, CXX-Qt 0.9, a Rust `InstallerBackend` exposed to QML, background
  installation threads, and progress returned through the Qt thread queue.
- [FluffSetup, `99bb1f5`](https://github.com/FluffNet/fluffsetup/tree/99bb1f5e8a3450cf4619c0aaf78b8011b674d221):
  the same Rust/CXX-Qt stack, a Rust `SetupBackend`, embedded QML, and small
  native adapters for Qt session and window integration.
- [Fluff Linux Calculator, `1d3d704`](https://github.com/FluffNet/flufflinux-calculator/tree/1d3d704e24c69a15f296e30510d298cdd1a92a5f):
  Rust calculation and application state behind `CalculatorBackend`, QML
  controls, the KDE desktop control style, translation loading, and fakeroot
  staging around a locked Cargo release build.
- [Fluff Linux App Center, `2622f0a`](https://github.com/FluffNet/flufflinux-appcenter/tree/2622f0abaf19e1189eb2983554b7c4acf38fd0a4):
  a Rust entry point and catalog code with a manual Qt bridge, QML views, and
  substantial C++ transaction logic. Its packaging and desktop integration
  provide references, while FLU's backend target follows the CXX-Qt pattern
  used by the other three applications.

## Validation record

Validation uses a disposable Fluff Linux VM with Qt 6.11.2, KDE Frameworks 6.30,
Rust 1.98.1 and GCC 16.2.1. The original FLU 1.4-3 files and package database entry
were backed up before testing 1.5-1. The tests do not upgrade the VM's real
system packages.

The Rust core currently has 24 passing unit tests. They cover signing-key
parsing and repository isolation, desktop state transitions and notices,
settings persistence, diagnostic redaction, native process I/O and timeouts,
recovery-lock exclusion, HTTPS-only artifact fetching and atomic state writes.
A watcher regression prevents read-access notifications from feeding FLU's own
status reads back into an endless refresh loop.

The final VM run passed all four CTest suites: AppStream validation, 77
signing-key QtTest results, 10 backend-state results and 10 scrolling results
(QtTest totals include initialization and cleanup). Workspace Clippy with
warnings denied and Cargo formatting checks passed. The 24 Rust tests also
passed on both the development host and Linux VM. The additional sleep-inhibitor
failure test checks the existing translated startup error and retry state.

The Qt suites exercise real disposable GnuPG certificates and the same Rust
signing-key implementation used in production; the actual `UpdateBackend`
QObject's properties, notifications and persisted settings; and the production
QML momentum functions. AppStream metadata and workspace formatting/lint checks
were included in the final verification.

The isolated Pacman fixture covers:

1. Updating an installed package, downloading archives, installing a new
   package and applying a replacement with only `replaces` metadata.
2. Protected, warning and automatic dependency conflicts.
3. Protected, warning and automatic direct package conflicts.
4. Mixed dependency blockers, with protected packages checked first.
5. Preserving an unowned file and completing the automatic retry.
6. Refusing to rename a file owned by an installed package.
7. Cancelling and resuming downloads, preventing cancellation during installation,
   preserving live locks and clearing an ownerless stale lock.
8. HTTP download failure without changing installed package versions.
9. Invalid user/database paths and invalid package policy.
10. The existing initial-update timestamp format.
11. The complete desktop-controller path: `checkupdates`, transaction preview,
    replacement presentation, worker startup, installation and success state.
12. Native logind sleep inhibition without a GUI, release after forced worker
    termination and successful retry, and fail-closed startup when the system
    bus is unavailable. Cancellation and every terminal worker scenario check
    that no FLU inhibitor remains.

All 17 named scenarios passed on the sleep-protection build; the three policy classifications are separate
scenarios for both dependency and direct package conflicts. The fixture creates
a private mount/PID namespace and a repository of disposable packages. Its
temporary service and authorization launchers exist only inside that namespace.
All package operations use the compiled production helper and worker.

The final package was installed and launched through the normal Plasma session.
Its installed plugin, helper and worker hashes match the tested build. Native
polkit authentication and update checking were exercised. The update list
starts with Close highlighted, Tab transfers focus to the eye button, and
closing/reopening restores the initial Close focus. After the watcher fix,
the idle app sampled at 0.0% CPU instead of continuously rereading its state.

Native desktop screenshots are stored under `docs/screenshots/`. Physical
touchpad feel still requires the user's hardware; the existing QML scrolling
implementation is preserved and its momentum regressions are run.

The native KDE Power Management widget was also checked while an isolated
update continued with FLU's window closed. Under the regular user's session it
displayed the updates icon and "Fluff Linux Update is blocking sleep. (Updating)". See
[`1.5-sleep-inhibitor.png`](screenshots/1.5-sleep-inhibitor.png); the VM's native
battery/power widget was opened standalone for this capture. This is KDE's native
formatting, not a custom FLU label. Its inhibition
entry disappeared again after completion.

## Reproducing validation

See the commands in the README. Build with `BUILD_TESTING=ON` for the Qt tests;
run Rust tests and workspace Clippy separately. The Pacman workflow runner must
be run as root on a disposable Linux development system. Pass its optional
controller example to include the complete desktop-controller workflow.

Every delivered ZIP must match its stated commit and include an updated
`.PKGINFO` build date. Keep this PR as a draft until the user accepts the native
1.5 candidate.

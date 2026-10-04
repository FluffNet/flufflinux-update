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
  backend; layout, controls and Close focus are retained. Kirigami handles wheel
  and touchpad scrolling; native Flickable touchscreen gestures remain enabled.
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

## Low system-power warning

The Rust desktop controller reads UPower's documented `DisplayDevice` over the
system bus instead of scanning every sysfs device whose type is `Battery`.
That old test included wireless mice and other accessories. UPower's composite
contains system batteries or a UPS, excludes peripheral batteries, and handles
multiple batteries without relying on chassis type, a lid, `BAT0` names, or
device paths. This also supports USB UPS devices managed in userspace.

The warning remains advisory. Its charger wording is translated in all 27
catalogs and applies both before and during installation.
It appears for a present Battery/UPS composite at 20% or less while discharging
(or empty), after a successful check with updates available and throughout
preparation, downloading and installation. Eligibility is recalculated before
each published snapshot, so checking again, completion and failure immediately
clear any previous warning, even when a failed update retains its retry list.
A retry can show the warning again; cancelling a download returns to the ready
state. Charging, healthy, absent and unknown/invalid readings do not produce a
low-battery warning. The composite's
`PowerSupply` property is not required: unlike that property on physical devices,
it is not guaranteed by the display-device contract.

UPower is now an explicit runtime dependency. Reads run on the background
controller thread, are limited to one snapshot every five seconds, and have a
750 ms method timeout. A failed read clears the stale reading and retries; FLU
does not reinterpret accessory batteries as a fallback or modify power settings.

References: [UPower display-device contract](https://upower.freedesktop.org/docs/UPower.html#UPower.GetDisplayDevice)
and [device type/power-supply definitions](https://upower.freedesktop.org/docs/Device.html).

## Mouse, touchpad and touchscreen scrolling

Both application-owned scrollable views in `main.qml` (the comparison ListView
and Pacman Flickable) use `Kirigami.WheelHandler`, with an explicit target,
`blockTargetWheel: true` and `scrollFlickableTarget: true`. The former custom
QtQuick wheel handlers, velocity blending and momentum timers have been removed.
Kirigami uses KDE's scrolling settings, including smooth-scroll preferences and
wheel step sizes, and handles the attached scrollbars. The main `SimpleKCM`
already uses Kirigami's ScrollablePage and KDE's ScrollView wheel handler; no
second handler is added to that framework-owned container. There are no GridViews.

`filterMouseEvents: false` and the views' existing interactive/flick properties
preserve touchscreen flicks and left-button dragging. App Center's middle-mouse
component remains unchanged. Starting middle autoscroll cancels native flicking
and briefly detaches/restores the wheel target to stop Kirigami's separate
animations. The next wheel event stops middle autoscroll and proceeds normally;
the signal handler does not accept or rewrite that event.

This is also recorded in the repository's `AGENTS.md` as the QML convention for
future changes. Reference: [Kirigami WheelHandler](https://api.kde.org/qml-org-kde-kirigami-wheelhandler.html).

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

The original Rust core tests cover signing-key
parsing and repository isolation, desktop state transitions and notices,
settings persistence, diagnostic redaction, native process I/O and timeouts,
recovery-lock exclusion, HTTPS-only artifact fetching and atomic state writes.
A watcher regression prevents read-access notifications from feeding FLU's own
status reads back into an endless refresh loop.

The Kirigami-scrolling VM run passed all five CTest suites: AppStream validation,
77 signing-key QtTest results, 10 backend-state results and
29 updates-window keyboard/pointer/touch results (QtTest totals include initialization and
cleanup), and compiled battery-warning
lookups in all 27 translation catalogs. Workspace Clippy with
warnings denied passed during the backend migration; Cargo formatting checks
and all 34 Linux Rust core tests passed again on the Kirigami build. The original 24 Rust tests also
passed on both the development host and Linux VM. The additional sleep-inhibitor
failure test checks the existing translated startup error and retry state.

The system-power fix adds six battery-policy tests and a Linux-only D-Bus wire
test, plus three controller lifecycle tests (34 Rust tests on Linux, 33 on the
development host). The private-bus test
serves a generic 19% mouse battery alongside a changing display device and checks
accessory-only desktops, low/healthy system batteries, low/charging UPSes, the
20% boundary, charging transitions, multi-battery composites, removal, empty
supplies, and service disappearance/recovery. Invalid/missing readings and a
display device without a usable `PowerSupply` field are covered as well. No
mock devices or test switches are installed in FLU or the system UPower service.

The Qt suites exercise real disposable GnuPG certificates and the same Rust
signing-key implementation used in production; the actual `UpdateBackend`
QObject's properties, notifications and persisted settings; and the production
QML updates window. The superseded custom-momentum unit suite was replaced by
real input-event tests against Kirigami. AppStream metadata and workspace formatting/lint checks
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

All 17 named scenarios passed again on the system-power-warning build; the three policy classifications are separate
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

Escape closes the updates list using its normal close handler, preserving
window settings without affecting the main window or the update operation.
The keyboard regression loads the production QML window and sends real Qt key
events from Close, the eye button after Tab, and both list views, including
reopening and Escape in the main window. It passes offscreen and in the VM's
normal-user KDE session through XWayland. Key handling uses Qt's standard
[`Keys.onEscapePressed`](https://doc.qt.io/qt-6/qml-qtquick-keys.html)
on the list's shared content parent.

Both list views reuse App Center's MIT-licensed `MiddleMouseScroll.qml`
component, preserving its anchor indicator, 12-pixel dead zone, speed curve
and 1800-pixel/second cap. Middle-click latches scrolling; holding the middle
button and moving scrolls until release. Clicking again, Escape, the wheel,
leaving/hiding the view, or losing window activation stops it. A stopping click
is consumed instead of activating underlying content. Escape stops autoscroll
first; when not autoscrolling, Escape closes the list as before. The comparison
view supports both axes, while the Pacman view scrolls vertically.

Tests exercise both production list views with real
Qt mouse/key/touch events, including boundaries, dead zone, held scrolling, toggling,
Escape, wheel handoff, view/window exit, reopening, click consumption, horizontal
scrolling, and no-overflow lists. The reusable component is bundled in the KCM
resources; no fixture data or test code is installed.
The wheel tests additionally check the requested Kirigami properties, single
wheel-step movement without duplicate stock handling, wheel input over scrollbars,
fine-angle phased touchpad gestures, the pixel-only fallback, repeated gestures
without jump-back, and native touchscreen dragging/flicking. Pixel-only fallback
events use no scroll phase: Qt filters angle-less ScrollUpdate events following
accepted events as duplicate compatibility events before they reach the handler.
Each test row uses fresh windows to isolate native transient-window focus
events; reopening behavior is still exercised within the relevant test rows.
All 29 results pass offscreen at 100% and 150% scaling and in normal-user KDE
XWayland. Native Wayland passes 28 results, including every wheel, touchscreen
and middle-mouse case. The pre-existing inactive-parent Escape test cannot
programmatically activate its host window under the current compositor policy;
it fails at the focus precondition, before sending Escape. That native focus
limitation is not counted as a passing check.

Native desktop screenshots are stored under `docs/screenshots/`. Physical
touchpad feel still requires the user's hardware; synthetic regression tests
verify event routing and movement, not subjective device feel.

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

# Translating the Fluff Linux System Updates panel

Thank you for helping translate Fluff Linux. You do not need to understand C++
or QML to add a language.

Included catalogs cover every official European Union language together with
Arabic, Hebrew, Japanese, and Russian. English is the source language.

## Add your language

1. Find the short language code used by KDE, such as `de` for German, `es` for
   Spanish, or `ja` for Japanese.
2. Create a folder named `po/<language-code>/`.
3. Copy `po/kcm_fluffupdates.pot` to:
   `po/<language-code>/kcm_fluffupdates.po`.
4. In the copied file, set the `Language:` line to your language code.
5. For every English `msgid`, write the translation in the following `msgstr`.
6. Keep `%1` unchanged. KDE replaces it with the recorded update date.
7. Save the file as UTF-8 and compile the project normally. The translation is
   automatically included when the project is staged into the fakeroot.

Example:

```po
#. Second half of the runtime window title.
msgid "Fluff Linux Update"
msgstr "Your translation here"
```

The comment beginning with `#.` explains where the text appears or what it
means. The English `msgid` must never be changed. Only edit `msgstr`.

## English strings and context

| English text | Where it appears / what it means |
|---|---|
| System Updates | First half of the runtime window title. |
| Fluff Linux Update | Second half of the runtime window title. |
| Keep Fluff Linux secure and up to date by installing system updates. | Short introduction at the top of the page. |
| System last updated: %1 (%2) | Date line; `%1` is the exact date and `%2` is its relative age. |
| Check for Updates | Manual check button inside the status card. |
| Install Updates | Starts the privileged manual update after a successful check. |
| View Updates | Button beside Install Updates that opens the separate package and version-change window. |
| Change view | Accessible name and tooltip for the eye-icon button that switches between the comparison list and compact pacman-style view. |
| Updates to Install | Title of the resizable package and version-change window. Only the title and Close button follow the interface direction; package/version rows remain left-to-right. |
| Package details are unavailable. | Fallback inside the update-list window if package/version rows could not be parsed. |
| Fluff Linux Update is still under development. If you encounter an issue, please report it on our GitHub page. | Development and issue-reporting information banner. |
| System updates were installed successfully. | Temporary green message shown for seven seconds after installation completes. |
| GitHub | Opens the Fluff Linux Update issue tracker in the default browser. Keep the GitHub brand name untranslated. |
| No network connection! | Warning when the system reports no connected network link. |
| Network connection is limited. If you are not connected to an organizational network, please check your internet connection. | Informational note for local/intranet connectivity; controls remain enabled. |
| pacman process is already running. | Shown when checking or installation cannot start because another pacman process exists; keep `pacman` unchanged. |
| Downloading updates… | Heading during the cancellable download phase. |
| %1 / %2 downloaded | Downloaded data and total download size; preserve `%1` and `%2`. |
| Installing updates… | Heading during the non-cancellable installation phase. |
| %1/%2 updates installed | Installed-update counter; preserve `%1` and `%2`. |
| Updates are currently being installed. Please avoid powering off or restarting the computer until installation is complete. Interrupting the update may damage system files. | Safety warning shown only during package installation. |
| The battery is low. Please plug in your computer before installing updates. | Non-blocking warning shown after updates are found when a laptop is discharging at twenty percent battery or less. |
| Update process was cancelled | Five-second notice after cancelling a download. |
| Cancel | Button available only while packages are downloading. |
| The update process could not be started. | Background worker launch failure. |
| The update process could not be cancelled. | Download cancellation failure. |
| Connection failed while downloading updates. Check your network and try again. | Download-phase connection failure. |
| The system update failed. | Installation-phase pacman failure. |
| Checking for updates… | Temporary status while repositories are checked. |
| System updates are available | Result when pending system upgrades exist. |
| Download size: %1 | Total network download; preserve `%1`. |
| Storage required for system updates: %1 | Positive net storage change reported by pacman; preserve `%1`. |
| Storage freed after update: %1 | Net storage recovered after updating; preserve `%1`. |
| No additional download required | Required packages are already cached. |
| The update checker could not be started. Make sure pacman-contrib is installed. | Missing helper error. |
| The update check stopped unexpectedly. | Checker process ended abnormally. |
| An unknown error has occurred. Please report this issue on our GitHub page for assistance. | Generic red error below the Check for Updates button when the failure cannot be identified. |
| No internet connection. Check your network and try again. | Red error when DNS or network connectivity is unavailable. |
| The repository servers could not be reached. Try again later or check your mirror configuration. | Red error when mirrors or repository databases cannot be reached. |
| Authorization error, please try again. | Polkit authorization was cancelled, denied, or otherwise failed. |
| The privileged update check stopped unexpectedly. | Privileged transaction calculation ended abnormally. |
| The privileged update check could not be started. | Polkit or privileged pacman process could not be launched. |
| System was not previously updated. | Shown when the JSON file or date is absent. |
| The last update information could not be read. | Shown when the state file cannot be opened. |
| The last update information is invalid. | Shown when the state file is not valid JSON. |
| Updates were not installed on this system | Blue status when no successful update date exists. |
| To update the system, check for available updates using the button below. | Guidance below the blue status and above the update-check button. |
| Update date available | Blue fallback when the stored date cannot be interpreted. |
| Updated recently | Green status for an update within the last month. It does not claim that no updates are available. |
| System is up to date | Temporary green status only after a successful check finds no available updates. Cleared when leaving the page. |
| Last update was installed more than a month ago | Yellow status for an update older than one month. |
| Last update was installed more than three months ago | Orange status for an update older than three months. |
| %1 second/minute/hour/day ago | Relative age beside the exact date. Preserve `%1`; plural forms follow the language’s rules. |
| Action is required | Temporary red status while a warning package removal requires a decision. |
| Action is required! | Temporary red status when a protected package blocks the update. |
| Action required | Title of the warning and protected package dialogs. |
| Fluff Linux Update cannot continue because the upcoming update requires removing “%1”, which is a protected system package. Removing it could prevent Fluff Linux from working correctly. Please report this issue on GitHub for assistance. | Protected package dialog. Preserve `%1`. |
| To allow the system to install updates, Fluff Linux Update needs to remove “%1”. Removing this package may affect related software. If you are not sure, please report the issue on GitHub for help. | Warning package decision dialog. Preserve `%1`. |
| Accept | Approves removal of a warning package. |
| Close | Closes a protected package dialog without changing the system. |
| To allow system updates to continue, %1 was automatically removed after it was deemed safe to remove. | Ten-second notice for one safe automatic removal. Preserve `%1`. |
| To allow system updates to continue, the following packages were automatically removed after they were deemed safe to remove: %1 | Ten-second notice for multiple safe automatic removals. Preserve `%1`. |
| A file conflict was detected and resolved. %1 was renamed to %2. The update process has restarted. | Ten-second file conflict recovery notice. Preserve `%1` and `%2`. |
| Fluff Linux Update could not verify the FluffNet repository signing key. The update was stopped to protect your system. | Red status shown when verified FluffNet key recovery fails. |
| Repository: %1 | Repository field in signing-key technical details. Preserve `%1`; the inserted repository name remains left-to-right. |
| FLU version: %1 | Installed FLU version in signing-key technical details. Preserve `%1`. |
| Fluff Linux version: %1 | Operating-system version in signing-key technical details. Preserve `%1`. |
| The official FluffNet repository signing key was verified and added to Pacman. | Temporary notice after the verified certificate is imported successfully. |

The panel name and description shown in the System Settings search results also
have translations in `src/kcm_fluffupdates.json`. The application-menu entry is
translated in `src/org.flufflinux.update.desktop`. When adding a production
translation, add matching locale entries to both metadata files, where `xx` is
the same language code used by the PO catalog.

## Right-to-left languages

No special layout code is required in a translation. KDE and Qt automatically
select right-to-left direction for languages such as Arabic and Hebrew, and the
panel mirrors its rows and controls accordingly. The package/version list
intentionally remains left-to-right because package names, versions, and the
old-to-new arrow use technical notation.

## Updating an existing translation

Compare your `.po` file with `kcm_fluffupdates.pot`. Add any missing `msgid`
blocks and translate their empty `msgstr`. Obsolete strings may be removed.
Never translate technical names such as `Fluff Linux` or `pacman` unless the
language community has an established written form for them.

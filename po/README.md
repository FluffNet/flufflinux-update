# Translating the Fluff Linux System Updates panel

Thank you for helping translate Fluff Linux. You do not need to understand C++
or QML to add a language.

Included catalogs currently cover Arabic, French, German, Hebrew, Japanese,
Russian, Spanish, and Swedish.

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
| View Updates | Button beside Install Updates that opens the package and version-change list. |
| Updates to Install | Title of the package and version-change dialog. Only the title and Close button follow the interface direction; package/version rows remain left-to-right. |
| Package details are unavailable. | Fallback inside the dialog if package/version rows could not be parsed. |
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
| The update download failed. Check your connection and try again. | Download-phase failure. |
| The system update failed. | Installation-phase pacman failure. |
| Checking for updates… | Temporary status while repositories are checked. |
| System updates are available | Result when pending system upgrades exist. |
| Download size: %1 | Total network download; preserve `%1`. |
| Storage required for system updates: %1 | Positive net storage change reported by pacman; preserve `%1`. |
| Storage freed after update: %1 | Net storage recovered after updating; preserve `%1`. |
| No additional download required | Required packages are already cached. |
| The update checker could not be started. Make sure pacman-contrib is installed. | Missing helper error. |
| The update check stopped unexpectedly. | Checker process ended abnormally. |
| The update check failed. | Generic checking error. |
| No internet connection. Check your network and try again. | Red error when DNS or network connectivity is unavailable. |
| The repository servers could not be reached. Try again later or check your mirror configuration. | Red error when mirrors or repository databases cannot be reached. |
| Update sizes could not be calculated. | Pacman found updates, but its transaction-size summary could not be read. |
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

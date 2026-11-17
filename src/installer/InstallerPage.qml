import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The host supplies decoded protocol events. This page never starts a process
// or authorizes an operation. Completion belongs to the host's process watcher.
Controls.Pane {
    id: root
    property var event: ({})
    readonly property var state: event.state || ({})
    readonly property var display: event.display || ({})
    implicitWidth: 480
    implicitHeight: contentItem.implicitHeight + topPadding + bottomPadding

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.largeSpacing
        Kirigami.Heading {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.state.phase === "complete"
                ? i18nd("kcm_fluffupdates", "System is up to date")
                : root.state.phase === "downloading"
                    ? i18nd("kcm_fluffupdates", "Downloading updates…")
                    : root.state.phase === "installing"
                        ? i18nd("kcm_fluffupdates", "Installing updates…")
                        : i18nd("kcm_fluffupdates", "Install Updates")
        }
        Controls.ProgressBar {
            Layout.fillWidth: true
            from: 0
            to: 100
            value: root.state.progress || 0
            indeterminate: !root.state.phase || root.state.phase === "starting"
        }
        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: root.state.phase === "downloading"
            text: i18nd("kcm_fluffupdates", "%1 / %2 downloaded",
                "\u2066" + (root.display.downloadedSize || "0 B") + "\u2069",
                "\u2066" + (root.display.totalDownloadSize || "0 B") + "\u2069")
                + "  " + "\u2066" + (root.state.speed || "") + "\u2069"
        }
        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: root.state.phase === "downloading" && !!root.display.downloadTimeRemaining
            text: i18nd("kcm_fluffupdates", "Estimated time: %1",
                "\u2066" + (root.display.downloadTimeRemaining || "") + "\u2069")
        }
        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: root.state.phase === "installing"
            text: i18nd("kcm_fluffupdates", "%1/%2 updates installed",
                root.state.completed_packages || 0, root.state.total_packages || 0)
        }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: root.state.phase === "failed"
            type: Kirigami.MessageType.Error
            text: i18nd("kcm_fluffupdates", root.display.errorMessage || "The system update failed.")
        }
    }
}

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import QtQuick.Window

import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCMUtils

KCMUtils.SimpleKCM {
    id: root

    readonly property int minimumUsableWidth: 480
    readonly property int minimumUsableHeight: 400

    // Qt selects RightToLeft for Arabic and Hebrew. Explicit mirroring makes
    // every nested row follow that direction as well.
    LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    onVisibleChanged: {
        if (!visible) {
            updatesWindow.close()
            kcm.clearCheckResult()
        } else {
            Qt.callLater(enforceWindowMinimumSize)
        }
    }

    function enforceWindowMinimumSize() {
        const hostWindow = root.Window.window
        if (hostWindow) {
            hostWindow.minimumWidth = Math.max(
                hostWindow.minimumWidth, root.minimumUsableWidth)
            hostWindow.minimumHeight = Math.max(
                hostWindow.minimumHeight, root.minimumUsableHeight)
        }
    }

    Component.onCompleted: Qt.callLater(enforceWindowMinimumSize)

    Layout.minimumWidth: minimumUsableWidth
    Layout.minimumHeight: minimumUsableHeight
    implicitWidth: Kirigami.Units.gridUnit * 36
    implicitHeight: Kirigami.Units.gridUnit * 28

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: i18nd("kcm_fluffupdates", "Keep Fluff Linux secure and up to date by installing system updates.")
            color: Kirigami.Theme.disabledTextColor
        }

        Kirigami.Card {
            Layout.fillWidth: true

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.largeSpacing

                    Rectangle {
                        implicitWidth: Kirigami.Units.iconSizes.medium
                        implicitHeight: width
                        radius: width / 2
                        color: kcm.freshnessColor

                        Accessible.name: kcm.freshnessText
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: Kirigami.Units.gridUnit * 12
                        spacing: 0

                        Controls.Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: kcm.checkComplete && !kcm.updatesAvailable && kcm.checkError.length === 0
                                ? i18nd("kcm_fluffupdates", "System is up to date")
                                : kcm.freshnessText
                            color: kcm.freshnessColor
                            font.bold: true
                        }

                        Controls.Label {
                            Layout.fillWidth: true
                            visible: kcm.hasLastUpdate
                            wrapMode: Text.WordWrap
                            text: i18nd("kcm_fluffupdates", "System last updated: %1 (%2)",
                                "\u2066" + kcm.lastUpdate + "\u2069",
                                "\u2066" + kcm.relativeTime + "\u2069")
                        }

                        Controls.Label {
                            Layout.fillWidth: true
                            visible: !kcm.hasLastUpdate
                            wrapMode: Text.WordWrap
                            text: i18nd("kcm_fluffupdates", "To update the system, check for available updates using the button below.")
                        }

                        Controls.Button {
                            Layout.topMargin: Kirigami.Units.smallSpacing * 2
                            Layout.alignment: Qt.AlignLeading
                            text: i18nd("kcm_fluffupdates", "Check for Updates")
                            icon.name: "view-refresh"
                            enabled: kcm.networkConnected && !kcm.checking
                                && !kcm.updateActive
                            onClicked: kcm.checkForUpdates()
                        }
                    }
                }

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: kcm.checking
                    type: Kirigami.MessageType.Information
                    text: i18nd("kcm_fluffupdates", "Checking for updates…")
                }

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: kcm.checkComplete && !kcm.checking && kcm.checkError.length > 0
                    type: Kirigami.MessageType.Error
                    text: kcm.checkError
                }
            }
        }

        Kirigami.Card {
            Layout.fillWidth: true
            visible: (kcm.checkComplete && !kcm.checking
                && kcm.checkError.length === 0 && kcm.updatesAvailable)
                || kcm.updateActive || kcm.installPhase === "failed"

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Heading {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    level: 3
                    text: kcm.installPhase === "downloading"
                        ? i18nd("kcm_fluffupdates", "Downloading updates…")
                        : kcm.installPhase === "installing"
                            ? i18nd("kcm_fluffupdates", "Installing updates…")
                            : i18nd("kcm_fluffupdates", "Install Updates")
                }

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: !kcm.updateActive
                    text: i18nd("kcm_fluffupdates", "System updates are available")
                    font.bold: true
                }

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: !kcm.updateActive
                    text: i18nd("kcm_fluffupdates", "Download size: %1",
                        "\u2066" + kcm.downloadSize + "\u2069")
                }

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: !kcm.updateActive && kcm.diskChange.length > 0
                    text: kcm.diskSpaceFreed
                        ? i18nd("kcm_fluffupdates", "Storage freed after update: %1",
                            "\u2066" + kcm.diskChange + "\u2069")
                        : i18nd("kcm_fluffupdates", "Storage required for system updates: %1",
                            "\u2066" + kcm.diskChange + "\u2069")
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.smallSpacing * 2
                    spacing: Kirigami.Units.smallSpacing
                    visible: kcm.updateActive

                    Item {
                        id: updateProgressBar

                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 1.5
                        implicitWidth: Kirigami.Units.gridUnit * 12
                        implicitHeight: Kirigami.Units.gridUnit * 1.5

                        readonly property real position: Math.max(0,
                            Math.min(1, kcm.installProgress / 100))
                        readonly property bool mirrored:
                            Qt.application.layoutDirection === Qt.RightToLeft

                        // The contrasting outline remains visible in both
                        // light and dark themes as the window is resized.
                        Rectangle {
                            anchors.fill: parent
                            radius: 2

                            gradient: Gradient {
                                GradientStop {
                                    position: 0
                                    color: Qt.darker(Kirigami.Theme.backgroundColor, 1.12)
                                }
                                GradientStop {
                                    position: 0.35
                                    color: Kirigami.Theme.backgroundColor
                                }
                                GradientStop {
                                    position: 1
                                    color: Qt.lighter(Kirigami.Theme.backgroundColor, 1.12)
                                }
                            }
                        }

                        Rectangle {
                            width: Math.max(0, (parent.width - 4)
                                * updateProgressBar.position)
                            height: Math.max(0, parent.height - 4)
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: updateProgressBar.mirrored
                                ? undefined : parent.left
                            anchors.right: updateProgressBar.mirrored
                                ? parent.right : undefined
                            anchors.leftMargin: updateProgressBar.mirrored ? 0 : 2
                            anchors.rightMargin: updateProgressBar.mirrored ? 2 : 0
                            radius: 1
                            clip: true

                            gradient: Gradient {
                                GradientStop {
                                    position: 0
                                    color: Qt.lighter(
                                        Kirigami.Theme.highlightColor, 1.35)
                                }
                                GradientStop {
                                    position: 0.45
                                    color: Kirigami.Theme.highlightColor
                                }
                                GradientStop {
                                    position: 1
                                    color: Qt.darker(
                                        Kirigami.Theme.highlightColor, 1.18)
                                }
                            }

                            Behavior on width {
                                NumberAnimation {
                                    duration: Kirigami.Units.shortDuration
                                    easing.type: Easing.OutCubic
                                }
                            }
                        }

                        // Draw the outline last so neither the animated fill
                        // nor theme rendering can cover or resize it.
                        Rectangle {
                            anchors.fill: parent
                            z: 2
                            radius: 2
                            color: "transparent"
                            border.width: 1
                            border.color: Qt.rgba(
                                Kirigami.Theme.textColor.r,
                                Kirigami.Theme.textColor.g,
                                Kirigami.Theme.textColor.b,
                                0.45)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: kcm.installPhase === "downloading"

                        Controls.Label {
                            text: i18nd("kcm_fluffupdates", "%1 / %2 downloaded",
                                "\u2066" + kcm.downloadedSize + "\u2069",
                                "\u2066" + kcm.totalDownloadSize + "\u2069")
                        }

                        Item { Layout.fillWidth: true }

                        Controls.Label {
                            text: "\u2066" + kcm.downloadSpeed + "\u2069"
                        }
                    }

                    Controls.Label {
                        Layout.fillWidth: true
                        visible: kcm.installPhase === "installing"
                        wrapMode: Text.WordWrap
                        text: i18nd("kcm_fluffupdates", "%1/%2 updates installed",
                            "\u2066" + kcm.completedPackages + "\u2069",
                            "\u2066" + kcm.totalPackages + "\u2069")
                    }

                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        visible: kcm.installPhase === "installing"
                        type: Kirigami.MessageType.Warning
                        text: i18nd("kcm_fluffupdates", "Updates are currently being installed. Please avoid powering off or restarting the computer until installation is complete. Interrupting the update may damage system files.")
                    }
                }

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: kcm.cancellationNotice
                    type: Kirigami.MessageType.Information
                    text: i18nd("kcm_fluffupdates", "Update process was cancelled")
                }

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: kcm.installError.length > 0
                    type: Kirigami.MessageType.Error
                    text: kcm.installError
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: !kcm.updateActive

                    Controls.Button {
                        text: i18nd("kcm_fluffupdates", "Install Updates")
                        icon.name: "system-software-update"
                        enabled: kcm.networkConnected
                        onClicked: kcm.startInstallation()
                    }

                    Controls.Button {
                        visible: !kcm.updateActive
                            && kcm.installPhase !== "downloading"
                            && kcm.installPhase !== "installing"
                            && (kcm.updatesAvailable
                                || kcm.updatePackages.length > 0)
                        text: i18nd("kcm_fluffupdates", "View Updates")
                        icon.name: "dialog-information"
                        onClicked: updatesWindow.present()
                    }

                    Item { Layout.fillWidth: true }
                }

                Controls.Button {
                    visible: kcm.installPhase === "downloading"
                    text: i18nd("kcm_fluffupdates", "Cancel")
                    icon.name: "dialog-cancel"
                    onClicked: kcm.cancelInstallation()
                }
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: !kcm.networkConnected
            type: Kirigami.MessageType.Warning
            text: i18nd("kcm_fluffupdates", "No network connection!")
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: kcm.networkConnected && kcm.networkLimited
            type: Kirigami.MessageType.Information
            text: i18nd("kcm_fluffupdates", "Network connection is limited. If you are not connected to an organizational network, please check your internet connection.")
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: kcm.installationSuccessNotice
            type: Kirigami.MessageType.Positive
            text: i18nd("kcm_fluffupdates", "System updates were installed successfully.")
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: kcm.batteryLow
            type: Kirigami.MessageType.Warning
            icon.name: "battery-low"
            text: i18nd("kcm_fluffupdates", "The battery is low. Please plug in your computer before installing updates.")
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: developmentMessageRow.implicitHeight
                + Kirigami.Units.smallSpacing * 2
            radius: Kirigami.Units.cornerRadius
            color: Qt.rgba(0.24, 0.68, 0.91, 0.15)
            border.width: 1
            border.color: "#3daee9"

            RowLayout {
                id: developmentMessageRow

                anchors.fill: parent
                anchors.margins: Kirigami.Units.smallSpacing
                spacing: Kirigami.Units.smallSpacing

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: Kirigami.Units.smallSpacing

                    Controls.Label {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        wrapMode: Text.WordWrap
                        text: i18nd("kcm_fluffupdates", "Fluff Linux Update is still under development. If you encounter an issue, please report it on our GitHub page.")
                    }

                    Controls.Button {
                        Layout.alignment: Qt.AlignLeading
                        text: i18nd("kcm_fluffupdates", "GitHub")
                        icon.name: "internet-services"
                        onClicked: Qt.openUrlExternally("https://github.com/FluffNet/flufflinux-update/issues")
                    }
                }
            }
        }

        Item { Layout.fillHeight: true }
    }

    Window {
        id: updatesWindow

        function present() {
            show()
            raise()
            requestActivate()
        }

        function widestField(fieldName) {
            let widest = 0
            for (let index = 0; index < kcm.updatePackages.length; ++index) {
                const entry = kcm.updatePackages[index]
                widest = Math.max(widest,
                    updateListFontMetrics.advanceWidth(
                        String(entry[fieldName] || "")))
            }
            return widest
        }

        readonly property real preferredPackageWidth: Math.max(
            Kirigami.Units.gridUnit * 8,
            widestField("name") + Kirigami.Units.smallSpacing * 2)
        readonly property real preferredVersionsWidth: Math.max(
            Kirigami.Units.gridUnit * 12,
            widestField("currentVersion")
                + updateListFontMetrics.advanceWidth("  →  ")
                + widestField("newVersion")
                + Kirigami.Units.smallSpacing * 2)
        readonly property real preferredWindowWidth:
            preferredPackageWidth + preferredVersionsWidth
                + Kirigami.Units.largeSpacing * 5

        visible: false
        flags: Qt.Window
        modality: Qt.NonModal
        transientParent: root.Window.window
        minimumWidth: 480
        minimumHeight: 400
        width: Math.min(Screen.desktopAvailableWidth
                - Kirigami.Units.largeSpacing * 2,
            Math.max(minimumWidth, preferredWindowWidth))
        height: Math.min(Screen.desktopAvailableHeight
                - Kirigami.Units.largeSpacing * 2,
            Math.max(minimumHeight, Kirigami.Units.gridUnit * 26))
        title: i18nd("kcm_fluffupdates", "Updates to Install")
        color: Kirigami.Theme.backgroundColor

        onVisibleChanged: {
            if (visible) {
                updateList.positionViewAtBeginning()
                pacmanViewFlickable.contentY = 0
            }
        }

        FontMetrics {
            id: updateListFontMetrics
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.smallSpacing

                Controls.ToolButton {
                    checkable: true
                    checked: kcm.pacmanView
                    display: Controls.AbstractButton.IconOnly
                    icon.name: "view-visible"
                    Accessible.name: i18nd("kcm_fluffupdates", "Change view")
                    Controls.ToolTip.text: Accessible.name
                    Controls.ToolTip.visible: hovered
                    onToggled: kcm.setPacmanView(checked)
                }

                Item { Layout.fillWidth: true }
            }

            Controls.ScrollView {
                id: updatesScrollView

                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: !kcm.pacmanView
                clip: true
                LayoutMirroring.enabled: false
                LayoutMirroring.childrenInherit: true

                ListView {
                    id: updateList

                    width: updatesScrollView.availableWidth
                    height: updatesScrollView.availableHeight
                    layoutDirection: Qt.LeftToRight
                    model: kcm.updatePackages
                    spacing: Kirigami.Units.smallSpacing
                    boundsBehavior: Flickable.StopAtBounds

                    Controls.Label {
                        anchors.centerIn: parent
                        width: parent.width
                            - Kirigami.Units.largeSpacing * 2
                        visible: updateList.count === 0
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        text: i18nd("kcm_fluffupdates", "Package details are unavailable.")
                    }

                    delegate: Item {
                        required property var modelData

                        width: ListView.view.width
                        height: packageRow.implicitHeight
                            + Kirigami.Units.smallSpacing * 2

                        RowLayout {
                            id: packageRow

                            anchors.fill: parent
                            anchors.leftMargin: Kirigami.Units.smallSpacing
                            anchors.rightMargin: Kirigami.Units.smallSpacing
                            spacing: Kirigami.Units.largeSpacing
                            layoutDirection: Qt.LeftToRight

                            Controls.Label {
                                Layout.preferredWidth: Math.min(
                                    updatesWindow.preferredPackageWidth,
                                    updateList.width * 0.42)
                                Layout.minimumWidth: 0
                                wrapMode: Text.WrapAnywhere
                                color: Kirigami.Theme.textColor
                                text: "\u2066" + modelData.name + "\u2069"
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                Layout.preferredWidth:
                                    updatesWindow.preferredVersionsWidth
                                spacing: Kirigami.Units.smallSpacing
                                layoutDirection: Qt.LeftToRight

                                Controls.Label {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    wrapMode: Text.WrapAnywhere
                                    horizontalAlignment: Text.AlignRight
                                    color: "#ff3b30"
                                    text: modelData.currentVersion
                                }

                                Controls.Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    color: Kirigami.Theme.textColor
                                    text: "→"
                                }

                                Controls.Label {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    wrapMode: Text.WrapAnywhere
                                    horizontalAlignment: Text.AlignLeft
                                    color: "#00b84a"
                                    text: modelData.newVersion
                                }
                            }
                        }
                    }
                }
            }

            Controls.ScrollView {
                id: pacmanViewScroll

                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: kcm.pacmanView
                clip: true
                LayoutMirroring.enabled: false
                LayoutMirroring.childrenInherit: true

                Flickable {
                    id: pacmanViewFlickable

                    width: pacmanViewScroll.availableWidth
                    height: pacmanViewScroll.availableHeight
                    contentWidth: width
                    contentHeight: pacmanPackageFlow.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds

                    Flow {
                        id: pacmanPackageFlow

                        width: parent.width
                        spacing: Kirigami.Units.smallSpacing
                        layoutDirection: Qt.LeftToRight

                        Repeater {
                            model: kcm.updatePackages

                            delegate: Row {
                                required property var modelData

                                layoutDirection: Qt.LeftToRight

                                Controls.Label {
                                    color: Kirigami.Theme.textColor
                                    text: "\u2066" + modelData.name + "-\u2069"
                                }

                                Controls.Label {
                                    color: "#00b84a"
                                    text: "\u2066" + modelData.newVersion + "\u2069"
                                }
                            }
                        }
                    }
                }
            }

            Controls.DialogButtonBox {
                Layout.fillWidth: true
                standardButtons: Controls.DialogButtonBox.Close
                onRejected: updatesWindow.close()
            }
        }
    }
}

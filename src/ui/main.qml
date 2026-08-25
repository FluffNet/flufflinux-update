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
    readonly property bool systemUpToDate: kcm.checkComplete
        && !kcm.updatesAvailable && kcm.checkError.length === 0
        && kcm.recoveryActionState.length === 0
    readonly property bool recoveryActionRequired:
        kcm.recoveryActionState.length > 0

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
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: Kirigami.Units.iconSizes.large
                        implicitHeight: width
                        radius: width / 2
                        color: root.recoveryActionRequired
                            ? "#d71920" : kcm.freshnessColor

                        Accessible.name: root.recoveryActionRequired
                            ? (kcm.recoveryActionState === "protected"
                                ? i18nd("kcm_fluffupdates", "Action is required!")
                                : i18nd("kcm_fluffupdates", "Action is required"))
                            : root.systemUpToDate
                            ? i18nd("kcm_fluffupdates", "System is up to date")
                            : kcm.freshnessText

                        Text {
                            anchors.centerIn: parent
                            visible: root.systemUpToDate
                            text: "\u2713"
                            color: "white"
                            font.pixelSize: parent.width * 0.62
                            font.weight: Font.Black

                            Accessible.ignored: true
                        }

                        Text {
                            anchors.centerIn: parent
                            visible: kcm.recoveryActionState === "protected"
                            text: "\u00d7"
                            color: "white"
                            font.pixelSize: parent.width * 0.72
                            font.weight: Font.Black

                            Accessible.ignored: true
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        Layout.minimumWidth: Kirigami.Units.gridUnit * 12
                        spacing: 0

                        Controls.Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: root.recoveryActionRequired
                                ? (kcm.recoveryActionState === "protected"
                                    ? i18nd("kcm_fluffupdates", "Action is required!")
                                    : i18nd("kcm_fluffupdates", "Action is required"))
                                : root.systemUpToDate
                                ? i18nd("kcm_fluffupdates", "System is up to date")
                                : kcm.freshnessText
                            color: root.recoveryActionRequired
                                ? "#d71920" : kcm.freshnessColor
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
                            // The root's LayoutMirroring setting moves this
                            // left anchor to the right in RTL interfaces.
                            // Do not mirror it manually as well, or the two
                            // transformations cancel each other out.
                            anchors.left: parent.left
                            anchors.leftMargin: 2
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
            visible: kcm.recoveryNotice.length > 0
            type: Kirigami.MessageType.Information
            text: kcm.recoveryNotice
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
            if (!visible) {
                restoringWindowState = true
                width = rememberedWidth
                height = rememberedHeight
                if (rememberedMaximized) {
                    showMaximized()
                } else {
                    showNormal()
                }
                Qt.callLater(function() {
                    restoringWindowState = false
                })
            } else if (visibility === Window.Minimized) {
                if (rememberedMaximized) {
                    showMaximized()
                } else {
                    showNormal()
                }
            }
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

        function widestComparisonRow() {
            let widest = 0
            for (let index = 0; index < kcm.updatePackages.length; ++index) {
                const entry = kcm.updatePackages[index]
                const packageName = String(entry.name || "")
                const currentVersion = String(entry.currentVersion || "")
                const replacementName = entry.newName
                        && entry.newName !== entry.name
                    ? String(entry.newName) + " " : ""
                const newVersion = String(entry.newVersion || "")
                const arrow = currentVersion.length > 0 ? "→" : ""
                const rowWidth =
                    updateListFontMetrics.advanceWidth(packageName)
                    + updateListFontMetrics.advanceWidth(currentVersion)
                    + updateListFontMetrics.advanceWidth(arrow)
                    + updateListFontMetrics.advanceWidth(
                        replacementName + newVersion)
                    + Kirigami.Units.smallSpacing * 7
                widest = Math.max(widest, rowWidth)
            }
            return widest
        }

        readonly property real preferredPackageWidth: Math.max(
            Kirigami.Units.gridUnit * 8,
            widestField("name") + widestField("currentVersion")
                + Kirigami.Units.smallSpacing * 3)
        readonly property real preferredVersionsWidth: Math.max(
            Kirigami.Units.gridUnit * 8,
            updateListFontMetrics.advanceWidth("→")
                + widestField("newName")
                + Kirigami.Units.smallSpacing
                + widestField("newVersion")
                + Kirigami.Units.smallSpacing * 3)
        readonly property real preferredComparisonContentWidth: Math.max(
            Kirigami.Units.gridUnit * 12, widestComparisonRow())
        readonly property real preferredWindowWidth:
            preferredPackageWidth + preferredVersionsWidth
                + Kirigami.Units.largeSpacing * 5
        readonly property real defaultWindowWidth:
            Math.min(Screen.desktopAvailableWidth
                    - Kirigami.Units.largeSpacing * 2,
                Math.max(minimumWidth, preferredWindowWidth))
        readonly property real defaultWindowHeight:
            Math.min(Screen.desktopAvailableHeight
                    - Kirigami.Units.largeSpacing * 2,
                Math.max(minimumHeight, Kirigami.Units.gridUnit * 26))
        readonly property real maximumRestorableWidth:
            Math.max(minimumWidth, Screen.desktopAvailableWidth
                - Kirigami.Units.largeSpacing * 2)
        readonly property real maximumRestorableHeight:
            Math.max(minimumHeight, Screen.desktopAvailableHeight
                - Kirigami.Units.largeSpacing * 2)
        property real rememberedWidth: kcm.updateWindowWidth > 0
            ? Math.min(maximumRestorableWidth,
                Math.max(minimumWidth, kcm.updateWindowWidth))
            : defaultWindowWidth
        property real rememberedHeight: kcm.updateWindowHeight > 0
            ? Math.min(maximumRestorableHeight,
                Math.max(minimumHeight, kcm.updateWindowHeight))
            : defaultWindowHeight
        property bool rememberedMaximized: kcm.updateWindowMaximized
        property bool restoringWindowState: false

        visible: false
        flags: Qt.Window
        modality: Qt.NonModal
        transientParent: root.Window.window
        minimumWidth: 480
        minimumHeight: 400
        width: rememberedWidth
        height: rememberedHeight
        title: i18nd("kcm_fluffupdates", "Updates to Install")
        color: Kirigami.Theme.backgroundColor

        onWidthChanged: {
            if (visible && visibility === Window.Windowed
                    && !restoringWindowState) {
                rememberedWidth = width
            }
        }
        onHeightChanged: {
            if (visible && visibility === Window.Windowed
                    && !restoringWindowState) {
                rememberedHeight = height
            }
        }
        onVisibilityChanged: {
            if (visibility === Window.Maximized
                    || visibility === Window.FullScreen) {
                rememberedMaximized = true
            } else if (visibility === Window.Windowed) {
                rememberedMaximized = false
            }
        }
        onVisibleChanged: {
            if (visible) {
                updateList.positionViewAtBeginning()
                pacmanViewFlickable.contentY = 0
            }
        }
        onActiveChanged: {
            if (active && visible) {
                closeUpdatesWindowButton.forceActiveFocus(
                    Qt.TabFocusReason)
            }
        }
        onClosing: function(close) {
            kcm.saveUpdateWindowState(
                Math.round(rememberedWidth),
                Math.round(rememberedHeight),
                rememberedMaximized)
        }

        FontMetrics {
            id: updateListFontMetrics
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            Item {
                id: comparisonViewContainer

                readonly property bool verticalOverflow:
                    updateList.contentHeight > updateList.height + 0.5
                readonly property bool horizontalOverflow:
                    updateList.contentWidth > updateList.width + 0.5

                onVerticalOverflowChanged: {
                    if (!verticalOverflow) {
                        updateList.contentY = updateList.originY
                    }
                }
                onHorizontalOverflowChanged: {
                    if (!horizontalOverflow) {
                        updateList.contentX = updateList.originX
                    }
                }

                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: !kcm.pacmanView
                LayoutMirroring.enabled: false
                LayoutMirroring.childrenInherit: true

                Rectangle {
                    anchors.fill: parent
                    color: Kirigami.Theme.backgroundColor
                }

                Item {
                    id: comparisonVerticalGutter

                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.bottom: comparisonHorizontalGutter.top
                    width: comparisonViewContainer.verticalOverflow
                        ? Math.max(Kirigami.Units.gridUnit,
                            comparisonVerticalScrollBar.implicitWidth)
                        : 0

                    Rectangle {
                        anchors.fill: parent
                        visible: comparisonViewContainer.verticalOverflow
                        color: Kirigami.Theme.backgroundColor
                    }
                }

                Item {
                    id: comparisonHorizontalGutter

                    anchors.left: parent.left
                    anchors.right: comparisonVerticalGutter.left
                    anchors.bottom: parent.bottom
                    height: comparisonViewContainer.horizontalOverflow
                        ? Math.max(Kirigami.Units.gridUnit,
                            comparisonHorizontalScrollBar.implicitHeight)
                        : 0

                    Rectangle {
                        anchors.fill: parent
                        visible: comparisonViewContainer.horizontalOverflow
                        color: Kirigami.Theme.backgroundColor
                    }
                }

                Rectangle {
                    anchors.top: comparisonHorizontalGutter.top
                    anchors.left: comparisonHorizontalGutter.right
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    color: Kirigami.Theme.backgroundColor
                }

                ListView {
                    id: updateList

                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: comparisonVerticalGutter.left
                    anchors.bottom: comparisonHorizontalGutter.top
                    clip: true
                    interactive: true
                    acceptedButtons: Qt.LeftButton
                    synchronousDrag: true
                    flickableDirection: Flickable.AutoFlickDirection
                    contentWidth: Math.max(width,
                        updatesWindow.preferredComparisonContentWidth)
                    layoutDirection: Qt.LeftToRight
                    model: kcm.updatePackages
                    spacing: Kirigami.Units.smallSpacing
                    boundsBehavior: Flickable.StopAtBounds
                    pixelAligned: false

                    Controls.ScrollBar.vertical: Controls.ScrollBar {
                        id: comparisonVerticalScrollBar

                        parent: comparisonVerticalGutter
                        anchors.fill: parent
                        visible: comparisonViewContainer.verticalOverflow
                        policy: Controls.ScrollBar.AlwaysOn
                        active: true
                    }

                    Controls.ScrollBar.horizontal: Controls.ScrollBar {
                        id: comparisonHorizontalScrollBar

                        parent: comparisonHorizontalGutter
                        anchors.fill: parent
                        visible: comparisonViewContainer.horizontalOverflow
                        policy: Controls.ScrollBar.AlwaysOn
                        active: true
                    }

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
                        required property int index

                        width: ListView.view.contentWidth
                        height: packageRow.implicitHeight
                            + Kirigami.Units.smallSpacing * 2
                            + packageSeparator.height

                        RowLayout {
                            id: packageRow

                            anchors.fill: parent
                            anchors.bottomMargin: packageSeparator.height
                            anchors.leftMargin: Kirigami.Units.smallSpacing
                            anchors.rightMargin: Kirigami.Units.smallSpacing
                            spacing: Kirigami.Units.largeSpacing
                            layoutDirection: Qt.LeftToRight

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                spacing: Kirigami.Units.smallSpacing
                                layoutDirection: Qt.LeftToRight

                                Controls.Label {
                                    Layout.minimumWidth: implicitWidth
                                    wrapMode: Text.NoWrap
                                    color: Kirigami.Theme.textColor
                                    text: "\u2066" + modelData.name + "\u2069"
                                }

                                Controls.Label {
                                    Layout.minimumWidth: implicitWidth
                                    wrapMode: Text.NoWrap
                                    color: "#ff3b30"
                                    text: modelData.currentVersion
                                }

                                Controls.Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    visible: Boolean(modelData.currentVersion)
                                    color: Kirigami.Theme.textColor
                                    text: "→"
                                }

                                Controls.Label {
                                    Layout.minimumWidth: implicitWidth
                                    wrapMode: Text.NoWrap
                                    color: "#00b84a"
                                    text: (modelData.newName
                                            && modelData.newName !== modelData.name
                                            ? modelData.newName + " " : "")
                                        + modelData.newVersion
                                }

                                Item {
                                    Layout.fillWidth: true
                                }
                            }
                        }

                        Kirigami.Separator {
                            id: packageSeparator

                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            visible: index < updateList.count - 1
                            height: visible ? 1 : 0
                        }
                    }
                }
            }

            Item {
                id: pacmanViewContainer

                readonly property bool verticalOverflow:
                    pacmanViewFlickable.contentHeight
                        > pacmanViewFlickable.height + 0.5

                onVerticalOverflowChanged: {
                    if (!verticalOverflow) {
                        pacmanViewFlickable.contentY =
                            pacmanViewFlickable.originY
                    }
                }

                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: kcm.pacmanView
                LayoutMirroring.enabled: false
                LayoutMirroring.childrenInherit: true

                Rectangle {
                    anchors.fill: parent
                    color: Kirigami.Theme.backgroundColor
                }

                Item {
                    id: pacmanVerticalGutter

                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    width: pacmanViewContainer.verticalOverflow
                        ? Math.max(Kirigami.Units.gridUnit,
                            pacmanVerticalScrollBar.implicitWidth)
                        : 0

                    Rectangle {
                        anchors.fill: parent
                        visible: pacmanViewContainer.verticalOverflow
                        color: Kirigami.Theme.backgroundColor
                    }
                }

                Flickable {
                    id: pacmanViewFlickable

                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: pacmanVerticalGutter.left
                    anchors.bottom: parent.bottom
                    clip: true
                    interactive: true
                    acceptedButtons: Qt.LeftButton
                    synchronousDrag: true
                    flickableDirection: Flickable.VerticalFlick
                    contentWidth: width
                    contentHeight: pacmanPackageFlow.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds
                    pixelAligned: false

                    Controls.ScrollBar.vertical: Controls.ScrollBar {
                        id: pacmanVerticalScrollBar

                        parent: pacmanVerticalGutter
                        anchors.fill: parent
                        visible: pacmanViewContainer.verticalOverflow
                        policy: Controls.ScrollBar.AlwaysOn
                        active: true
                    }

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
                                    text: "\u2066"
                                        + (modelData.newName || modelData.name)
                                        + "-\u2069"
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

            Kirigami.Separator {
                Layout.fillWidth: true
            }

            Item {
                Layout.fillWidth: true
                Layout.minimumHeight: Math.max(
                    viewToggleButton.implicitHeight,
                    closeUpdatesWindowButton.implicitHeight)
                    + Kirigami.Units.smallSpacing * 2
                Layout.preferredHeight: Layout.minimumHeight
                LayoutMirroring.enabled: false
                LayoutMirroring.childrenInherit: true

                Controls.ToolButton {
                    id: viewToggleButton

                    anchors.left: parent.left
                    anchors.leftMargin: Kirigami.Units.smallSpacing
                    anchors.verticalCenter: parent.verticalCenter
                    checkable: true
                    checked: kcm.pacmanView
                    display: Controls.AbstractButton.IconOnly
                    icon.name: "view-visible"
                    Accessible.name: i18nd("kcm_fluffupdates", "Change view")
                    Controls.ToolTip.text: Accessible.name
                    Controls.ToolTip.visible: hovered
                    KeyNavigation.tab: closeUpdatesWindowButton
                    KeyNavigation.backtab: closeUpdatesWindowButton
                    onToggled: kcm.setPacmanView(checked)
                }

                Controls.Button {
                    id: closeUpdatesWindowButton

                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.verticalCenter: parent.verticalCenter
                    focusPolicy: Qt.StrongFocus
                    activeFocusOnTab: true
                    text: i18nd("kcm_fluffupdates", "Close")
                    icon.name: "dialog-close"
                    KeyNavigation.tab: viewToggleButton
                    KeyNavigation.backtab: viewToggleButton
                    onClicked: updatesWindow.close()
                    Keys.onReturnPressed: updatesWindow.close()
                    Keys.onEnterPressed: updatesWindow.close()
                    Keys.onSpacePressed: updatesWindow.close()
                }
            }
        }
    }

    Controls.Dialog {
        id: protectedRemovalDialog

        anchors.centerIn: parent
        modal: true
        title: i18nd("kcm_fluffupdates", "Action required")
        visible: kcm.recoveryDialogType === "protected"
        onRejected: kcm.resolveRemovalWarning(false)

        contentItem: RowLayout {
            width: Math.min(Kirigami.Units.gridUnit * 28,
                            root.width - Kirigami.Units.largeSpacing * 4)
            spacing: Kirigami.Units.largeSpacing

            Kirigami.Icon {
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: Kirigami.Units.iconSizes.huge
                Layout.preferredHeight: width
                source: "dialog-error"
            }

            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: i18nd("kcm_fluffupdates",
                    "Fluff Linux Update cannot continue because the upcoming update requires removing “%1”, which is a protected system package. Removing it could prevent Fluff Linux from working correctly. Please report this issue on GitHub for assistance.",
                    kcm.recoveryPackage)
            }
        }

        footer: Controls.DialogButtonBox {
            Controls.Button {
                text: i18nd("kcm_fluffupdates", "Close")
                icon.name: "dialog-close"
                Controls.DialogButtonBox.buttonRole: Controls.DialogButtonBox.RejectRole
            }

            Controls.Button {
                text: i18nd("kcm_fluffupdates", "GitHub")
                icon.name: "internet-services"
                Controls.DialogButtonBox.buttonRole: Controls.DialogButtonBox.ActionRole
                onClicked: {
                    Qt.openUrlExternally("https://github.com/FluffNet/flufflinux-update/issues")
                    kcm.resolveRemovalWarning(false)
                }
            }
        }
    }

    Controls.Dialog {
        id: warningRemovalDialog

        anchors.centerIn: parent
        modal: true
        title: i18nd("kcm_fluffupdates", "Action required")
        visible: kcm.recoveryDialogType === "warning"
        onAccepted: kcm.resolveRemovalWarning(true)
        onRejected: kcm.resolveRemovalWarning(false)

        contentItem: RowLayout {
            width: Math.min(Kirigami.Units.gridUnit * 28,
                            root.width - Kirigami.Units.largeSpacing * 4)
            spacing: Kirigami.Units.largeSpacing

            Kirigami.Icon {
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: Kirigami.Units.iconSizes.huge
                Layout.preferredHeight: width
                source: "dialog-warning"
            }

            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: i18nd("kcm_fluffupdates",
                    "To allow the system to install updates, Fluff Linux Update needs to remove “%1”. Removing this package may affect related software. If you are not sure, please report the issue on GitHub for help.",
                    kcm.recoveryPackage)
            }
        }

        footer: Controls.DialogButtonBox {
            Controls.Button {
                text: i18nd("kcm_fluffupdates", "Cancel")
                icon.name: "dialog-cancel"
                Controls.DialogButtonBox.buttonRole: Controls.DialogButtonBox.RejectRole
            }

            Controls.Button {
                text: i18nd("kcm_fluffupdates", "Accept")
                icon.name: "dialog-ok-apply"
                Controls.DialogButtonBox.buttonRole: Controls.DialogButtonBox.AcceptRole
            }

            Controls.Button {
                text: i18nd("kcm_fluffupdates", "GitHub")
                icon.name: "internet-services"
                Controls.DialogButtonBox.buttonRole: Controls.DialogButtonBox.ActionRole
                onClicked: {
                    Qt.openUrlExternally("https://github.com/FluffNet/flufflinux-update/issues")
                    kcm.resolveRemovalWarning(false)
                }
            }
        }
    }
}

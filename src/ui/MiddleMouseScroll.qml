// SPDX-FileCopyrightText: 2026 FluffNet LLC
// SPDX-License-Identifier: MIT
// Shared behavior with FluffNet/flufflinux-appcenter (qml/MiddleMouseScroll.qml).
import QtQuick
import QtQuick.Controls

// A viewport overlay, not part of the moving content. Idle behind the content
// so nested scroll views get first refusal; active above it to consume the
// stopping click without activating an app or a destructive action underneath.
MouseArea {
    id: area
    objectName: "middleMouseScroll"
    property Flickable scrollTarget: null
    property bool horizontal: false
    property bool vertical: true
    // ScrollView's Pane children consume unused mouse buttons. Those views
    // place the idle catcher above their content; nested Flickables stay below.
    property real idleZ: -1
    readonly property var hostWindow: area.Window.window
    readonly property bool canScrollX: !!scrollTarget && horizontal && scrollTarget.contentWidth > scrollTarget.width + 1
    readonly property bool canScrollY: !!scrollTarget && vertical && scrollTarget.contentHeight > scrollTarget.height + 1
    property bool scrolling: false
    property point anchorPoint: Qt.point(0, 0)
    property point pointerPoint: Qt.point(0, 0)
    property bool dragged: false
    property double lastTick: 0
    readonly property int deadZone: 12
    parent: scrollTarget
    anchors.fill: parent
    z: scrolling ? 1000 : idleZ
    enabled: !!scrollTarget && scrollTarget.enabled
    acceptedButtons: scrolling ? Qt.LeftButton | Qt.MiddleButton | Qt.RightButton : Qt.MiddleButton
    hoverEnabled: scrolling
    preventStealing: scrolling
    cursorShape: scrolling ? (canScrollX && canScrollY ? Qt.SizeAllCursor : canScrollX ? Qt.SizeHorCursor : Qt.SizeVerCursor) : Qt.ArrowCursor
    signal started()

    function blockedByPopup() {
        const overlay = area.Overlay.overlay
        // Qt's ToolTip popup item is informational, not an input-blocking popup.
        if (!overlay || !overlay.children.some(item => item.visible && item.objectName !== "ToolTip")) return false
        for (let item = area.parent; item; item = item.parent)
            if (item === overlay) return false
        return true
    }
    function stop() { scrolling = false; dragged = false }
    function velocity(distance) {
        return Math.sign(distance) * Math.min(1800, Math.max(0, Math.abs(distance) - deadZone) * 8)
    }
    function step(seconds) {
        if (!scrollTarget) return
        if (canScrollY) scrollTarget.contentY = Math.max(scrollTarget.originY,
            Math.min(scrollTarget.originY + scrollTarget.contentHeight - scrollTarget.height,
                scrollTarget.contentY + velocity(pointerPoint.y - anchorPoint.y) * seconds))
        if (canScrollX) scrollTarget.contentX = Math.max(scrollTarget.originX,
            Math.min(scrollTarget.originX + scrollTarget.contentWidth - scrollTarget.width,
                scrollTarget.contentX + velocity(pointerPoint.x - anchorPoint.x) * seconds))
    }
    onPressed: function(mouse) {
        if (scrolling) { stop(); mouse.accepted = true; return }
        if ((!canScrollX && !canScrollY) || blockedByPopup()) { mouse.accepted = false; return }
        scrollTarget.cancelFlick()
        anchorPoint = Qt.point(mouse.x, mouse.y); pointerPoint = anchorPoint
        dragged = false; lastTick = Date.now(); scrolling = true
        started(); mouse.accepted = true
    }
    onPositionChanged: function(mouse) {
        pointerPoint = Qt.point(mouse.x, mouse.y)
        if ((pressedButtons & Qt.MiddleButton) && (Math.abs(mouse.x - anchorPoint.x) > deadZone
                || Math.abs(mouse.y - anchorPoint.y) > deadZone)) dragged = true
    }
    onReleased: if (dragged) stop()
    onCanceled: stop()
    onExited: if (!(pressedButtons & Qt.MiddleButton)) stop()
    onVisibleChanged: if (!visible) stop()
    onEnabledChanged: if (!enabled) stop()
    onCanScrollXChanged: if (!canScrollX && !canScrollY) stop()
    onCanScrollYChanged: if (!canScrollX && !canScrollY) stop()
    onWheel: function(wheel) { stop(); wheel.accepted = false }
    Connections {
        target: area.hostWindow
        function onActiveChanged() { if (!area.hostWindow.active) area.stop() }
    }
    Shortcut {
        sequence: "Escape"
        enabled: area.scrolling && area.visible
        context: Qt.WindowShortcut
        onActivated: area.stop()
    }
    Timer {
        interval: 16; repeat: true; running: area.scrolling
        onTriggered: {
            if (!area.visible || !area.hostWindow || !area.hostWindow.active || area.blockedByPopup()) { area.stop(); return }
            const now = Date.now()
            area.step(Math.min(0.05, (now - area.lastTick) / 1000)); area.lastTick = now
        }
    }
    Rectangle {
        visible: area.scrolling
        x: area.anchorPoint.x - width / 2; y: area.anchorPoint.y - height / 2
        width: 30; height: 30; radius: 15
        color: area.hostWindow ? area.hostWindow.palette.window : "#202326"
        border.color: area.hostWindow ? area.hostWindow.palette.windowText : "white"
        opacity: 0.9
        Label {
            anchors.centerIn: parent
            text: area.canScrollX && area.canScrollY ? "✥" : area.canScrollX ? "↔" : "↕"
            color: parent.border.color; font.pixelSize: 22
        }
    }
}

# FLU project conventions

## QML scrolling

Use `org.kde.kirigami as Kirigami` and one `Kirigami.WheelHandler` per
application-owned `Flickable`, `ListView`, or `GridView`. Set its `target` to
the view and explicitly enable `blockTargetWheel: true` and
`scrollFlickableTarget: true`. Use KDE's scrolling settings rather than custom
wheel acceleration or momentum. Do not stack a second wheel handler on a
framework-managed container that already supplies one.

Keep touchscreen dragging/flicking enabled, with `filterMouseEvents: false`
unless explicitly required otherwise. Preserve App Center-style middle-mouse
autoscroll, keyboard navigation and scrollbars. Test mouse wheels, touchpad
events, touchscreen drags, input handoff, bounds and both update-list modes.
Report physical-device validation separately from synthetic event tests.

Reference: https://api.kde.org/qml-org-kde-kirigami-wheelhandler.html

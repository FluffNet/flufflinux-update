#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWheelEvent>
#include <QtTest>

#include <memory>

// Load the production updates Window, with only its backend replaced by a
// fixture. Deliver real Qt key/pointer events, rather than invoking handlers.
class UpdatesWindowKeysTest : public QObject
{
    Q_OBJECT
    QQmlEngine engine;
    std::unique_ptr<QQuickWindow> host;
    std::unique_ptr<QObject> root;
    QQuickWindow *window = nullptr;
    QObject *backend = nullptr;
    QQuickItem *middle = nullptr;
    QQuickItem *view = nullptr;
    QElapsedTimer wheelClock;
    QPointingDevice *touchDevice = QTest::createTouchDevice();

    QObject *wheelHandler(bool pacmanView)
    {
        return window->property(pacmanView ? "pacmanWheel" : "comparisonWheel").value<QObject *>();
    }

    void sendWheel(QPoint angle, QPoint pixel = {}, Qt::ScrollPhase phase = Qt::NoScrollPhase,
                   QPointF pos = {})
    {
        if (pos.isNull())
            pos = position();
        QWheelEvent event(pos, window->mapToGlobal(pos.toPoint()), pixel, angle,
                          Qt::NoButton, Qt::NoModifier, phase, false,
                          pixel.isNull() ? Qt::MouseEventNotSynthesized : Qt::MouseEventSynthesizedBySystem);
        event.setTimestamp(wheelClock.elapsed() + 1);
        QCoreApplication::sendEvent(window, &event);
    }

    static void viewRows()
    {
        QTest::addColumn<bool>("pacmanView");
        QTest::newRow("comparison") << false;
        QTest::newRow("pacman") << true;
    }

    void scrollableView(bool pacmanView)
    {
        QVariantList packages;
        for (int i = 0; i < 100; ++i) {
            const QString name = QStringLiteral("test-%1-").arg(i) + QString(100, QLatin1Char('a'));
            packages.append(QVariantMap{
                {QStringLiteral("name"), name},
                {QStringLiteral("currentVersion"), QStringLiteral("1.0")},
                {QStringLiteral("newVersion"), QStringLiteral("2.0")}
            });
        }
        backend->setProperty("updatePackages", packages);
        backend->setProperty("pacmanView", pacmanView);
        open();
        view = control(pacmanView ? "pacmanControl" : "listControl");
        middle = control(pacmanView ? "pacmanMiddle" : "comparisonMiddle");
        QVERIFY(view && middle);
        QTRY_VERIFY(view->property("contentHeight").toReal() > view->height() + 300);
        QCOMPARE(middle->parentItem(), view); // viewport, not scrolling content
        QTRY_COMPARE(middle->height(), view->height());
    }

    QPoint position(qreal x = 300, qreal y = 140)
    {
        return view->mapToScene(QPointF(x, y)).toPoint();
    }

    void startMiddle()
    {
        // Establish hover before pressing, including after native window
        // mapping or a view switch (where configure/enter events are queued).
        // Kirigami's wheel overlay can refresh hover when it disappears; keep
        // the platform cursor aligned with the synthetic event where supported.
        if (QGuiApplication::platformName() != QStringLiteral("wayland"))
            QCursor::setPos(window->mapToGlobal(position()));
        QTest::mouseMove(window, position());
        QTest::qWait(30);
        QTest::mouseClick(window, Qt::MiddleButton, Qt::NoModifier, position());
        QTRY_VERIFY(middle->property("scrolling").toBool());
    }

    QQuickItem *control(const char *name)
    {
        return qobject_cast<QQuickItem *>(window->property(name).value<QObject *>());
    }

    void open()
    {
        QVERIFY(QMetaObject::invokeMethod(window, "present"));
        QTRY_VERIFY(window->isVisible());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        // QTest's synthetic mouse events do not move X11's real cursor.
        // Keep it inside the test window so focus-follows-mouse policies do
        // not deactivate it halfway through a simulated gesture.
        if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
            QCursor::setPos(window->mapToGlobal(QPoint(300, 140)));
            window->requestActivate();
        }
        QTRY_COMPARE(QGuiApplication::focusWindow(), window);
        QTest::qWait(60);
    }

private Q_SLOTS:
    void init()
    {
        wheelClock.start();
        // Each row gets new native windows. Reusing a hidden transient across
        // rows can deliver the previous row's focus events to the next one.
        host = std::make_unique<QQuickWindow>();
        QFile file(QStringLiteral(FLU_QML_PATH));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto source = file.readAll();
        const auto start = source.indexOf("    Window {\n        id: updatesWindow");
        const auto end = source.indexOf("    Controls.Dialog {", start);
        QVERIFY(start >= 0 && end > start);
        auto windowSource = source.mid(start, end - start);
        windowSource.replace("id: updatesWindow", R"QML(id: updatesWindow
            property alias closeControl: closeUpdatesWindowButton
            property alias viewControl: viewToggleButton
            property alias listControl: updateList
            property alias pacmanControl: pacmanViewFlickable
            property alias comparisonMiddle: comparisonMiddleScroll
            property alias pacmanMiddle: pacmanMiddleScroll
            property alias comparisonWheel: comparisonWheelHandler
            property alias pacmanWheel: pacmanWheelHandler
            property alias comparisonBar: comparisonVerticalScrollBar
            property alias pacmanBar: pacmanVerticalScrollBar
        )QML");
        QQmlComponent component(&engine);
        component.setData(QByteArray(R"QML(
            import QtQuick
            import QtQuick.Controls as Controls
            import QtQuick.Layouts
            import QtQuick.Window
            import org.kde.kirigami as Kirigami
            Item {
                id: root
                width: 640; height: 480
                property alias testedWindow: updatesWindow
                property alias testedBackend: backend
                function i18nd(domain, message) { return message }
                QtObject {
                    id: backend
                    property int updateWindowWidth: 640
                    property int updateWindowHeight: 480
                    property bool updateWindowMaximized: false
                    property bool pacmanView: false
                    property var updatePackages: [
                        {name: "linux", currentVersion: "1", newVersion: "2"}
                    ]
                    property int saveCalls: 0
                    function setPacmanView(value) { pacmanView = value }
                    function saveUpdateWindowState(width, height, maximized) {
                        saveCalls++
                    }
                }
        )QML") + windowSource + "\n}", QUrl::fromLocalFile(QStringLiteral(FLU_QML_PATH)));
        root.reset(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        qobject_cast<QQuickItem *>(root.get())->setParentItem(host->contentItem());
        window = qobject_cast<QQuickWindow *>(root->property("testedWindow").value<QObject *>());
        backend = root->property("testedBackend").value<QObject *>();
        QVERIFY(window && backend);
        host->resize(640, 480);
        host->show();
        QVERIFY(QTest::qWaitForWindowExposed(host.get()));
    }

    void cleanup()
    {
        if (window) {
            window->close();
            QCoreApplication::processEvents();
            QVERIFY(!control("comparisonMiddle")->property("scrolling").toBool());
            QVERIFY(!control("pacmanMiddle")->property("scrolling").toBool());
        }
        root.reset();
        host.reset();
        window = nullptr;
        backend = nullptr;
        view = nullptr;
        middle = nullptr;
        QCoreApplication::processEvents();
    }

    void escapeClosesFromEveryControl_data()
    {
        QTest::addColumn<QByteArray>("focusControl");
        QTest::addColumn<bool>("pacmanView");
        QTest::newRow("close-button") << QByteArray("closeControl") << false;
        QTest::newRow("eye-button-after-tab") << QByteArray("viewControl") << false;
        QTest::newRow("comparison-list") << QByteArray("listControl") << false;
        QTest::newRow("pacman-list") << QByteArray("pacmanControl") << true;
    }

    void escapeClosesFromEveryControl()
    {
        QFETCH(QByteArray, focusControl);
        QFETCH(bool, pacmanView);
        backend->setProperty("pacmanView", pacmanView);
        // Repeat to cover reopening after Escape, not only first display.
        for (int attempt = 0; attempt < 2; ++attempt) {
            open();
            auto *item = control(focusControl.constData());
            QVERIFY(item);
            if (focusControl == "viewControl") {
                control("closeControl")->forceActiveFocus(Qt::TabFocusReason);
                QTest::keyClick(window, Qt::Key_Tab);
            } else {
                item->forceActiveFocus(Qt::TabFocusReason);
            }
            QTRY_VERIFY(item->hasActiveFocus());
            const int saves = backend->property("saveCalls").toInt();
            QTest::keyClick(window, Qt::Key_Escape);
            QTRY_VERIFY(!window->isVisible());
            QCOMPARE(backend->property("saveCalls").toInt(), saves + 1);
            QVERIFY(host->isVisible());
        }
    }

    void escapeInHostDoesNotCloseInactiveList()
    {
        open();
        host->requestActivate();
        // A transient parent can report active while its child has focus.
        // Wait for the actual keyboard window before delivering the key.
        QTRY_COMPARE(QGuiApplication::focusWindow(), host.get());
        QTest::keyClick(host.get(), Qt::Key_Escape);
        QCoreApplication::processEvents();
        QVERIFY(window->isVisible());
        window->close();
        QTest::keyClick(host.get(), Qt::Key_Escape);
        QVERIFY(host->isVisible());
    }

    void middleScrollAndEscape_data() { viewRows(); }
    void middleScrollAndEscape()
    {
        QFETCH(bool, pacmanView);
        scrollableView(pacmanView);
        startMiddle();
        QTest::qWait(80);
        QCOMPARE(view->property("contentY").toReal(), view->property("originY").toReal());
        QTest::mouseMove(window, position(300, 240));
        QTRY_VERIFY(view->property("contentY").toReal() > 20);
        QCOMPARE(middle->y(), 0.0);
        QTest::mouseMove(window, position(300, 60));
        QTRY_COMPARE(view->property("contentY").toReal(), view->property("originY").toReal());
        const qreal maximum = view->property("originY").toReal()
            + view->property("contentHeight").toReal() - view->height();
        view->setProperty("contentY", maximum - 1);
        QTest::mouseMove(window, position(300, 240));
        // ListView refines its estimated content height as delegates change.
        QTRY_COMPARE(view->property("contentY").toReal(),
                     view->property("originY").toReal() + view->property("contentHeight").toReal() - view->height());
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!middle->property("scrolling").toBool());
        QVERIFY(window->isVisible());
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!window->isVisible());
    }

    void middleHoldAndToggle_data() { viewRows(); }
    void middleHoldAndToggle()
    {
        QFETCH(bool, pacmanView);
        scrollableView(pacmanView);
        startMiddle();
        QTest::mouseClick(window, Qt::MiddleButton, Qt::NoModifier, position());
        QVERIFY(!middle->property("scrolling").toBool());
        QTest::mousePress(window, Qt::MiddleButton, Qt::NoModifier, position());
        QTest::mouseMove(window, position(300, 240), 30);
        QTRY_VERIFY(view->property("contentY").toReal() > 10);
        QTest::mouseRelease(window, Qt::MiddleButton, Qt::NoModifier, position(300, 240));
        QVERIFY(!middle->property("scrolling").toBool());
        const qreal stopped = view->property("contentY").toReal();
        QTest::qWait(100);
        QCOMPARE(view->property("contentY").toReal(), stopped);
    }

    void middleStopsWheelAnimationAndYieldsToWheel_data() { viewRows(); }
    void middleStopsWheelAnimationAndYieldsToWheel()
    {
        QFETCH(bool, pacmanView);
        scrollableView(pacmanView);
        sendWheel(QPoint(0, -1200));
        QTRY_VERIFY(view->property("contentY").toReal() > 0);
        // Keep a real button grab while the old animation would run. Offscreen
        // platforms can synthesize stale/scaled cursor hover when the wheel
        // overlay expires; that is unrelated to animation cancellation.
        QTest::mouseMove(window, position());
        QTest::mousePress(window, Qt::MiddleButton, Qt::NoModifier, position());
        QTRY_VERIFY(middle->property("scrolling").toBool());
        const qreal before = view->property("contentY").toReal();
        QTest::qWait(500);
        QCOMPARE(view->property("contentY").toReal(), before);
        QCOMPARE(wheelHandler(pacmanView)->property("target").value<QObject *>(), view);
        QTest::mouseRelease(window, Qt::MiddleButton, Qt::NoModifier, position());
        sendWheel(QPoint(0, -120));
        QTRY_VERIFY(!middle->property("scrolling").toBool());
        QTRY_VERIFY(view->property("contentY").toReal() > before);
    }

    void kirigamiWheelAndBounds_data() { viewRows(); }
    void kirigamiWheelAndBounds()
    {
        QFETCH(bool, pacmanView);
        scrollableView(pacmanView);
        auto *wheel = wheelHandler(pacmanView);
        QVERIFY(wheel);
        QCOMPARE(wheel->property("target").value<QObject *>(), view);
        QVERIFY(wheel->property("blockTargetWheel").toBool());
        QVERIFY(wheel->property("scrollFlickableTarget").toBool());
        QVERIFY(!wheel->property("filterMouseEvents").toBool());
        QVERIFY(view->property("interactive").toBool());
        const qreal step = wheel->property("verticalStepSize").toReal();
        QVERIFY(step > 0);
        sendWheel(QPoint(0, -120));
        QTRY_VERIFY(qAbs(view->property("contentY").toReal() - step) < 1);
        QTest::qWait(100);
        // Exactly one handler scrolls: the stock Flickable must not add motion.
        QVERIFY(qAbs(view->property("contentY").toReal() - step) < 1);
        auto *bar = control(pacmanView ? "pacmanBar" : "comparisonBar");
        QVERIFY(bar && bar->isVisible());
        sendWheel(QPoint(0, -120), {}, Qt::NoScrollPhase,
                  bar->mapToScene(QPointF(bar->width() / 2, bar->height() / 2)));
        QTRY_VERIFY(qAbs(view->property("contentY").toReal() - 2 * step) < 1);
        sendWheel(QPoint(0, 12000));
        QTRY_COMPARE(view->property("contentY").toReal(), view->property("originY").toReal());
        sendWheel(QPoint(0, 120));
        QTest::qWait(400);
        QCOMPARE(view->property("contentY").toReal(), view->property("originY").toReal());
        sendWheel(QPoint(0, -120000));
        // ListView refines its last separator/delegate estimate at the end;
        // Kirigami also rounds to physical pixels.
        QTRY_VERIFY(qAbs(view->property("contentY").toReal()
                        - (view->property("originY").toReal() + view->property("contentHeight").toReal()
                           - view->height())) <= 1);
    }

    void touchpadGestures_data()
    {
        QTest::addColumn<bool>("pacmanView");
        QTest::addColumn<bool>("pixelOnly");
        for (const bool pacman : {false, true}) {
            QTest::addRow("%s-pixels", pacman ? "pacman" : "comparison") << pacman << true;
            QTest::addRow("%s-fine-angles", pacman ? "pacman" : "comparison") << pacman << false;
        }
    }
    void touchpadGestures()
    {
        QFETCH(bool, pacmanView);
        QFETCH(bool, pixelOnly);
        scrollableView(pacmanView);
        // Phased Linux touchpad events use fine angle deltas. Test the pixel-only
        // fallback without phases: Qt discards angle-less ScrollUpdate events
        // after an accepted event as duplicate compatibility wheel events.
        // Repeated gestures must not reset content at the end or next beginning.
        for (int gesture = 0; gesture < 2; ++gesture) {
            const qreal before = view->property("contentY").toReal();
            if (!pixelOnly)
                sendWheel({}, {}, Qt::ScrollBegin);
            for (int i = 0; i < 6; ++i) {
                QTest::qWait(16);
                sendWheel(pixelOnly ? QPoint() : QPoint(0, -24), QPoint(0, -12),
                          pixelOnly ? Qt::NoScrollPhase : Qt::ScrollUpdate);
            }
            QTRY_VERIFY(view->property("contentY").toReal() > before + 20);
            const qreal end = view->property("contentY").toReal();
            QTest::qWait(16);
            if (!pixelOnly)
                sendWheel({}, {}, Qt::ScrollEnd);
            QTest::qWait(350);
            QVERIFY(view->property("contentY").toReal() >= end - 1);
            QVERIFY(!middle->property("scrolling").toBool());
        }
    }

    void horizontalWheel()
    {
        scrollableView(false);
        const qreal step = wheelHandler(false)->property("horizontalStepSize").toReal();
        sendWheel(QPoint(-120, 0));
        QTRY_VERIFY(qAbs(view->property("contentX").toReal() - step) < 1);
        QCOMPARE(view->property("contentY").toReal(), view->property("originY").toReal());
        sendWheel(QPoint(120, 0));
        QTRY_COMPARE(view->property("contentX").toReal(), view->property("originX").toReal());
    }

    void touchscreenFlick_data() { viewRows(); }
    void touchscreenFlick()
    {
        QFETCH(bool, pacmanView);
        scrollableView(pacmanView);
        QTest::touchEvent(window, touchDevice).press(0, position(200, 300), window);
        for (int y = 275; y >= 150; y -= 25) {
            QTest::qWait(20);
            QTest::touchEvent(window, touchDevice).move(0, position(200, y), window);
        }
        QTRY_VERIFY(view->property("contentY").toReal() > 60);
        QVERIFY(view->property("dragging").toBool());
        QTest::touchEvent(window, touchDevice).release(0, position(200, 125), window);
        QTRY_VERIFY(!view->property("dragging").toBool());
        QTRY_VERIFY(view->property("flicking").toBool());
        QVERIFY(!middle->property("scrolling").toBool());
        QVERIFY(QMetaObject::invokeMethod(view, "cancelFlick"));
    }

    void middleStopsOnViewExitAndClose_data() { viewRows(); }
    void middleStopsOnViewExitAndClose()
    {
        QFETCH(bool, pacmanView);
        scrollableView(pacmanView);
        startMiddle();
        backend->setProperty("pacmanView", !pacmanView);
        QTRY_VERIFY(!middle->property("scrolling").toBool());
        backend->setProperty("pacmanView", pacmanView);
        startMiddle();
        QTest::mouseMove(window, position(300, 150));
        QTest::mouseMove(window, QPoint(5, window->height() - 5));
        QTRY_VERIFY(!middle->property("scrolling").toBool());
        startMiddle();
        window->close();
        QTRY_VERIFY(!middle->property("scrolling").toBool());
        open();
        QVERIFY(!middle->property("scrolling").toBool());
    }

    void middleStoppingClickIsConsumed_data() { viewRows(); }
    void middleStoppingClickIsConsumed()
    {
        QFETCH(bool, pacmanView);
        scrollableView(pacmanView);
        QQmlComponent component(&engine);
        component.setData("import QtQuick.Controls\nButton { width: 80; height: 30; text: 'Test' }", QUrl());
        std::unique_ptr<QObject> button(component.create());
        QVERIFY2(button, qPrintable(component.errorString()));
        auto *item = qobject_cast<QQuickItem *>(button.get());
        item->setParentItem(view);
        item->setPosition(QPointF(260, 125));
        item->setZ(1);
        QSignalSpy clicks(button.get(), SIGNAL(clicked()));
        startMiddle();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, position());
        QVERIFY(!middle->property("scrolling").toBool());
        QCOMPARE(clicks.count(), 0);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, position());
        QCOMPARE(clicks.count(), 1);
    }

    void middleHorizontalScroll()
    {
        scrollableView(false);
        QTRY_VERIFY(middle->property("canScrollX").toBool());
        startMiddle();
        QTest::mouseMove(window, position(450, 140));
        QTRY_VERIFY(view->property("contentX").toReal() > 20);
        QCOMPARE(view->property("contentY").toReal(), view->property("originY").toReal());
    }

    void middleDoesNotStartWithoutOverflow_data() { viewRows(); }
    void middleDoesNotStartWithoutOverflow()
    {
        QFETCH(bool, pacmanView);
        scrollableView(pacmanView);
        backend->setProperty("updatePackages", QVariantList{});
        QTRY_VERIFY(!middle->property("canScrollX").toBool());
        QTRY_VERIFY(!middle->property("canScrollY").toBool());
        QTest::mouseClick(window, Qt::MiddleButton, Qt::NoModifier, position());
        QVERIFY(!middle->property("scrolling").toBool());
        sendWheel(QPoint(0, -120));
        QTest::qWait(400);
        QCOMPARE(view->property("contentY").toReal(), view->property("originY").toReal());
    }
};

QTEST_MAIN(UpdatesWindowKeysTest)
#include "updateswindowkeystest.moc"

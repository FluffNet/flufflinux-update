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
            property alias comparisonCoast: comparisonMomentum
            property alias pacmanCoast: pacmanMomentum
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
        window->close();
        QCoreApplication::processEvents();
        QVERIFY(!control("comparisonMiddle")->property("scrolling").toBool());
        QVERIFY(!control("pacmanMiddle")->property("scrolling").toBool());
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

    void middleStopsOldMomentumAndYieldsToWheel_data() { viewRows(); }
    void middleStopsOldMomentumAndYieldsToWheel()
    {
        QFETCH(bool, pacmanView);
        scrollableView(pacmanView);
        auto *coast = window->property(pacmanView ? "pacmanCoast" : "comparisonCoast").value<QObject *>();
        auto *wheel = window->property(pacmanView ? "pacmanWheel" : "comparisonWheel").value<QObject *>();
        QVERIFY(coast && wheel);
        coast->setProperty("velocityY", -1000);
        coast->setProperty("running", true);
        wheel->setProperty("touchpadGesture", true);
        wheel->setProperty("velocityY", -1000);
        startMiddle();
        QVERIFY(!coast->property("running").toBool());
        QCOMPARE(coast->property("velocityY").toReal(), 0.0);
        QVERIFY(!wheel->property("touchpadGesture").toBool());
        const qreal before = view->property("contentY").toReal();
        const QPointF pos = position();
        QWheelEvent event(pos, window->mapToGlobal(pos.toPoint()), {}, QPoint(0, -120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(window, &event);
        QTRY_VERIFY(!middle->property("scrolling").toBool());
        QTRY_VERIFY(view->property("contentY").toReal() > before);
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
    }
};

QTEST_MAIN(UpdatesWindowKeysTest)
#include "updateswindowkeystest.moc"

#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest>

#include <memory>

// Load the production updates Window, with only its backend replaced by a
// fixture. Deliver real Qt key events, rather than invoking the close handler.
class UpdatesWindowKeysTest : public QObject
{
    Q_OBJECT
    QQmlEngine engine;
    QQuickWindow host;
    std::unique_ptr<QObject> root;
    QQuickWindow *window = nullptr;
    QObject *backend = nullptr;

    QQuickItem *control(const char *name)
    {
        return qobject_cast<QQuickItem *>(window->property(name).value<QObject *>());
    }

    void open()
    {
        QVERIFY(QMetaObject::invokeMethod(window, "present"));
        QTRY_VERIFY(window->isVisible());
        QTRY_COMPARE(QGuiApplication::focusWindow(), window);
    }

private Q_SLOTS:
    void initTestCase()
    {
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
        )QML") + windowSource + "\n}", QUrl(QStringLiteral("file:///flu-updates-window-test.qml")));
        root.reset(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        qobject_cast<QQuickItem *>(root.get())->setParentItem(host.contentItem());
        window = qobject_cast<QQuickWindow *>(root->property("testedWindow").value<QObject *>());
        backend = root->property("testedBackend").value<QObject *>();
        QVERIFY(window && backend);
        host.resize(640, 480);
        host.show();
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
            QVERIFY(host.isVisible());
        }
    }

    void escapeInHostDoesNotCloseInactiveList()
    {
        open();
        host.requestActivate();
        // A transient parent can report active while its child has focus.
        // Wait for the actual keyboard window before delivering the key.
        QTRY_COMPARE(QGuiApplication::focusWindow(), &host);
        QTest::keyClick(&host, Qt::Key_Escape);
        QCoreApplication::processEvents();
        QVERIFY(window->isVisible());
        window->close();
        QTest::keyClick(&host, Qt::Key_Escape);
        QVERIFY(host.isVisible());
    }
};

QTEST_MAIN(UpdatesWindowKeysTest)
#include "updateswindowkeystest.moc"

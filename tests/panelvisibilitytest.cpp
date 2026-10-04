#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest>
#include <memory>

class PanelVisibilityTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void embeddedPanelIgnoresHiddenRenderWindow()
    {
        QFile file(QStringLiteral(FLU_QML_PATH));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto source = file.readAll();
        const auto start = source.indexOf("    // Visibility of this page");
        const auto end = source.indexOf("    Layout.minimumWidth:", start);
        QVERIFY(start >= 0 && end > start);
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport QtQuick.Window\nItem {\n"
            "property QtObject backend: QtObject {\n"
            " property bool reported: false\n"
            " function setPanelVisible(value) { reported = value }\n}\n"
            "function enforceWindowMinimumSize() {}\n"
            + source.mid(start, end - start) + "\n}", QUrl());
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> page(component.create());
        QVERIFY(page);
        auto *item = qobject_cast<QQuickItem *>(page.get());
        auto *backend = page->property("backend").value<QObject *>();
        QVERIFY(item && backend);
        // QQuickWidget's render window stays hidden even when the actual
        // QWidget/KCM is displayed. This must not create a background job.
        QQuickWindow renderWindow;
        QQuickItem container(renderWindow.contentItem());
        item->setParentItem(&container);
        QVERIFY(!renderWindow.isVisible());
        QTRY_VERIFY(backend->property("reported").toBool());
        for (int i = 0; i < 3; ++i) {
            container.setVisible(false); // Leave the System Settings module.
            QTRY_VERIFY(!backend->property("reported").toBool());
            container.setVisible(true); // Return without closing the host.
            QTRY_VERIFY(backend->property("reported").toBool());
        }
        item->setParentItem(nullptr);
    }
};
QTEST_MAIN(PanelVisibilityTest)
#include "panelvisibilitytest.moc"

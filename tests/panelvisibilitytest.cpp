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
    void downloadEstimateVisibilityAndLiveText()
    {
        QFile file(QStringLiteral(FLU_QML_PATH));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto source = file.readAll();
        const auto marker = source.indexOf("objectName: \"downloadTimeRemaining\"");
        const auto start = source.lastIndexOf("Controls.Label {", marker);
        const auto end = source.indexOf("Controls.Label {", marker);
        QVERIFY(marker >= 0 && start >= 0 && end > start);
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport QtQuick.Controls as Controls\n"
            "import QtQuick.Layouts\nItem {\n"
            "property QtObject backend: QtObject {\n"
            " property string installPhase: 'idle'\n"
            " property string downloadTimeRemaining: ''\n}\n"
            "function i18nd(domain, message, value) { return message.arg(value) }\n"
            + source.mid(start, end - start) + "\n}", QUrl());
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> page(component.create());
        QVERIFY(page);
        auto *label = page->findChild<QQuickItem *>(QStringLiteral("downloadTimeRemaining"));
        auto *backend = page->property("backend").value<QObject *>();
        QVERIFY(label && backend);
        QVERIFY(!label->isVisible());
        backend->setProperty("installPhase", QStringLiteral("downloading"));
        QVERIFY(!label->isVisible());
        backend->setProperty("downloadTimeRemaining", QStringLiteral("0:05"));
        QTRY_VERIFY(label->isVisible());
        QCOMPARE(label->property("text").toString(), QStringLiteral("Estimated time: \u20660:05\u2069"));
        backend->setProperty("downloadTimeRemaining", QStringLiteral("0:02"));
        QCOMPARE(label->property("text").toString(), QStringLiteral("Estimated time: \u20660:02\u2069"));
        backend->setProperty("downloadTimeRemaining", QString());
        QTRY_VERIFY(!label->isVisible());
        backend->setProperty("downloadTimeRemaining", QStringLiteral("0:02"));
        for (const auto *phase : {"starting", "installing", "complete", "failed", "cancelled"}) {
            backend->setProperty("installPhase", QString::fromLatin1(phase));
            QTRY_VERIFY(!label->isVisible());
        }
    }
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

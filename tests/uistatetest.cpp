#include "flu_bridge/src/backend.cxxqt.h"
#include <QDir>
#include <QFile>
#include <QMetaProperty>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class UiStateTest:public QObject {
    Q_OBJECT
    QTemporaryDir config;
    QString path;
private Q_SLOTS:
    void initTestCase() {
        QVERIFY(config.isValid());
        qputenv("XDG_CONFIG_HOME",config.path().toUtf8());
        path=config.path()+QStringLiteral("/flufflinux-update/settings.conf");
    }
    void init() {QFile::remove(path);}
    void defaultsMatchExistingKcm() {
        flu::UpdateBackend state;
        QCOMPARE(state.getPacmanView(),false);
        QCOMPARE(state.getUpdateWindowWidth(),0);
        QCOMPARE(state.getUpdateWindowHeight(),0);
        QCOMPARE(state.getUpdateWindowMaximized(),false);
        QCOMPARE(state.getInstallPhase(),QStringLiteral("idle"));
    }
    void viewChangeNotifiesExactlyOnceAndPersists() {
        flu::UpdateBackend state;
        QSignalSpy changes(&state,&flu::UpdateBackend::pacmanViewChanged);
        state.setPacmanView(true);
        QCOMPARE(state.getPacmanView(),true);QCOMPARE(changes.count(),1);
        state.setPacmanView(true);QCOMPARE(changes.count(),1);
        QSettings settings(path,QSettings::IniFormat);
        QCOMPARE(settings.value("Interface/PacmanView").toBool(),true);
        state.setPacmanView(false);QCOMPARE(changes.count(),2);
        settings.sync();QCOMPARE(settings.value("Interface/PacmanView").toBool(),false);
    }
    void restoringSettingsDoesNotClamp() {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSettings settings(path,QSettings::IniFormat);
        settings.setValue("UpdateWindow/Width",0);
        settings.setValue("UpdateWindow/Height",-1);
        settings.setValue("UpdateWindow/Maximized",true);
        settings.setValue("Interface/PacmanView",true);settings.sync();
        flu::UpdateBackend state;
        QCOMPARE(state.getUpdateWindowWidth(),0);
        QCOMPARE(state.getUpdateWindowHeight(),-1);
        QCOMPARE(state.getUpdateWindowMaximized(),true);
        QCOMPARE(state.getPacmanView(),true);
    }
    void savedWindowState_data() {
        QTest::addColumn<int>("width");QTest::addColumn<int>("height");
        QTest::addColumn<bool>("maximized");QTest::addColumn<int>("expectedWidth");QTest::addColumn<int>("expectedHeight");
        QTest::newRow("minimum")<<0<<-1<<false<<480<<400;
        QTest::newRow("normal")<<1024<<768<<false<<1024<<768;
        QTest::newRow("maximized")<<1280<<900<<true<<1280<<900;
    }
    void savedWindowState() {
        QFETCH(int,width);QFETCH(int,height);QFETCH(bool,maximized);
        QFETCH(int,expectedWidth);QFETCH(int,expectedHeight);
        flu::UpdateBackend state;
        QSignalSpy changes(&state,&flu::UpdateBackend::updateWindowWidthChanged);
        state.saveUpdateWindowState(width,height,maximized);
        QCOMPARE(state.getUpdateWindowWidth(),expectedWidth);
        QCOMPARE(state.getUpdateWindowHeight(),expectedHeight);
        QCOMPARE(state.getUpdateWindowMaximized(),maximized);
        QCOMPARE(changes.count(),1);
        state.saveUpdateWindowState(width,height,maximized);QCOMPARE(changes.count(),1);
        QSettings settings(path,QSettings::IniFormat);
        QCOMPARE(settings.value("UpdateWindow/Width").toInt(),expectedWidth);
        QCOMPARE(settings.value("UpdateWindow/Height").toInt(),expectedHeight);
    }
    void nativePropertiesNotifyAndRemainReadOnly() {
        flu::UpdateBackend state;
        const auto *meta=state.metaObject();
        for(int i=meta->propertyOffset();i<meta->propertyCount();++i) {
            const auto property=meta->property(i);
            QVERIFY2(property.hasNotifySignal(),property.name());
            QCOMPARE(property.isWritable(),QByteArray(property.name())=="pacmanView");
        }
        QCOMPARE(state.property("updatePackages").toList().size(),0);
    }
    void queuedSnapshotsSurviveRepeatedConstructionAndDestruction() {
        for(int i=0;i<20;++i) {flu::UpdateBackend state;QCoreApplication::processEvents();}
        QTest::qWait(850); // A late queued callback must not access a destroyed QObject.
    }
};
QTEST_GUILESS_MAIN(UiStateTest)
#include "uistatetest.moc"

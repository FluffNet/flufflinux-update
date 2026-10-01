#include "flu_bridge/src/ui_state.cxxqt.h"

#include <QSignalSpy>
#include <QtTest>

class UiStateTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void defaultsMatchExistingKcm()
    {
        flu::UiState state;
        QCOMPARE(state.getPacmanView(), false);
        QCOMPARE(state.getWindowWidth(), 0);
        QCOMPARE(state.getWindowHeight(), 0);
        QCOMPARE(state.getWindowMaximized(), false);
    }

    void viewChangeNotifiesExactlyOnce()
    {
        flu::UiState state;
        QSignalSpy changes(&state, &flu::UiState::pacmanViewChanged);
        state.setPacmanView(true);
        QCOMPARE(state.getPacmanView(), true);
        QCOMPARE(changes.count(), 1);
        state.setPacmanView(true);
        QCOMPARE(changes.count(), 1);
        state.setPacmanView(false);
        QCOMPARE(changes.count(), 2);
    }

    void restoringSettingsDoesNotClamp()
    {
        flu::UiState state;
        state.setWindowWidth(0);
        state.setWindowHeight(-1);
        state.setWindowMaximized(true);
        QCOMPARE(state.getWindowWidth(), 0);
        QCOMPARE(state.getWindowHeight(), -1);
        QCOMPARE(state.getWindowMaximized(), true);
    }

    void savedWindowState_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        QTest::addColumn<bool>("maximized");
        QTest::addColumn<int>("expectedWidth");
        QTest::addColumn<int>("expectedHeight");
        QTest::newRow("minimum") << 0 << -1 << false << 480 << 400;
        QTest::newRow("normal") << 1024 << 768 << false << 1024 << 768;
        QTest::newRow("maximized") << 1280 << 900 << true << 1280 << 900;
    }

    void savedWindowState()
    {
        QFETCH(int, width);
        QFETCH(int, height);
        QFETCH(bool, maximized);
        QFETCH(int, expectedWidth);
        QFETCH(int, expectedHeight);
        flu::UiState state;
        QSignalSpy widthChanges(&state, &flu::UiState::windowWidthChanged);
        state.saveWindowState(width, height, maximized);
        QCOMPARE(state.getWindowWidth(), expectedWidth);
        QCOMPARE(state.getWindowHeight(), expectedHeight);
        QCOMPARE(state.getWindowMaximized(), maximized);
        QCOMPARE(widthChanges.count(), 1);
        state.saveWindowState(width, height, maximized);
        QCOMPARE(widthChanges.count(), 1);
    }
};

QTEST_GUILESS_MAIN(UiStateTest)
#include "uistatetest.moc"

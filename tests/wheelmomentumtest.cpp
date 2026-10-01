#include <QFile>
#include <QJSEngine>
#include <QRegularExpression>
#include <QtTest>

#include <memory>

// Execute the actual production QML functions with Qt's JavaScript engine,
// not a second implementation of the scrolling equations. This covers their
// calculations; real pointer-device delivery and perceived feel are separate
// desktop/manual checks.
class WheelMomentumTest : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QJSEngine> engine;

    static QString functionSource(const QString &source, const QString &name)
    {
        const auto match = QRegularExpression(
            QStringLiteral("function\\s+%1\\s*\\(").arg(name)).match(source);
        if (!match.hasMatch()) {
            return {};
        }
        const auto opening = source.indexOf(QLatin1Char('{'), match.capturedEnd());
        int depth = 0;
        for (auto position = opening; position >= 0 && position < source.size(); ++position) {
            if (source[position] == QLatin1Char('{')) {
                ++depth;
            } else if (source[position] == QLatin1Char('}') && --depth == 0) {
                return source.mid(match.capturedStart(), position - match.capturedStart() + 1);
            }
        }
        return {};
    }

    QJSValue evaluate(const QString &source)
    {
        return engine->evaluate(source);
    }

    double number(const QString &expression)
    {
        return evaluate(expression).toNumber();
    }

private Q_SLOTS:
    void init()
    {
        QFile qml(QStringLiteral(FLU_QML_PATH));
        QVERIFY(qml.open(QIODevice::ReadOnly));
        const QString source = QString::fromUtf8(qml.readAll());
        const QStringList names = {
            QStringLiteral("beginWheelGesture"), QStringLiteral("blendWheelVelocity"),
            QStringLiteral("scrollFromWheel"), QStringLiteral("finishWheelGesture"),
            QStringLiteral("advanceWheelMomentum"),
        };
        QStringList fields;
        for (const auto &name : names) {
            const QString implementation = functionSource(source, name);
            QVERIFY2(!implementation.isEmpty(), qPrintable(name));
            fields.append(name + QStringLiteral(": ") + implementation);
        }
        engine = std::make_unique<QJSEngine>();
        const QString script = QStringLiteral(R"JS(
            var clockNow = 1000;
            Date.now = function() { return clockNow; };
            var Kirigami = { Units: { gridUnit: 18 } };
            var updatesWindow = { %1 };
            var flickable = {
                originX: 0, originY: 0, contentX: 500, contentY: 500,
                width: 400, height: 400, contentWidth: 4000, contentHeight: 4000,
                maximumFlickVelocity: 6000
            };
            var handler = {touchpadGesture: false, velocityX: 0, velocityY: 0, lastEventTime: 990};
            var momentum = {
                velocityX: 0, velocityY: 0, lastFrameTime: 1000, running: false,
                start: function() { this.running = true; },
                stop: function() { this.running = false; }
            };
            function wheel(x, y, ax, ay) {
                return {pixelDelta: {x:x, y:y}, angleDelta: {x:ax, y:ay}, accepted: false};
            }
        )JS").arg(fields.join(QStringLiteral(",\n")));
        // QML exposes sibling methods in the object's lexical scope. Provide
        // that same scope when running the functions outside the QML object.
        QString aliases;
        for (const auto &name : names) {
            aliases += QStringLiteral("var %1 = updatesWindow.%1;\n").arg(name);
        }
        const auto result = evaluate(script + aliases);
        QVERIFY2(!result.isError(), qPrintable(result.toString()));
    }

    void pixelDeltasRemainOneToOne()
    {
        const auto result = evaluate(QStringLiteral(
            "var event = wheel(-60,-120,0,0);"
            "updatesWindow.scrollFromWheel(flickable,event,handler,momentum,true);"));
        QVERIFY2(!result.isError(), qPrintable(result.toString()));
        QCOMPARE(number(QStringLiteral("flickable.contentX")), 560.0);
        QCOMPARE(number(QStringLiteral("flickable.contentY")), 620.0);
        QVERIFY(evaluate(QStringLiteral("event.accepted && handler.touchpadGesture")).toBool());
    }

    void mouseWheelKeepsExistingStep()
    {
        evaluate(QStringLiteral(
            "updatesWindow.scrollFromWheel(flickable,wheel(0,0,0,120),handler,momentum,false);"));
        QCOMPARE(number(QStringLiteral("flickable.contentY")), 410.0);
        QVERIFY(!evaluate(QStringLiteral("handler.touchpadGesture")).toBool());
    }

    void touchingAgainDoesNotStopMomentum()
    {
        evaluate(QStringLiteral(
            "momentum.velocityY=-2000; momentum.running=true;"
            "updatesWindow.beginWheelGesture(handler); clockNow=1016;"
            "updatesWindow.advanceWheelMomentum(flickable,momentum,false,true);"));
        QCOMPARE(number(QStringLiteral("flickable.contentY")), 532.0);
        QVERIFY(number(QStringLiteral("momentum.velocityY")) < -1000);
        QVERIFY(evaluate(QStringLiteral("momentum.running")).toBool());
    }

    void touchSlowsRatherThanStopsCoasting()
    {
        evaluate(QStringLiteral(
            "momentum.velocityY=-2000; clockNow=1016;"
            "updatesWindow.advanceWheelMomentum(flickable,momentum,false,false);"));
        const double idleSpeed = -number(QStringLiteral("momentum.velocityY"));
        evaluate(QStringLiteral(
            "momentum.velocityY=-2000; momentum.lastFrameTime=1000;"
            "updatesWindow.advanceWheelMomentum(flickable,momentum,false,true);"));
        const double touchSpeed = -number(QStringLiteral("momentum.velocityY"));
        QVERIFY(touchSpeed > 0 && touchSpeed < idleSpeed);
    }

    void repeatedGlidesBuildMomentum()
    {
        evaluate(QStringLiteral(
            "momentum.velocityY=-1000; handler.touchpadGesture=true; handler.velocityY=-500;"
            "updatesWindow.finishWheelGesture(flickable,handler,momentum,false);"));
        QCOMPARE(number(QStringLiteral("momentum.velocityY")), -1250.0);
        QVERIFY(evaluate(QStringLiteral("momentum.running")).toBool());
    }

    void oppositeInputSlowsThenReverses()
    {
        evaluate(QStringLiteral(
            "momentum.velocityY=-1000;"
            "updatesWindow.scrollFromWheel(flickable,wheel(0,120,0,0),handler,momentum,false);"));
        QCOMPARE(number(QStringLiteral("momentum.velocityY")), -700.0);
        QCOMPARE(number(QStringLiteral("flickable.contentY")), 380.0);
        evaluate(QStringLiteral("updatesWindow.finishWheelGesture(flickable,handler,momentum,false);"));
        QVERIFY(number(QStringLiteral("momentum.velocityY")) > 0);
    }

    void boundsStopAtTheEdge()
    {
        evaluate(QStringLiteral(
            "flickable.contentY=3599; momentum.velocityY=-2000; momentum.running=true; clockNow=1016;"
            "updatesWindow.advanceWheelMomentum(flickable,momentum,false,false);"));
        QCOMPARE(number(QStringLiteral("flickable.contentY")), 3600.0);
        QCOMPARE(number(QStringLiteral("momentum.velocityY")), 0.0);
        QVERIFY(!evaluate(QStringLiteral("momentum.running")).toBool());
    }

    void speedCapAndSmallGestureThresholdAreUnchanged()
    {
        evaluate(QStringLiteral(
            "handler.touchpadGesture=true; handler.velocityY=-10000;"
            "updatesWindow.finishWheelGesture(flickable,handler,momentum,false);"));
        QCOMPARE(number(QStringLiteral("momentum.velocityY")), -6000.0);
        evaluate(QStringLiteral(
            "momentum.velocityY=0; momentum.running=false; handler.velocityY=-79;"
            "updatesWindow.finishWheelGesture(flickable,handler,momentum,false);"));
        QCOMPARE(number(QStringLiteral("momentum.velocityY")), 0.0);
        QVERIFY(!evaluate(QStringLiteral("momentum.running")).toBool());
    }
};

QTEST_GUILESS_MAIN(WheelMomentumTest)
#include "wheelmomentumtest.moc"

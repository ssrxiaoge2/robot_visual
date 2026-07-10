#include <QtTest/QtTest>

#include <QSettings>

#include "palletscheduler.h"

class PalletSchedulerTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QVERIFY(m_settingsDir.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
        clearPersistedSettings();
    }

    void init()
    {
        clearPersistedSettings();
    }

    void cleanup()
    {
        clearPersistedSettings();
    }

    void defaultSmallBoxCapacityAndFirstPoint()
    {
        PalletScheduler scheduler;
        scheduler.reset(PalletArea::SmallBox);
        scheduler.setConfig(PalletArea::SmallBox, PalletScheduler::defaultSmallBoxConfig());

        QCOMPARE(scheduler.columns(PalletArea::SmallBox), 2);
        QCOMPARE(scheduler.rows(PalletArea::SmallBox), 3);
        QCOMPARE(scheduler.perLayerCapacity(PalletArea::SmallBox), 6);
        QCOMPARE(scheduler.totalCapacity(PalletArea::SmallBox), 48);

        PalletPose offset;
        QString error;
        QVERIFY2(scheduler.nextRelativeOffset(PalletArea::SmallBox, &offset, &error),
                 qPrintable(error));
        QCOMPARE(offset.x, -235.0);
        QCOMPARE(offset.y, -290.0);
        QCOMPARE(offset.z, 0.0);
    }

    void manualResetRestartsFromFirstPoint()
    {
        PalletScheduler scheduler;
        scheduler.reset(PalletArea::LargeBox);
        scheduler.setConfig(PalletArea::LargeBox, PalletScheduler::defaultLargeBoxConfig());

        QString error;
        QVERIFY(scheduler.commitPlaced(PalletArea::LargeBox, &error));
        QVERIFY(scheduler.commitPlaced(PalletArea::LargeBox, &error));
        QCOMPARE(scheduler.placedCount(PalletArea::LargeBox), 2);

        scheduler.reset(PalletArea::LargeBox);
        QCOMPARE(scheduler.placedCount(PalletArea::LargeBox), 0);

        PalletPose offset;
        QVERIFY2(scheduler.nextRelativeOffset(PalletArea::LargeBox, &offset, &error),
                 qPrintable(error));
        QCOMPARE(offset.x, 0.0);
        QCOMPARE(offset.y, -200.0);
        QCOMPARE(offset.z, 0.0);
    }

    void fullAreaRejectsNextPoint()
    {
        PalletScheduler scheduler;
        PalletConfig cfg = PalletScheduler::defaultLargeBoxConfig();
        cfg.maxLayers = 1;
        scheduler.setConfig(PalletArea::LargeBox, cfg);
        scheduler.reset(PalletArea::LargeBox);

        QString error;
        QVERIFY(scheduler.commitPlaced(PalletArea::LargeBox, &error));
        QVERIFY(scheduler.commitPlaced(PalletArea::LargeBox, &error));

        PalletPose offset;
        QVERIFY(!scheduler.nextRelativeOffset(PalletArea::LargeBox, &offset, &error));
        QVERIFY(error.contains(QStringLiteral("码垛区已满")));
        QVERIFY(error.contains(QStringLiteral("清零")));

        QVERIFY(!scheduler.commitPlaced(PalletArea::LargeBox, &error));
        QVERIFY(error.contains(QStringLiteral("码垛区已满")));
        QVERIFY(error.contains(QStringLiteral("清零")));
    }

    void releaseHeightSuggestionExplainsRealReleaseZ()
    {
        PalletScheduler scheduler;
        PalletConfig cfg = PalletScheduler::defaultSmallBoxConfig();
        QCOMPARE(cfg.robotBaseHeightFromGround, 850.0);
        cfg.releaseZOffset = cfg.boxSize.z * 2.0;
        scheduler.setConfig(PalletArea::SmallBox, cfg);

        QStringList errors;
        QStringList suggestions;
        QVERIFY(scheduler.validateConfig(PalletArea::SmallBox, &errors, &suggestions));
        QVERIFY(errors.isEmpty());
        QVERIFY(suggestions.contains(
            QStringLiteral("目标层上方释放高度建议在 108.0-162.0 mm；真实释放地面高度 = 托盘面离地高度 + 层高 + 该高度")));
    }

    void targetTcpZIncludesLayerHeightAndGripperOffset()
    {
        PalletScheduler scheduler;
        PalletConfig cfg = PalletScheduler::defaultSmallBoxConfig();
        cfg.robotBaseHeightFromGround = 850.0;
        cfg.palletSize.z = 163.0;
        cfg.boxSize.z = 50.0;
        cfg.maxLayers = 6;
        cfg.releaseZOffset = 30.0;
        scheduler.setConfig(PalletArea::SmallBox, cfg);
        scheduler.reset(PalletArea::SmallBox);

        const int perLayer = scheduler.perLayerCapacity(PalletArea::SmallBox);
        QString error;
        for (int i = 0; i < perLayer * 4; ++i)
            QVERIFY2(scheduler.commitPlaced(PalletArea::SmallBox, &error), qPrintable(error));

        PalletPose offset;
        QVERIFY2(scheduler.nextRelativeOffset(PalletArea::SmallBox, &offset, &error),
                 qPrintable(error));
        QCOMPARE(offset.z, 363.0);
        QCOMPARE(PalletScheduler::releaseGroundZ(offset, cfg.releaseZOffset), 393.0);
        QCOMPARE(PalletScheduler::releaseTcpZ(offset, cfg.releaseZOffset, cfg.robotBaseHeightFromGround), -37.0);
    }

private:
    void clearPersistedSettings()
    {
        QSettings settings(QStringLiteral("wh-robot"), QStringLiteral("robot-visual"));
        settings.clear();
        settings.sync();
    }

    QTemporaryDir m_settingsDir;
};

QTEST_MAIN(PalletSchedulerTest)
#include "test_pallet_scheduler.moc"

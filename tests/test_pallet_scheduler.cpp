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
        cfg.releaseZOffset = cfg.boxSize.z * 2.0;
        scheduler.setConfig(PalletArea::SmallBox, cfg);

        QStringList errors;
        QStringList suggestions;
        QVERIFY(scheduler.validateConfig(PalletArea::SmallBox, &errors, &suggestions));
        QVERIFY(errors.isEmpty());
        QVERIFY(suggestions.contains(
            QStringLiteral("目标层上方释放高度建议在 108.0-162.0 mm；真实松爪 Z = 目标层 Z + 该高度")));
    }

    void validateConfigRejectsReleaseZAboveRobotLimit()
    {
        PalletScheduler scheduler;
        PalletConfig cfg = PalletScheduler::defaultSmallBoxConfig();
        cfg.originPose.z = 100.0;
        cfg.palletSize.z = 20.0;
        cfg.boxSize.z = 50.0;
        cfg.maxLayers = 3;
        cfg.releaseZOffset = 60.0;
        cfg.maxRobotZ = 275.0;
        scheduler.setConfig(PalletArea::SmallBox, cfg);

        QStringList errors;
        QStringList suggestions;
        QVERIFY(!scheduler.validateConfig(PalletArea::SmallBox, &errors, &suggestions));
        QVERIFY(suggestions.isEmpty());
        QVERIFY(errors.contains(QStringLiteral("最高层释放点 Z=280.0 超过安全上限 275.0")));
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

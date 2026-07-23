#include <QtTest/QtTest>

#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

#include "settingsmanager.h"

class SettingsManagerTest : public QObject
{
    Q_OBJECT

private slots:
    void missingFileUsesDefaults()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SettingsManager manager(dir.filePath(QStringLiteral("runtime.ini")));

        const SettingsLoadResult result = manager.load();

        QCOMPARE(result.settings.pickup.largeBasketGrabZClearanceMm, 417.0);
        QVERIFY(result.warnings.isEmpty());
    }

    void loadsValidValues()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("runtime.ini"));
        {
            QSettings ini(path, QSettings::IniFormat);
            ini.setValue(QStringLiteral("pickup/largeBasketGrabZClearanceMm"), 421.5);
            ini.setValue(QStringLiteral("depthDescent/triggerDepthMm"), 1300.0);
            ini.sync();
            QCOMPARE(ini.status(), QSettings::NoError);
        }

        SettingsManager manager(path);
        const SettingsLoadResult result = manager.load();

        QCOMPARE(result.settings.pickup.largeBasketGrabZClearanceMm, 421.5);
        QCOMPARE(result.settings.depthDescent.triggerDepthMm, 1300.0);
    }

    void invalidDepthGroupFallsBackTogether()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("runtime.ini"));
        {
            QSettings ini(path, QSettings::IniFormat);
            ini.setValue(QStringLiteral("depthDescent/triggerDepthMm"), 1300.0);
            ini.setValue(QStringLiteral("depthDescent/stepMm"), 500.0);
            ini.setValue(QStringLiteral("depthDescent/maxAccumulatedMm"), 400.0);
            ini.sync();
        }

        SettingsManager manager(path);
        const SettingsLoadResult result = manager.load();

        QCOMPARE(result.settings.depthDescent.triggerDepthMm, 1200.0);
        QCOMPARE(result.settings.depthDescent.stepMm, 200.0);
        QCOMPARE(result.settings.depthDescent.maxAccumulatedMm, 400.0);
        QVERIFY(result.warnings.join('\n').contains(QStringLiteral("深度自动下探")));
    }

    void invalidSinglePickupValueFallsBackWithoutLosingOtherValue()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("runtime.ini"));
        {
            QSettings ini(path, QSettings::IniFormat);
            ini.setValue(QStringLiteral("pickup/largeBasketGrabZClearanceMm"),
                         QStringLiteral("not-a-number"));
            ini.setValue(QStringLiteral("pickup/purpleBasketGrabZClearanceMm"), 390.0);
            ini.sync();
        }

        SettingsManager manager(path);
        const SettingsLoadResult result = manager.load();

        QCOMPARE(result.settings.pickup.largeBasketGrabZClearanceMm, 417.0);
        QCOMPARE(result.settings.pickup.purpleBasketGrabZClearanceMm, 390.0);
        QVERIFY(!result.warnings.isEmpty());
    }

    void outOfRangePickupValueFallsBackWithoutLosingOtherValue()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("runtime.ini"));
        {
            QSettings ini(path, QSettings::IniFormat);
            ini.setValue(QStringLiteral("pickup/largeBasketGrabZClearanceMm"), -1.0);
            ini.setValue(QStringLiteral("pickup/purpleBasketGrabZClearanceMm"), 390.0);
            ini.sync();
        }

        SettingsManager manager(path);
        const SettingsLoadResult result = manager.load();

        QCOMPARE(result.settings.pickup.largeBasketGrabZClearanceMm, 417.0);
        QCOMPARE(result.settings.pickup.purpleBasketGrabZClearanceMm, 390.0);
        QVERIFY(result.warnings.join('\n').contains(
            QStringLiteral("pickup/largeBasketGrabZClearanceMm")));
    }

    void stageDoesNotReplaceOfficialFileUntilCommit()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("runtime.ini"));
        RuntimeSettings candidate = RuntimeSettings::defaults();
        candidate.pickup.largeBasketGrabZClearanceMm = 426.0;
        SettingsManager manager(path);
        manager.load();

        QString error;
        QVERIFY2(manager.stageCandidate(candidate, &error), qPrintable(error));
        QVERIFY(!QFile::exists(path));
        QCOMPARE(manager.current().pickup.largeBasketGrabZClearanceMm, 417.0);

        QVERIFY2(manager.commitStaged(candidate, &error), qPrintable(error));
        QVERIFY(QFile::exists(path));
        QCOMPARE(manager.current().pickup.largeBasketGrabZClearanceMm, 426.0);

        SettingsManager reloaded(path);
        QCOMPARE(reloaded.load().settings.pickup.largeBasketGrabZClearanceMm, 426.0);
    }

    void discardStagedKeepsOfficialAndCurrentValues()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("runtime.ini"));
        SettingsManager manager(path);
        manager.load();
        RuntimeSettings candidate = RuntimeSettings::defaults();
        candidate.depthDescent.triggerDepthMm = 1400.0;

        QString error;
        QVERIFY2(manager.stageCandidate(candidate, &error), qPrintable(error));
        manager.discardStaged();

        QVERIFY(!QFile::exists(path));
        QCOMPARE(manager.current().depthDescent.triggerDepthMm, 1200.0);
    }

    void stageRejectsInvalidCandidate()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SettingsManager manager(dir.filePath(QStringLiteral("runtime.ini")));
        RuntimeSettings candidate = RuntimeSettings::defaults();
        candidate.depthDescent.stepMm = 500.0;

        QString error;
        QVERIFY(!manager.stageCandidate(candidate, &error));
        QVERIFY(error.contains(QStringLiteral("单次下探")));
    }
};

QTEST_APPLESS_MAIN(SettingsManagerTest)
#include "test_settings_manager.moc"

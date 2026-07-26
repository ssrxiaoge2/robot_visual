#include <QtTest/QtTest>
#include <QSettings>
#include <QTemporaryDir>
#include "chargesettings.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

class ChargeSettingsTest : public QObject
{
    Q_OBJECT

private slots:
    void defaultsMatchApprovedFieldValues()
    {
        const ChargeSettings s = ChargeSettings::defaults();
        QCOMPARE(s.host, QStringLiteral("192.168.115.108"));
        QCOMPARE(s.port, quint16{8899});
        QCOMPARE(s.slaveId, quint8{1});
        QCOMPARE(s.voltageV, 58.4);
        QCOMPARE(s.currentA, 50.0);
        QVERIFY(!s.cutoffCurrentA.has_value());
        QVERIFY(!s.maxChargeSeconds.has_value());
        QCOMPARE(s.startChargePercent, 15);
        QCOMPARE(s.dispatchReadyPercent, 20);
        QCOMPARE(s.stopChargePercent, 80);
        QCOMPARE(s.responseTimeoutMs, 5000);
        QCOMPARE(s.pollIntervalMs, 5000);
        QCOMPARE(s.startTimeoutMs, 90000);
        QCOMPARE(s.monitorTimeoutMs, 500000);
    }

    void rejectsInvalidThresholdOrdering()
    {
        ChargeSettings s = ChargeSettings::defaults();
        s.startChargePercent = 10;
        QVERIFY(!validateChargeSettings(s).ok);
        s = ChargeSettings::defaults();
        s.dispatchReadyPercent = 15;
        QVERIFY(!validateChargeSettings(s).ok);
        s = ChargeSettings::defaults();
        s.stopChargePercent = 20;
        QVERIFY(!validateChargeSettings(s).ok);
    }

    void acceptsStartThresholdStrictlyAboveTen()
    {
        ChargeSettings s = ChargeSettings::defaults();
        s.startChargePercent = 11;
        QVERIFY(validateChargeSettings(s).ok);
        s.startChargePercent = 10;
        QVERIFY(!validateChargeSettings(s).ok);
    }

    void rejectsInvalidElectricalAndTimeoutValues()
    {
        ChargeSettings s = ChargeSettings::defaults();
        s.voltageV = 0.0;
        QVERIFY(!validateChargeSettings(s).ok);
        s = ChargeSettings::defaults();
        s.currentA = 120.1;
        QVERIFY(!validateChargeSettings(s).ok);
        s = ChargeSettings::defaults();
        s.cutoffCurrentA = 0.0;
        QVERIFY(!validateChargeSettings(s).ok);
        s = ChargeSettings::defaults();
        s.monitorTimeoutMs = -1;
        QVERIFY(!validateChargeSettings(s).ok);
    }

    void optionalRegisterValuesMustFitUnsignedSixteenBits()
    {
        ChargeSettings settings = ChargeSettings::defaults();
        settings.cutoffCurrentA = 6553.5;
        settings.maxChargeSeconds = 65535;
        QVERIFY(validateChargeSettings(settings).ok);

        settings.cutoffCurrentA = 6553.6;
        QVERIFY(!validateChargeSettings(settings).ok);

        settings = ChargeSettings::defaults();
        settings.maxChargeSeconds = 65536;
        QVERIFY(!validateChargeSettings(settings).ok);
    }

    void loadsOptionalRegisterValuesAtExactIniBoundary()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("charge.ini"));
        QSettings ini(path, QSettings::IniFormat);
        ini.setValue(QStringLiteral("charge/cutoffCurrentA"), 6553.5);
        ini.setValue(QStringLiteral("charge/maxChargeSeconds"), 65535);
        ini.sync();

        const ChargeSettingsLoadResult loaded = loadChargeSettings(path);
        QCOMPARE(loaded.settings.cutoffCurrentA, std::optional<double>{6553.5});
        QCOMPARE(loaded.settings.maxChargeSeconds, std::optional<int>{65535});
        QVERIFY(loaded.warnings.isEmpty());
    }

    void rejectsOnlyOverflowingOptionalIniFieldsAndKeepsUnrelatedValues()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("charge.ini"));
        QSettings ini(path, QSettings::IniFormat);
        ini.setValue(QStringLiteral("connection/host"), QStringLiteral("10.20.30.40"));
        ini.setValue(QStringLiteral("charge/voltageV"), 55.5);
        ini.setValue(QStringLiteral("charge/currentA"), 42.0);
        ini.setValue(QStringLiteral("charge/cutoffCurrentA"), 6553.6);
        ini.setValue(QStringLiteral("charge/maxChargeSeconds"), 65536);
        ini.setValue(QStringLiteral("timeout/responseTimeoutMs"), 1234);
        ini.setValue(QStringLiteral("threshold/startChargePercent"), 16);
        ini.setValue(QStringLiteral("threshold/dispatchReadyPercent"), 25);
        ini.setValue(QStringLiteral("threshold/stopChargePercent"), 85);
        ini.sync();

        const ChargeSettingsLoadResult loaded = loadChargeSettings(path);
        QVERIFY(!loaded.settings.cutoffCurrentA.has_value());
        QVERIFY(!loaded.settings.maxChargeSeconds.has_value());
        QCOMPARE(loaded.settings.host, QStringLiteral("10.20.30.40"));
        QCOMPARE(loaded.settings.voltageV, 55.5);
        QCOMPARE(loaded.settings.currentA, 42.0);
        QCOMPARE(loaded.settings.responseTimeoutMs, 1234);
        QCOMPARE(loaded.settings.startChargePercent, 16);
        QCOMPARE(loaded.settings.dispatchReadyPercent, 25);
        QCOMPARE(loaded.settings.stopChargePercent, 85);
        const QString warningText = loaded.warnings.join(QStringLiteral("；"));
        QVERIFY(warningText.contains(QStringLiteral("charge/cutoffCurrentA")));
        QVERIFY(warningText.contains(QStringLiteral("charge/maxChargeSeconds")));
    }

    void persistsOptionalValuesAndRestoresInvalidFieldsToDefaults()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("charge.ini"));

        ChargeSettings settings = ChargeSettings::defaults();
        settings.cutoffCurrentA = 2.5;
        settings.maxChargeSeconds = 600;
        QString error;
        QVERIFY2(saveChargeSettings(path, settings, &error), qPrintable(error));

        QSettings ini(path, QSettings::IniFormat);
        ini.setValue(QStringLiteral("connection/port"), QStringLiteral("not-a-port"));
        ini.setValue(QStringLiteral("charge/currentA"), 0);
        ini.sync();

        const ChargeSettingsLoadResult loaded = loadChargeSettings(path);
        QCOMPARE(loaded.settings.port, quint16{8899});
        QCOMPARE(loaded.settings.currentA, 50.0);
        QCOMPARE(loaded.settings.cutoffCurrentA, std::optional<double>{2.5});
        QCOMPARE(loaded.settings.maxChargeSeconds, std::optional<int>{600});
        QVERIFY(!loaded.warnings.isEmpty());
    }

    void restoresOnlyInvalidThresholdGroupWithoutDiscardingOtherFields()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("charge.ini"));
        QSettings ini(path, QSettings::IniFormat);
        ini.setValue(QStringLiteral("connection/host"), QStringLiteral("10.10.10.10"));
        ini.setValue(QStringLiteral("connection/port"), 7788);
        ini.setValue(QStringLiteral("charge/voltageV"), 55.5);
        ini.setValue(QStringLiteral("charge/currentA"), 42.0);
        ini.setValue(QStringLiteral("timeout/responseTimeoutMs"), 1234);
        ini.setValue(QStringLiteral("threshold/startChargePercent"), 40);
        ini.setValue(QStringLiteral("threshold/dispatchReadyPercent"), 30);
        ini.setValue(QStringLiteral("threshold/stopChargePercent"), 80);
        ini.sync();

        const ChargeSettingsLoadResult loaded = loadChargeSettings(path);
        QCOMPARE(loaded.settings.host, QStringLiteral("10.10.10.10"));
        QCOMPARE(loaded.settings.port, quint16{7788});
        QCOMPARE(loaded.settings.voltageV, 55.5);
        QCOMPARE(loaded.settings.currentA, 42.0);
        QCOMPARE(loaded.settings.responseTimeoutMs, 1234);
        QCOMPARE(loaded.settings.startChargePercent, 15);
        QCOMPARE(loaded.settings.dispatchReadyPercent, 20);
        QCOMPARE(loaded.settings.stopChargePercent, 80);
        QVERIFY(!loaded.warnings.isEmpty());
    }

    void failedAtomicCommitPreservesExistingConfiguration()
    {
#ifndef Q_OS_WIN
        QSKIP("该测试使用 Windows 独占文件锁验证原子替换失败路径。");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("charge.ini"));
        ChargeSettings original = ChargeSettings::defaults();
        original.host = QStringLiteral("10.0.0.1");
        QString error;
        QVERIFY2(saveChargeSettings(path, original, &error), qPrintable(error));

        const HANDLE lock = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                                        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(lock != INVALID_HANDLE_VALUE);
        ChargeSettings replacement = original;
        replacement.host = QStringLiteral("10.0.0.2");
        QVERIFY(!saveChargeSettings(path, replacement, &error));
        CloseHandle(lock);

        QCOMPARE(loadChargeSettings(path).settings.host, original.host);
#endif
    }

    void neverPersistsAutomaticEnableState()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString error;
        QVERIFY2(saveChargeSettings(dir.filePath("charge.ini"),
                                    ChargeSettings::defaults(), &error),
                 qPrintable(error));
        QSettings ini(dir.filePath("charge.ini"), QSettings::IniFormat);
        QVERIFY(!ini.contains(QStringLiteral("automatic/enabled")));
    }
};

QTEST_APPLESS_MAIN(ChargeSettingsTest)
#include "test_charge_settings.moc"

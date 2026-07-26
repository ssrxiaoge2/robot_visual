#include <QTemporaryDir>
#include <QTest>

#include "autochargecoordinator.h"
#include "chargepilecontroller.h"
#include "chargesettings.h"
#include "chargesettingstransaction.h"

namespace {

void compareSettings(const ChargeSettings &actual,
                     const ChargeSettings &expected)
{
    QCOMPARE(actual.host, expected.host);
    QCOMPARE(actual.port, expected.port);
    QCOMPARE(actual.slaveId, expected.slaveId);
    QCOMPARE(actual.voltageV, expected.voltageV);
    QCOMPARE(actual.currentA, expected.currentA);
    QCOMPARE(actual.startChargePercent, expected.startChargePercent);
    QCOMPARE(actual.dispatchReadyPercent, expected.dispatchReadyPercent);
    QCOMPARE(actual.stopChargePercent, expected.stopChargePercent);
}

ChargeSettings originalSettings()
{
    ChargeSettings settings = ChargeSettings::defaults();
    settings.host = QStringLiteral("127.0.0.1");
    settings.port = 65530;
    return settings;
}

ChargeSettings candidateSettings(const ChargeSettings &original)
{
    ChargeSettings candidate = original;
    candidate.voltageV = 57.6;
    candidate.currentA = 48.0;
    candidate.startChargePercent = 16;
    candidate.dispatchReadyPercent = 22;
    candidate.stopChargePercent = 82;
    return candidate;
}

} // namespace

class ChargeSettingsTransactionTest : public QObject
{
    Q_OBJECT

private slots:
    void unsafeRecoveryContextRejectsWithoutChangingAnyCopy()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("charge-settings.ini"));

        const ChargeSettings original = originalSettings();
        const ChargeSettings candidate = candidateSettings(original);
        QString error;
        QVERIFY(saveChargeSettings(path, original, &error));

        ChargePileController controller;
        AutoChargeCoordinator coordinator;
        ChargeSettings deviceManagerSnapshot = original;
        QVERIFY(controller.applySettings(original, &error));
        coordinator.applySettings(original);

        // startCharge 接受请求时会同步冻结安全恢复上下文；无需等待或伪造网络响应。
        QVERIFY(controller.startCharge(
            ChargePileController::SessionOrigin::Manual, &error));

        ChargeSettingsTransactionTargets targets{
            path, &controller, &coordinator, &deviceManagerSnapshot};
        QVERIFY(!applyChargeSettingsTransaction(targets, candidate, &error));
        QVERIFY(error.contains(QStringLiteral("安全恢复上下文")));

        compareSettings(loadChargeSettings(path).settings, original);
        compareSettings(controller.appliedSettings(), original);
        compareSettings(coordinator.appliedSettings(), original);
        compareSettings(deviceManagerSnapshot, original);
    }

    void validCandidateCommitsFileControllerCoordinatorAndSnapshotTogether()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("charge-settings.ini"));

        const ChargeSettings original = originalSettings();
        const ChargeSettings candidate = candidateSettings(original);
        QString error;
        QVERIFY(saveChargeSettings(path, original, &error));

        ChargePileController controller;
        AutoChargeCoordinator coordinator;
        ChargeSettings deviceManagerSnapshot = original;
        QVERIFY(controller.applySettings(original, &error));
        coordinator.applySettings(original);

        ChargeSettingsTransactionTargets targets{
            path, &controller, &coordinator, &deviceManagerSnapshot};
        QVERIFY2(applyChargeSettingsTransaction(targets, candidate, &error),
                 qPrintable(error));

        compareSettings(loadChargeSettings(path).settings, candidate);
        compareSettings(controller.appliedSettings(), candidate);
        compareSettings(coordinator.appliedSettings(), candidate);
        compareSettings(deviceManagerSnapshot, candidate);
    }
};

QTEST_GUILESS_MAIN(ChargeSettingsTransactionTest)
#include "test_charge_settings_transaction.moc"

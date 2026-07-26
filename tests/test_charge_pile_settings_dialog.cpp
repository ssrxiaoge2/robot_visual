#include <QtTest>

#include "chargepilesettingsdialog.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTimer>

namespace {

template<typename Widget>
Widget *requiredChild(QObject &parent, const char *objectName)
{
    Widget *widget = parent.findChild<Widget *>(QString::fromLatin1(objectName));
    if (!widget)
        QTest::qFail(objectName, __FILE__, __LINE__);
    return widget;
}

void closeValidationMessageBox()
{
    QTimer::singleShot(0, [] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            box->accept();
    });
}

} // namespace

class ChargePileSettingsDialogTest : public QObject
{
    Q_OBJECT

private slots:
    void defaultsPopulateFieldVerifiedValues();
    void optionalRegistersOnlyEnableAfterExplicitOptIn();
    void invalidThresholdCombinationDoesNotRequestSave();
    void lockedDialogCannotRequestSave();
    void dialogCanBeLockedAfterOpening();
    void restoreDefaultsOnlyChangesChargeSettings();
};

void ChargePileSettingsDialogTest::defaultsPopulateFieldVerifiedValues()
{
    ChargePileSettingsDialog dialog(ChargeSettings::defaults(), false);

    QCOMPARE(requiredChild<QLineEdit>(dialog, "chargeHostEdit")->text(),
             QStringLiteral("192.168.115.108"));
    QCOMPARE(requiredChild<QSpinBox>(dialog, "chargePortSpin")->value(), 8899);
    QCOMPARE(requiredChild<QSpinBox>(dialog, "chargeSlaveIdSpin")->value(), 1);
    QCOMPARE(requiredChild<QDoubleSpinBox>(dialog, "chargeVoltageSpin")->value(), 58.4);
    QCOMPARE(requiredChild<QDoubleSpinBox>(dialog, "chargeCurrentSpin")->value(), 50.0);
    QVERIFY(!requiredChild<QCheckBox>(dialog, "cutoffCurrentCheck")->isChecked());
    QVERIFY(!requiredChild<QDoubleSpinBox>(dialog, "cutoffCurrentSpin")->isEnabled());
    QVERIFY(!requiredChild<QCheckBox>(dialog, "maxChargeSecondsCheck")->isChecked());
    QVERIFY(!requiredChild<QSpinBox>(dialog, "maxChargeSecondsSpin")->isEnabled());
}

void ChargePileSettingsDialogTest::optionalRegistersOnlyEnableAfterExplicitOptIn()
{
    ChargePileSettingsDialog dialog(ChargeSettings::defaults(), false);
    auto *cutoffCheck = requiredChild<QCheckBox>(dialog, "cutoffCurrentCheck");
    auto *cutoffSpin = requiredChild<QDoubleSpinBox>(dialog, "cutoffCurrentSpin");
    auto *maximumCheck = requiredChild<QCheckBox>(dialog, "maxChargeSecondsCheck");
    auto *maximumSpin = requiredChild<QSpinBox>(dialog, "maxChargeSecondsSpin");

    cutoffCheck->setChecked(true);
    maximumCheck->setChecked(true);

    QVERIFY(cutoffSpin->isEnabled());
    QVERIFY(maximumSpin->isEnabled());
}

void ChargePileSettingsDialogTest::invalidThresholdCombinationDoesNotRequestSave()
{
    ChargePileSettingsDialog dialog(ChargeSettings::defaults(), false);
    QSignalSpy saveSpy(&dialog, &ChargePileSettingsDialog::saveRequested);
    auto *buttonBox = requiredChild<QDialogButtonBox>(dialog, "chargeSettingsButtonBox");
    auto *saveButton = buttonBox->button(QDialogButtonBox::Save);
    QVERIFY(saveButton);

    requiredChild<QSpinBox>(dialog, "startChargePercentSpin")->setValue(25);
    requiredChild<QSpinBox>(dialog, "dispatchReadyPercentSpin")->setValue(20);
    closeValidationMessageBox();
    saveButton->click();

    QCOMPARE(saveSpy.count(), 0);
    QVERIFY(dialog.isVisible() || dialog.result() == 0);
}

void ChargePileSettingsDialogTest::lockedDialogCannotRequestSave()
{
    ChargePileSettingsDialog dialog(ChargeSettings::defaults(), true);
    QSignalSpy saveSpy(&dialog, &ChargePileSettingsDialog::saveRequested);
    auto *buttonBox = requiredChild<QDialogButtonBox>(dialog, "chargeSettingsButtonBox");
    auto *saveButton = buttonBox->button(QDialogButtonBox::Save);
    QVERIFY(saveButton);

    QVERIFY(!saveButton->isEnabled());
    saveButton->click();
    QCOMPARE(saveSpy.count(), 0);
}

void ChargePileSettingsDialogTest::dialogCanBeLockedAfterOpening()
{
    ChargePileSettingsDialog dialog(ChargeSettings::defaults(), false);
    QSignalSpy saveSpy(&dialog, &ChargePileSettingsDialog::saveRequested);
    auto *hostEdit = requiredChild<QLineEdit>(dialog, "chargeHostEdit");
    auto *voltageSpin =
        requiredChild<QDoubleSpinBox>(dialog, "chargeVoltageSpin");
    auto *responseTimeoutSpin =
        requiredChild<QSpinBox>(dialog, "responseTimeoutSpin");
    auto *restoreButton =
        requiredChild<QPushButton>(dialog, "restoreChargeDefaultsButton");
    auto *lockedBanner =
        requiredChild<QLabel>(dialog, "chargeSettingsLockedBanner");
    auto *buttonBox =
        requiredChild<QDialogButtonBox>(dialog, "chargeSettingsButtonBox");
    auto *saveButton = buttonBox->button(QDialogButtonBox::Save);
    QVERIFY(saveButton);

    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QVERIFY(hostEdit->isEnabled());
    QVERIFY(voltageSpin->isEnabled());
    QVERIFY(responseTimeoutSpin->isEnabled());
    QVERIFY(restoreButton->isEnabled());
    QVERIFY(saveButton->isEnabled());
    QVERIFY(!lockedBanner->isVisible());

    // 模拟模态窗口打开期间自动会话异步取得控制器所有权。
    dialog.setLocked(true);
    dialog.setLocked(true); // 重复状态刷新必须保持幂等。

    QVERIFY(!hostEdit->isEnabled());
    QVERIFY(!voltageSpin->isEnabled());
    QVERIFY(!responseTimeoutSpin->isEnabled());
    QVERIFY(!restoreButton->isEnabled());
    QVERIFY(!saveButton->isEnabled());
    QVERIFY(lockedBanner->isVisible());
    for (const char *groupName : {
             "chargeCommunicationGroup",
             "chargeElectricalGroup",
             "chargeTimeoutGroup"}) {
        QWidget *group = requiredChild<QWidget>(dialog, groupName);
        QVERIFY2(!group->isEnabled(), groupName);
        for (QWidget *input : group->findChildren<QWidget *>()) {
            if (qobject_cast<QLineEdit *>(input)
                || qobject_cast<QAbstractSpinBox *>(input)
                || qobject_cast<QCheckBox *>(input)) {
                const QByteArray inputName = input->objectName().toUtf8();
                QVERIFY2(!input->isEnabled(), inputName.constData());
            }
        }
    }
    saveButton->click();
    QCOMPARE(saveSpy.count(), 0);

    dialog.setLocked(false);
    QVERIFY(hostEdit->isEnabled());
    QVERIFY(voltageSpin->isEnabled());
    QVERIFY(responseTimeoutSpin->isEnabled());
    QVERIFY(restoreButton->isEnabled());
    QVERIFY(saveButton->isEnabled());
    QVERIFY(!lockedBanner->isVisible());
    // 解锁后未勾选的可选寄存器数值仍保持禁用。
    QVERIFY(!requiredChild<QDoubleSpinBox>(
        dialog, "cutoffCurrentSpin")->isEnabled());
    QVERIFY(!requiredChild<QSpinBox>(
        dialog, "maxChargeSecondsSpin")->isEnabled());
}

void ChargePileSettingsDialogTest::restoreDefaultsOnlyChangesChargeSettings()
{
    ChargeSettings custom = ChargeSettings::defaults();
    custom.host = QStringLiteral("10.20.30.40");
    custom.voltageV = 56.0;
    custom.startChargePercent = 18;
    custom.dispatchReadyPercent = 30;
    custom.stopChargePercent = 90;
    ChargePileSettingsDialog dialog(custom, false);
    QSignalSpy saveSpy(&dialog, &ChargePileSettingsDialog::saveRequested);

    requiredChild<QPushButton>(dialog, "restoreChargeDefaultsButton")->click();

    QCOMPARE(requiredChild<QLineEdit>(dialog, "chargeHostEdit")->text(),
             QStringLiteral("192.168.115.108"));
    QCOMPARE(requiredChild<QDoubleSpinBox>(dialog, "chargeVoltageSpin")->value(), 58.4);
    QCOMPARE(requiredChild<QSpinBox>(dialog, "startChargePercentSpin")->value(), 15);
    QCOMPARE(requiredChild<QSpinBox>(dialog, "dispatchReadyPercentSpin")->value(), 20);
    QCOMPARE(requiredChild<QSpinBox>(dialog, "stopChargePercentSpin")->value(), 80);
    QCOMPARE(saveSpy.count(), 0);
    QVERIFY(!dialog.findChild<QCheckBox *>(QStringLiteral("autoChargeSwitch")));
}

QTEST_MAIN(ChargePileSettingsDialogTest)
#include "test_charge_pile_settings_dialog.moc"

#include <QtTest/QtTest>

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>

#include "settingsdialog.h"

class SettingsDialogTest : public QObject
{
    Q_OBJECT

private slots:
    void buildsSixCategoriesWithStableObjectNames()
    {
        SettingsDialog dialog(RuntimeSettings::defaults(), false);
        QCOMPARE(dialog.findChild<QListWidget *>("settingsCategoryList")->count(), 6);
        QCOMPARE(dialog.findChild<QStackedWidget *>("settingsStack")->count(), 6);
        QVERIFY(dialog.findChild<QPushButton *>("restoreCategoryDefaultsButton"));
        QVERIFY(dialog.findChild<QPushButton *>("saveAndApplyButton"));
        QVERIFY(dialog.findChild<QLabel *>("runtimeLockedBanner"));
        QVERIFY(dialog.findChild<QCheckBox *>("safetyAcknowledgementCheckBox"));
    }

    void runtimeLockMakesDialogViewOnly()
    {
        SettingsDialog dialog(RuntimeSettings::defaults(), true);
        QVERIFY(dialog.findChild<QLabel *>("runtimeLockedBanner")->isVisibleTo(&dialog));
        QVERIFY(!dialog.findChild<QStackedWidget *>("settingsStack")->isEnabled());
        QVERIFY(!dialog.findChild<QPushButton *>("saveAndApplyButton")->isEnabled());
    }

    void restoresOnlyCurrentCategory()
    {
        RuntimeSettings current = RuntimeSettings::defaults();
        current.pickup.largeBasketGrabZClearanceMm = 430.0;
        current.depthDescent.triggerDepthMm = 1300.0;
        SettingsDialog dialog(current, false);

        QTest::mouseClick(dialog.findChild<QPushButton *>("restoreCategoryDefaultsButton"),
                          Qt::LeftButton);

        QCOMPARE(dialog.candidate().pickup.largeBasketGrabZClearanceMm, 417.0);
        QCOMPARE(dialog.candidate().depthDescent.triggerDepthMm, 1300.0);
    }

    void saveEmitsCandidateButCancelDoesNot()
    {
        SettingsDialog dialog(RuntimeSettings::defaults(), false);
        QSignalSpy spy(&dialog, &SettingsDialog::saveRequested);
        auto *large = dialog.findChild<QDoubleSpinBox *>("largeBasketClearanceSpin");
        large->setValue(425.0);
        QTest::mouseClick(dialog.findChild<QPushButton *>("saveAndApplyButton"),
                          Qt::LeftButton);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(dialog.candidate().pickup.largeBasketGrabZClearanceMm, 425.0);

        SettingsDialog cancelled(RuntimeSettings::defaults(), false);
        QSignalSpy cancelSpy(&cancelled, &SettingsDialog::saveRequested);
        cancelled.reject();
        QCOMPARE(cancelSpy.count(), 0);
    }

    void pickupClearanceAllowsNegativeValues()
    {
        SettingsDialog dialog(RuntimeSettings::defaults(), false);
        auto *large = dialog.findChild<QDoubleSpinBox *>("largeBasketClearanceSpin");
        auto *purple = dialog.findChild<QDoubleSpinBox *>("purpleBasketClearanceSpin");
        QVERIFY(large);
        QVERIFY(purple);
        QCOMPARE(large->minimum(), -1000.0);
        QCOMPARE(purple->minimum(), -1000.0);

        large->setValue(-25.0);
        purple->setValue(-30.0);

        QCOMPARE(dialog.candidate().pickup.largeBasketGrabZClearanceMm, -25.0);
        QCOMPARE(dialog.candidate().pickup.purpleBasketGrabZClearanceMm, -30.0);
    }

    void safetyChangeRequiresAcknowledgement()
    {
        SettingsDialog dialog(RuntimeSettings::defaults(), false);
        QSignalSpy spy(&dialog, &SettingsDialog::saveRequested);
        auto *safety = dialog.findChild<QDoubleSpinBox *>("safetyMaxXySpin");
        safety->setValue(260.0);

        QTest::mouseClick(dialog.findChild<QPushButton *>("saveAndApplyButton"),
                          Qt::LeftButton);
        QCOMPARE(spy.count(), 0);
        QVERIFY(dialog.findChild<QLabel *>("settingsChangePreview")->text()
                    .contains(QStringLiteral("250")));

        dialog.findChild<QCheckBox *>("safetyAcknowledgementCheckBox")->setChecked(true);
        QTest::mouseClick(dialog.findChild<QPushButton *>("saveAndApplyButton"),
                          Qt::LeftButton);
        QCOMPARE(spy.count(), 1);
    }

    void editsFineCorrectionCountWithApprovedRange()
    {
        SettingsDialog dialog(RuntimeSettings::defaults(), false);
        auto *spin = dialog.findChild<QSpinBox *>(
            QStringLiteral("visionMaxFineCorrectionsSpin"));
        QVERIFY(spin);
        QCOMPARE(spin->minimum(), 0);
        QCOMPARE(spin->maximum(), 2);
        QCOMPARE(spin->value(), 1);
        QVERIFY(!dialog.findChild<QSpinBox *>(
            QStringLiteral("visionMaxIterationsSpin")));

        spin->setValue(2);
        QCOMPARE(dialog.candidate().vision.maxFineCorrectionCount, 2);
        QTest::mouseClick(dialog.findChild<QPushButton *>(
                              "saveAndApplyButton"),
                          Qt::LeftButton);
        QVERIFY(dialog.findChild<QLabel *>("settingsChangePreview")->text()
                    .contains(QStringLiteral("联合精修正次数")));
    }
};

QTEST_MAIN(SettingsDialogTest)
#include "test_settings_dialog.moc"

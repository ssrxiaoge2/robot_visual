#include "shortageconfigdialog.h"
#include "shortagerecoverydialog.h"

#include <QApplication>
#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTest>
#include <QTextEdit>
#include <QTimer>

namespace {

ShortageRuntimeState runtimeWithStations()
{
    ShortageRuntimeState runtime;
    runtime.initialized = true;
    for (int stationId = 1; stationId <= 12; ++stationId) {
        ShortageStationRuntime station;
        station.stationId = stationId;
        station.stock = stationId * 10;
        runtime.stations.append(station);
    }
    return runtime;
}

ShortageUiSnapshot snapshotForRecovery()
{
    ShortageUiSnapshot snapshot;
    snapshot.runtime = runtimeWithStations();
    snapshot.summaryLine1Zh = QStringLiteral("普通恢复");
    snapshot.summaryLine2Zh = QStringLiteral("仅维护修正");
    return snapshot;
}

template <typename T>
T *requiredChild(QObject *parent, const char *objectName)
{
    T *child = parent->findChild<T *>(QString::fromLatin1(objectName));
    if (child == nullptr)
        qFatal("missing child objectName=%s", objectName);
    return child;
}

void closeNextMessageBox()
{
    QTimer::singleShot(0, [] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (auto *messageBox = qobject_cast<QMessageBox *>(widget)) {
                messageBox->accept();
                return;
            }
        }
        QTimer::singleShot(0, [] {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                if (auto *messageBox = qobject_cast<QMessageBox *>(widget)) {
                    messageBox->accept();
                    return;
                }
            }
        });
    });
}

} // namespace

class ShortageDialogTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void configHasThreeProductTabsAndTwelveRowsEach();
    void liveMesEndpointHasIndependentNamedEditor();
    void testBoundaryWarningIsAlwaysVisible();
    void manualAndFieldSourcesAreExclusive();
    void testPageProvidesEightOperationButtons();
    void saveExposesLastValidatedConfigurationOnly();
    void recoveryEditsOnlyOneStationWithReasonAndTypedId();
    void ordinaryRecoveryShowsNoTaskDecisionControls();
};

void ShortageDialogTest::initTestCase()
{
    qRegisterMetaType<ShortageMaintenanceCorrection>("ShortageMaintenanceCorrection");
}

void ShortageDialogTest::configHasThreeProductTabsAndTwelveRowsEach()
{
    ShortageConfigDialog dialog(ShortageConfigStore::sheet3Defaults(), [] {
        return ShortageEditConditions {};
    });

    QCOMPARE(dialog.minimumWidth(), 1200);
    QCOMPARE(dialog.minimumHeight(), 720);

    auto *mainTabs = requiredChild<QTabWidget>(&dialog, "shortageMainTabs");
    QVERIFY(mainTabs->count() >= 2);

    auto *productTabs = requiredChild<QTabWidget>(&dialog, "shortageProductTabs");
    QCOMPARE(productTabs->count(), 3);

    const QList<const char *> tableNames {
        "stationTable_88",
        "stationTable_88R",
        "stationTable_92",
    };
    for (const char *name : tableNames) {
        auto *table = requiredChild<QTableWidget>(&dialog, name);
        QCOMPARE(table->rowCount(), 12);
        QCOMPARE(table->columnCount(), 11);
        QCOMPARE(table->horizontalHeaderItem(0)->text(), QStringLiteral("代码工位"));
        QCOMPARE(table->horizontalHeaderItem(10)->text(), QStringLiteral("R/H"));
    }
}

void ShortageDialogTest::liveMesEndpointHasIndependentNamedEditor()
{
    ShortageConfigDialog dialog(ShortageConfigStore::sheet3Defaults(), [] {
        return ShortageEditConditions {};
    });

    auto *endpointEdit = requiredChild<QLineEdit>(&dialog, "liveMesDayEndpointEdit");
    QVERIFY(endpointEdit->text().contains(QStringLiteral(".228")));
    QVERIFY(!endpointEdit->text().contains(QStringLiteral(".229")));
    QVERIFY(dialog.findChild<QLineEdit *>(QStringLiteral("legacyMesEndpointEdit")) == nullptr);
}

void ShortageDialogTest::testBoundaryWarningIsAlwaysVisible()
{
    ShortageConfigDialog dialog(ShortageConfigStore::sheet3Defaults(), [] {
        return ShortageEditConditions {};
    });
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    auto *warning = requiredChild<QLabel>(&dialog, "testBoundaryWarningLabel");
    QVERIFY(warning->isVisible());
    QVERIFY(!warning->text().trimmed().isEmpty());
}

void ShortageDialogTest::manualAndFieldSourcesAreExclusive()
{
    ShortageConfigDialog dialog(ShortageConfigStore::sheet3Defaults(), [] {
        return ShortageEditConditions {};
    });

    auto *group = requiredChild<QButtonGroup>(&dialog, "testSourceButtonGroup");
    QVERIFY(group->exclusive());

    auto *manual = requiredChild<QRadioButton>(&dialog, "manualSourceRadio");
    auto *field = requiredChild<QRadioButton>(&dialog, "fieldSourceRadio");
    QVERIFY(manual->isChecked());

    field->click();
    QVERIFY(field->isChecked());
    QVERIFY(!manual->isChecked());

    manual->click();
    QVERIFY(manual->isChecked());
    QVERIFY(!field->isChecked());
}

void ShortageDialogTest::testPageProvidesEightOperationButtons()
{
    ShortageConfigDialog dialog(ShortageConfigStore::sheet3Defaults(), [] {
        return ShortageEditConditions {};
    });

    const QList<const char *> buttonNames {
        "testInitZeroButton",
        "testStartFieldButton",
        "testStopButton",
        "testDispatchAcceptedButton",
        "testDispatchRejectedButton",
        "testFailureBeforeUnloadButton",
        "testMaterialUnloadedButton",
        "testTaskSucceededButton",
    };
    for (const char *name : buttonNames)
        QVERIFY(requiredChild<QPushButton>(&dialog, name)->isEnabled());
}

void ShortageDialogTest::saveExposesLastValidatedConfigurationOnly()
{
    ShortageEditConditions editConditions;
    ShortageConfigDialog dialog(ShortageConfigStore::sheet3Defaults(), [&editConditions] {
        return editConditions;
    });
    QSignalSpy savedSpy(&dialog, &ShortageConfigDialog::configurationSaved);

    auto *endpointEdit = requiredChild<QLineEdit>(&dialog, "liveMesDayEndpointEdit");
    auto *save = requiredChild<QPushButton>(&dialog, "saveConfigurationButton");

    const QString validEndpoint =
        QStringLiteral("https://192.168.115.228:5084/api/MesData/day");
    endpointEdit->setText(validEndpoint);
    save->click();

    QCOMPARE(savedSpy.size(), 1);
    QCOMPARE(dialog.validatedConfiguration().parameters.liveMesDayEndpoint, validEndpoint);

    const QString blockedEndpoint =
        QStringLiteral("https://192.168.115.228:5084/api/MesData/blocked");
    editConditions.liveSamplingStopped = false;
    endpointEdit->setText(blockedEndpoint);
    closeNextMessageBox();
    save->click();

    QCOMPARE(savedSpy.size(), 1);
    QCOMPARE(dialog.validatedConfiguration().parameters.liveMesDayEndpoint, validEndpoint);

    editConditions.liveSamplingStopped = true;
    endpointEdit->setText(QStringLiteral("ftp://192.168.115.228/api/MesData/day"));
    closeNextMessageBox();
    save->click();

    QCOMPARE(savedSpy.size(), 1);
    QCOMPARE(dialog.validatedConfiguration().parameters.liveMesDayEndpoint, validEndpoint);
}

void ShortageDialogTest::recoveryEditsOnlyOneStationWithReasonAndTypedId()
{
    ShortageRecoveryDialog dialog(snapshotForRecovery(), 7);
    QSignalSpy spy(&dialog, &ShortageRecoveryDialog::maintenanceCorrectionRequested);

    auto *station = requiredChild<QSpinBox>(&dialog, "recoveryStationIdSpin");
    auto *newStock = requiredChild<QSpinBox>(&dialog, "recoveryNewStockSpin");
    auto *reason = requiredChild<QLineEdit>(&dialog, "recoveryReasonEdit");
    auto *typed = requiredChild<QLineEdit>(&dialog, "recoveryTypedStationIdEdit");
    auto *submit = requiredChild<QPushButton>(&dialog, "recoverySubmitButton");

    QCOMPARE(station->value(), 7);
    station->setValue(8);
    newStock->setValue(1234);
    reason->setText(QStringLiteral("维护现场确认"));
    typed->setText(QStringLiteral("8"));
    submit->click();

    QCOMPARE(spy.size(), 1);
    const ShortageMaintenanceCorrection correction =
        qvariant_cast<ShortageMaintenanceCorrection>(spy.takeFirst().at(0));
    QCOMPARE(correction.stationId, 8);
    QCOMPARE(correction.oldStock, 80);
    QCOMPARE(correction.newStock, 1234);
    QCOMPARE(correction.reason, QStringLiteral("维护现场确认"));
    QCOMPARE(correction.typedStationId, QStringLiteral("8"));
}

void ShortageDialogTest::ordinaryRecoveryShowsNoTaskDecisionControls()
{
    ShortageRecoveryDialog dialog(snapshotForRecovery(), 3);

    QVERIFY(dialog.findChild<QObject *>(QStringLiteral("fullClearButton")) == nullptr);
    QVERIFY(dialog.findChild<QObject *>(QStringLiteral("bulkSubmitButton")) == nullptr);
    QVERIFY(dialog.findChild<QObject *>(QStringLiteral("taskDecisionGroup")) == nullptr);
    QVERIFY(dialog.findChild<QObject *>(QStringLiteral("cancelOrderButton")) == nullptr);
}

QTEST_MAIN(ShortageDialogTest)

#include "test_shortage_dialog.moc"

#include "shortageconfigdialog.h"
#include "shortagerecoverydialog.h"

#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTest>
#include <QTemporaryDir>
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

QList<QWidget *> visibleValidationDialogs()
{
    QList<QWidget *> dialogs;
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (QString::fromLatin1(widget->metaObject()->className()) == QStringLiteral("ShortageValidationDialog")
            && widget->isVisible())
            dialogs.append(widget);
    }
    return dialogs;
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
    void configDialogSupportsMinimizeMaximizeAndClose();     ///< 旧测试窗口可与主页面切换。
    void manualSourceProvidesProductModeAndActualQty();      ///< 手工源具备完整输入。
    void sourceSelectionCallsControllerAndGatesControls();   ///< 来源切换不会误启现场采样。
    void snapshotRefreshesStationAndOrderViews();            ///< 首样本补料必须立即可见。
    void testPageProvidesAllDesignedOperations();             ///< 状态、任务和异常动作完整。
    void actionButtonsFollowControllerAvailability();         ///< 禁用只做提示，控制器仍二次校验。
    void validationTabOnlyContainsIndependentDialogEntry();   ///< 第三 Tab 不混入验证业务控件。
    void repeatedValidationOpenReusesSingleDialog();          ///< 重复入口只恢复现有验证窗口。
    void hiddenValidationEntryLeavesFirstTwoTabsUntouched();  ///< 隐藏开关不影响既有两页。
    void saveExposesLastValidatedConfigurationOnly();
    void constructorDoesNotExposeInvalidConfigurationAsValidated();
    void saveValidationFocusesInvalidParameterControl();
    void saveValidationSwitchesToInvalidStationCell();
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

void ShortageDialogTest::configDialogSupportsMinimizeMaximizeAndClose()
{
    ShortageConfigDialog dialog(ShortageConfigStore::sheet3Defaults(), [] {
        return ShortageEditConditions {};
    });

    QVERIFY(!dialog.isModal());
    QVERIFY(dialog.windowFlags().testFlag(Qt::WindowMinimizeButtonHint));
    QVERIFY(dialog.windowFlags().testFlag(Qt::WindowMaximizeButtonHint));
    QVERIFY(dialog.windowFlags().testFlag(Qt::WindowCloseButtonHint));
}

void ShortageDialogTest::manualSourceProvidesProductModeAndActualQty()
{
    ShortageConfigDialog dialog(ShortageConfigStore::sheet3Defaults(), [] {
        return ShortageEditConditions {};
    });

    auto *product = requiredChild<QComboBox>(&dialog, "manualProductCombo");
    auto *mode = requiredChild<QComboBox>(&dialog, "manualModeCombo");
    auto *actualQty = requiredChild<QLineEdit>(&dialog, "manualActualQtyEdit");
    auto *submit = requiredChild<QPushButton>(&dialog, "manualSampleSubmitButton");
    QCOMPARE(product->count(), 3);
    QCOMPARE(mode->count(), 3);
    QVERIFY(actualQty->validator() != nullptr);
    QVERIFY(submit->isEnabled());
}

void ShortageDialogTest::sourceSelectionCallsControllerAndGatesControls()
{
    QTemporaryDir testStateDir;
    QVERIFY(testStateDir.isValid());
    ShortageTestController controller(ShortageConfigStore::sheet3Defaults(),
                                      testStateDir.path(),
                                      nullptr,
                                      [] {
                                          return ShortageOperationResult {
                                              true, QStringLiteral("允许测试采样")};
                                      });
    ShortageConfigDialog dialog(ShortageConfigStore::sheet3Defaults(),
                                [] {
                                    return ShortageEditConditions {};
                                },
                                &controller,
                                controller.currentSnapshot());

    auto *manual = requiredChild<QRadioButton>(&dialog, "manualSourceRadio");
    auto *field = requiredChild<QRadioButton>(&dialog, "fieldSourceRadio");
    auto *submit = requiredChild<QPushButton>(&dialog, "manualSampleSubmitButton");
    auto *start = requiredChild<QPushButton>(&dialog, "testStartFieldButton");

    QVERIFY(manual->isChecked());
    QCOMPARE(controller.inputSource(), ShortageTestInputSource::Manual);
    QVERIFY(submit->isEnabled());
    QVERIFY(!start->isEnabled());

    field->click();
    QCOMPARE(controller.inputSource(), ShortageTestInputSource::Field);
    QVERIFY(!controller.fieldSamplingActive());
    QVERIFY(!submit->isEnabled());
    QVERIFY(start->isEnabled());

    manual->click();
    QCOMPARE(controller.inputSource(), ShortageTestInputSource::Manual);
    QVERIFY(!controller.fieldSamplingActive());
    QVERIFY(submit->isEnabled());
    QVERIFY(!start->isEnabled());
}

void ShortageDialogTest::snapshotRefreshesStationAndOrderViews()
{
    QTemporaryDir testStateDir;
    QVERIFY(testStateDir.isValid());
    ShortageTestController controller(ShortageConfigStore::sheet3Defaults(),
                                      testStateDir.path(),
                                      nullptr,
                                      [] {
                                          return ShortageOperationResult {
                                              true, QStringLiteral("允许测试采样")};
                                      });
    ShortageConfigDialog dialog(ShortageConfigStore::sheet3Defaults(),
                                [] {
                                    return ShortageEditConditions {};
                                },
                                &controller,
                                controller.currentSnapshot());

    auto *initZero = requiredChild<QPushButton>(&dialog, "testInitZeroButton");
    auto *actualQty = requiredChild<QLineEdit>(&dialog, "manualActualQtyEdit");
    auto *submit = requiredChild<QPushButton>(&dialog, "manualSampleSubmitButton");
    auto *runtime = requiredChild<QTableWidget>(&dialog, "testStationRuntimeTable");
    auto *planSummary = requiredChild<QLabel>(&dialog, "testPlanSummaryLabel");
    auto *orderSummary = requiredChild<QLabel>(&dialog, "testOrderSummaryLabel");

    initZero->click();
    actualQty->setText(QStringLiteral("100"));
    submit->click();

    QCOMPARE(runtime->item(0, 1)->text(), QStringLiteral("0"));
    QVERIFY(planSummary->text().contains(QStringLiteral("活动工位=1")));
    QVERIFY(orderSummary->text().contains(QStringLiteral("等待派单")));
}

void ShortageDialogTest::testPageProvidesAllDesignedOperations()
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
        "testSaveStateButton",
        "testReloadStateButton",
        "testClearStateButton",
        "testFailureAfterUnloadButton",
        "testResendUnloadButton",
        "testSimulateRestartButton",
        "manualSampleSubmitButton",
    };
    for (const char *name : buttonNames)
        QVERIFY(requiredChild<QPushButton>(&dialog, name)->isVisibleTo(&dialog)
                || requiredChild<QPushButton>(&dialog, name)->isEnabled()
                || !requiredChild<QPushButton>(&dialog, name)->text().isEmpty());
}

void ShortageDialogTest::actionButtonsFollowControllerAvailability()
{
    QTemporaryDir testStateDir;
    QVERIFY(testStateDir.isValid());
    ShortageTestController controller(ShortageConfigStore::sheet3Defaults(),
                                      testStateDir.path(),
                                      nullptr,
                                      [] {
                                          return ShortageOperationResult {
                                              true, QStringLiteral("允许测试采样")};
                                      });
    ShortageConfigDialog dialog(ShortageConfigStore::sheet3Defaults(),
                                [] {
                                    return ShortageEditConditions {};
                                },
                                &controller,
                                controller.currentSnapshot());

    auto *submit = requiredChild<QPushButton>(&dialog, "manualSampleSubmitButton");
    auto *start = requiredChild<QPushButton>(&dialog, "testStartFieldButton");
    auto *accept = requiredChild<QPushButton>(&dialog, "testDispatchAcceptedButton");
    auto *unload = requiredChild<QPushButton>(&dialog, "testMaterialUnloadedButton");
    auto *failAfterUnload = requiredChild<QPushButton>(&dialog, "testFailureAfterUnloadButton");
    auto *resend = requiredChild<QPushButton>(&dialog, "testResendUnloadButton");

    QVERIFY(submit->isEnabled());
    QVERIFY(!start->isEnabled());
    QVERIFY(!accept->isEnabled());
    QVERIFY(!unload->isEnabled());

    controller.initializeZeroAfterConfirmation();
    controller.applyManualSample(ProductModel::Model88, ProductionMode::LeftRight, 100);
    QVERIFY(accept->isEnabled());

    controller.simulateDispatchAccepted();
    QVERIFY(unload->isEnabled());
    QVERIFY(!failAfterUnload->isEnabled());

    controller.simulateMaterialUnloaded();
    QVERIFY(failAfterUnload->isEnabled());
    QVERIFY(resend->isEnabled());
}

void ShortageDialogTest::validationTabOnlyContainsIndependentDialogEntry()
{
    ShortageConfigDialog dialog(ShortageConfigStore::sheet3Defaults(), [] {
        return ShortageEditConditions {};
    });

    auto *mainTabs = requiredChild<QTabWidget>(&dialog, "shortageMainTabs");
    QCOMPARE(mainTabs->count(), 3);
    QCOMPARE(mainTabs->tabText(2), QStringLiteral("验证向导"));

    QWidget *entryPage = mainTabs->widget(2);
    auto *openButton = requiredChild<QPushButton>(entryPage, "openShortageValidationDialogButton");
    QCOMPARE(openButton->text(), QStringLiteral("打开验证控制台"));
    QVERIFY(entryPage->findChild<QListWidget *>(QString(), Qt::FindChildrenRecursively) == nullptr);
    QVERIFY(entryPage->findChild<QPlainTextEdit *>(QString(), Qt::FindChildrenRecursively) == nullptr);
}

void ShortageDialogTest::repeatedValidationOpenReusesSingleDialog()
{
    QTemporaryDir testStateDir;
    QVERIFY(testStateDir.isValid());
    ShortageTestController controller(ShortageConfigStore::sheet3Defaults(),
                                      testStateDir.path(),
                                      nullptr,
                                      [] {
                                          return ShortageOperationResult {
                                              true, QStringLiteral("允许测试采样")};
                                      });
    ShortageConfigDialog dialog(ShortageConfigStore::sheet3Defaults(),
                                [] {
                                    return ShortageEditConditions {};
                                },
                                &controller,
                                controller.currentSnapshot());
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    auto *openButton = dialog.findChild<QPushButton *>(QStringLiteral("openShortageValidationDialogButton"));
    QVERIFY2(openButton != nullptr, "验证向导必须提供打开验证控制台按钮");
    openButton->click();
    QTRY_COMPARE(visibleValidationDialogs().size(), 1);
    QWidget *firstDialog = visibleValidationDialogs().constFirst();
    QVERIFY(!firstDialog->isModal());
    QVERIFY(firstDialog->windowFlags().testFlag(Qt::WindowMinimizeButtonHint));
    QVERIFY(firstDialog->windowFlags().testFlag(Qt::WindowMaximizeButtonHint));
    QVERIFY(firstDialog->windowFlags().testFlag(Qt::WindowCloseButtonHint));

    firstDialog->showMinimized();
    QVERIFY(firstDialog->isMinimized());
    openButton->click();
    QTRY_COMPARE(visibleValidationDialogs().size(), 1);
    QCOMPARE(visibleValidationDialogs().constFirst(), firstDialog);
    QTRY_VERIFY(!firstDialog->isMinimized());
}

void ShortageDialogTest::hiddenValidationEntryLeavesFirstTwoTabsUntouched()
{
    ShortageConfigDialog dialog(ShortageConfigStore::sheet3Defaults(), [] {
        return ShortageEditConditions {};
    });

    auto *mainTabs = requiredChild<QTabWidget>(&dialog, "shortageMainTabs");
    QVERIFY(mainTabs->count() >= 2);
    QCOMPARE(mainTabs->tabText(0), QStringLiteral("宽屏配置"));
    QCOMPARE(mainTabs->tabText(1), QStringLiteral("完整逻辑测试"));
    QVERIFY(requiredChild<QTabWidget>(&dialog, "shortageProductTabs") != nullptr);
    QVERIFY(requiredChild<QPushButton>(&dialog, "testInitZeroButton") != nullptr);

    QVERIFY(dialog.findChild<QLineEdit *>(QStringLiteral("liveMesDayEndpointEdit")) != nullptr);
    QVERIFY(dialog.findChild<QPushButton *>(QStringLiteral("manualSampleSubmitButton")) != nullptr);
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

void ShortageDialogTest::constructorDoesNotExposeInvalidConfigurationAsValidated()
{
    ShortageConfiguration invalid = ShortageConfigStore::sheet3Defaults();
    invalid.parameters.liveMesDayEndpoint = QStringLiteral("ftp://192.168.115.229/legacy");

    ShortageConfigDialog dialog(invalid, [] {
        return ShortageEditConditions {};
    });

    QVERIFY(ShortageConfigStore::validate(dialog.validatedConfiguration()).ok);
    QVERIFY(!dialog.validatedConfiguration().parameters.liveMesDayEndpoint.contains(
        QStringLiteral(".229")));
}

void ShortageDialogTest::saveValidationFocusesInvalidParameterControl()
{
    ShortageConfigDialog dialog(ShortageConfigStore::sheet3Defaults(), [] {
        return ShortageEditConditions {};
    });
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    auto *mainTabs = requiredChild<QTabWidget>(&dialog, "shortageMainTabs");
    auto *intervalSpin = requiredChild<QSpinBox>(&dialog, "sampleIntervalSecondsSpin");
    auto *timeoutSpin = requiredChild<QSpinBox>(&dialog, "roundTimeoutSecondsSpin");
    auto *save = requiredChild<QPushButton>(&dialog, "saveConfigurationButton");

    intervalSpin->setValue(5);
    timeoutSpin->setValue(5);
    closeNextMessageBox();
    save->click();

    QCOMPARE(mainTabs->currentIndex(), 0);
    QTRY_COMPARE(QApplication::focusWidget(), intervalSpin);
}

void ShortageDialogTest::saveValidationSwitchesToInvalidStationCell()
{
    ShortageConfigDialog dialog(ShortageConfigStore::sheet3Defaults(), [] {
        return ShortageEditConditions {};
    });
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    auto *mainTabs = requiredChild<QTabWidget>(&dialog, "shortageMainTabs");
    auto *productTabs = requiredChild<QTabWidget>(&dialog, "shortageProductTabs");
    auto *table = requiredChild<QTableWidget>(&dialog, "stationTable_88R");
    auto *save = requiredChild<QPushButton>(&dialog, "saveConfigurationButton");

    mainTabs->setCurrentIndex(1);
    table->item(2, 3)->setText(QString());
    closeNextMessageBox();
    save->click();

    QCOMPARE(mainTabs->currentIndex(), 0);
    QCOMPARE(productTabs->currentWidget(), table);
    QCOMPARE(table->currentRow(), 2);
    QCOMPARE(table->currentColumn(), 3);
    QTRY_COMPARE(QApplication::focusWidget(), table);
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

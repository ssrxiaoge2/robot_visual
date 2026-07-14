#include "shortagevalidationdialog.h"

#include "shortagetestcontroller.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {

ShortageConfiguration validationTestConfiguration()
{
    ShortageConfiguration configuration;
    configuration.revision = 9003;
    configuration.parameters.preUnloadFailureLimit = 2;
    const QList<ProductModel> products {
        ProductModel::Model88,
        ProductModel::Model88R,
        ProductModel::Model92,
    };
    for (ProductModel product : products) {
        for (int stationId = 1; stationId <= 12; ++stationId) {
            ShortageStationConfig station;
            station.product = product;
            station.stationId = stationId;
            station.temporaryNo = QString::number(stationId);
            station.sitePosition = QStringLiteral("验证位置%1").arg(stationId);
            station.partNumber = QStringLiteral("VT-PN-%1").arg(stationId);
            station.enabled = true;
            station.boxQuantity = 100 + stationId;
            station.minimumStock = 20;
            station.maximumStock = 200;
            station.usageLeftRight = stationId;
            station.usageLeftOnly = stationId * 10;
            station.usageRightOnly = stationId * 100;
            configuration.stations.append(station);
        }
    }
    return configuration;
}

template <typename T>
T *requiredChild(QObject *parent, const char *objectName)
{
    T *child = parent->findChild<T *>(QString::fromLatin1(objectName));
    if (child == nullptr)
        qFatal("missing validation child objectName=%s", objectName);
    return child;
}

QString readProjectFile(const QString &relativePath)
{
    QFile file(QStringLiteral(ROBOT_VISUAL_SOURCE_DIR) + QLatin1Char('/') + relativePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

bool containsChineseText(const QString &text)
{
    // 中文文案检查使用 Unicode 码段，避免不同 Qt 版本对 \u 正则转义的兼容差异。
    for (const QChar ch : text) {
        const uint code = ch.unicode();
        if (code >= 0x4e00U && code <= 0x9fffU)
            return true;
    }
    return false;
}

QList<ShortageValidationAction> actionsOf(const ShortageValidationCase &item)
{
    // 只抽取公开验证动作，测试固定 VT 编排时不依赖 UI 文案。
    QList<ShortageValidationAction> actions;
    for (const ShortageValidationStep &step : item.steps)
        actions.append(step.action);
    return actions;
}

} // namespace

class ShortageValidationDialogTest final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void windowSupportsMinimizeMaximizeCloseAndIsNonModal(); ///< 新窗口可与主页面自由切换。
    void exposesExactlyFifteenChineseValidationCases();      ///< VT-01～VT-15 不缺项。
    void eachCaseHasStepsAndPassCriteria();                  ///< 每项均有操作和通过标准。
    void nextStepUsesOnlyTestControllerPublicActions();      ///< 向导不直接修改 Engine。
    void automaticCaseShowsActualSnapshotEvidence();         ///< 可判定项目显示预期、实际和结果。
    void fieldEvidenceWaitsForManualConfirmation();          ///< 实机项不能被本地测试冒充通过。
    void sourceFilesContainNoProductionOrHardwareDependency();///< 新 Dialog 与正式/FIFO/硬件隔离。
};

void ShortageValidationDialogTest::initTestCase()
{
    qRegisterMetaType<ShortageUiSnapshot>("ShortageUiSnapshot");
    qRegisterMetaType<ShortageTestActionAvailability>("ShortageTestActionAvailability");
}

void ShortageValidationDialogTest::windowSupportsMinimizeMaximizeCloseAndIsNonModal()
{
    ShortageValidationDialog dialog(nullptr);

    QVERIFY(!dialog.isModal());
    QVERIFY(dialog.windowFlags().testFlag(Qt::Window));
    QVERIFY(dialog.windowFlags().testFlag(Qt::WindowMinimizeButtonHint));
    QVERIFY(dialog.windowFlags().testFlag(Qt::WindowMaximizeButtonHint));
    QVERIFY(dialog.windowFlags().testFlag(Qt::WindowCloseButtonHint));
    QCOMPARE(dialog.minimumWidth(), 1280);
    QCOMPARE(dialog.minimumHeight(), 760);
}

void ShortageValidationDialogTest::exposesExactlyFifteenChineseValidationCases()
{
    ShortageValidationDialog dialog(nullptr);
    const QList<ShortageValidationCase> cases = dialog.validationCases();

    QCOMPARE(cases.size(), 15);
    for (int index = 0; index < cases.size(); ++index) {
        QCOMPARE(cases.at(index).id, QStringLiteral("VT-%1").arg(index + 1, 2, 10, QLatin1Char('0')));
        QVERIFY2(containsChineseText(cases.at(index).nameZh),
                 qPrintable(QStringLiteral("%1 名称必须为中文").arg(cases.at(index).id)));
    }

    QCOMPARE(cases.at(0).nameZh, QStringLiteral("窗口切换能力"));
    QCOMPARE(cases.at(14).nameZh, QStringLiteral("来源切换和误操作门禁"));
}

void ShortageValidationDialogTest::eachCaseHasStepsAndPassCriteria()
{
    ShortageValidationDialog dialog(nullptr);
    const QList<ShortageValidationCase> cases = dialog.validationCases();

    const QStringList criteria {
        QStringLiteral("两个窗口均可最小化、最大化和恢复；主窗口始终可以激活；重复点击只激活唯一窗口，不出现多个相同窗口；窗口操作不改变测试状态。"),
        QStringLiteral("正式状态文件哈希和主 FIFO 数量不变；AGV、机械臂和扫码硬件命令计数不增加；测试状态只写入 `test-*` 文件。"),
        QStringLiteral("页面接收手工 `actualQty=100`；真实请求计数不变；启动现场采样不可用；日志明确记录手工样本。"),
        QStringLiteral("建账后 12 工位均为 0；首样本只建基线；低位工位按同时间工位号排序；活动工位为 1；只生成一个工位 1 的 `AwaitingDispatch` 补料单并立即显示。"),
        QStringLiteral("真实采样只在现场源启动；两轮稳定前不产生稳定样本；首次稳定样本只建基线并立即显示一个 `AwaitingDispatch` 补料单；正式 Engine 和 FIFO 不变化。"),
        QStringLiteral("每个启用工位库存等于旧库存减 `5×当前模式用量`；用量 0 工位不变；基线为 105；最近增量为 5。"),
        QStringLiteral("拒收后补料单保持 `AwaitingDispatch` 且单号不变，不生成重复单；接受后绑定测试 taskId 并进入 `Running`。"),
        QStringLiteral("倒料完成时增加准确一箱；任务终态不重复加箱；未达最高位生成下一箱；达到最高位后释放活动工位并切换下一等待工位。"),
        QStringLiteral("每次倒料前失败不增加库存；达到阈值时只暂停目标工位；其他工位继续计划；日志包含工位、原库存、失败次数和处理动作。"),
        QStringLiteral("已倒料的一箱不回滚；倒料前失败计数不增加；补料单进入倒料后失败终态。"),
        QStringLiteral("库存不第二次增加；系统进入严重锁定；停止新自动意图；日志包含补料单号、taskId、工位和重复倒料处理动作。"),
        QStringLiteral("`1000→2→5` 按新周期累计 5 扣减；连续下降序列最终合计扣 5；`1000→2→1005` 只按旧周期增量 5 扣减；页面显示基线和候选变化。"),
        QStringLiteral("手工源九种组合使用正确配置用量；现场源一轮抖动不切换、两轮相同才确认；旧任务未终态时保存待切换上下文；排空后切换；库存沿用并使用新组合用量。"),
        QStringLiteral("安全状态逐字段恢复；不安全状态进入维护锁定；清空只删除 `test-*` 文件；正式文件哈希始终不变。"),
        QStringLiteral("现场采样运行中不能混用输入源；停止后可切换；非法动作按钮禁用；直接调用控制器仍被拒绝并输出中文原因。"),
    };
    QCOMPARE(cases.size(), criteria.size());

    for (int index = 0; index < cases.size(); ++index) {
        const ShortageValidationCase &item = cases.at(index);
        QVERIFY2(!item.steps.isEmpty(), qPrintable(item.id + QStringLiteral(" 必须包含步骤")));
        QCOMPARE(item.passCriteriaZh, criteria.at(index));
        for (const ShortageValidationStep &step : item.steps) {
            QVERIFY2(!step.instructionZh.trimmed().isEmpty(),
                     qPrintable(item.id + QStringLiteral(" 步骤说明不能为空")));
            QVERIFY2(!step.expectedZh.trimmed().isEmpty(),
                     qPrintable(item.id + QStringLiteral(" 通过观察不能为空")));
        }
    }

    QCOMPARE(actionsOf(cases.at(0)),
             QList<ShortageValidationAction>({
                 ShortageValidationAction::ShowInstruction,
                 ShortageValidationAction::ShowInstruction,
                 ShortageValidationAction::ShowInstruction,
                 ShortageValidationAction::ShowInstruction,
                 ShortageValidationAction::ShowInstruction,
             }));
    QVERIFY(cases.at(0).requiresManualEvidence);
    QCOMPARE(actionsOf(cases.at(1)),
             QList<ShortageValidationAction>({
                 ShortageValidationAction::ShowInstruction,
                 ShortageValidationAction::ClearTestState,
                 ShortageValidationAction::InitializeZero,
                 ShortageValidationAction::SelectManualSource,
                 ShortageValidationAction::ApplyManualSample,
                 ShortageValidationAction::ShowInstruction,
             }));
    QCOMPARE(actionsOf(cases.at(2)),
             QList<ShortageValidationAction>({
                 ShortageValidationAction::SelectManualSource,
                 ShortageValidationAction::ApplyManualSample,
                 ShortageValidationAction::ShowInstruction,
             }));
    const QList<ShortageValidationAction> establishVt04 {
        ShortageValidationAction::ClearTestState,
        ShortageValidationAction::InitializeZero,
        ShortageValidationAction::SelectManualSource,
        ShortageValidationAction::ApplyManualSample,
    };
    QCOMPARE(actionsOf(cases.at(3)), establishVt04);
    QCOMPARE(cases.at(3).steps.at(3).product, ProductModel::Model88);
    QCOMPARE(cases.at(3).steps.at(3).mode, ProductionMode::LeftRight);
    QCOMPARE(cases.at(3).steps.at(3).actualQty, qint64{100});
    QCOMPARE(actionsOf(cases.at(4)),
             QList<ShortageValidationAction>({
                 ShortageValidationAction::ClearTestState,
                 ShortageValidationAction::InitializeZero,
                 ShortageValidationAction::SelectFieldSource,
                 ShortageValidationAction::StartFieldSampling,
                 ShortageValidationAction::ShowInstruction,
                 ShortageValidationAction::StopFieldSampling,
             }));
    QCOMPARE(actionsOf(cases.at(5)),
             QList<ShortageValidationAction>({
                 ShortageValidationAction::ClearTestState,
                 ShortageValidationAction::InitializeZero,
                 ShortageValidationAction::SelectManualSource,
                 ShortageValidationAction::ApplyManualSample,
                 ShortageValidationAction::ApplyManualSample,
             }));
    QCOMPARE(actionsOf(cases.at(6)), establishVt04
             + QList<ShortageValidationAction>({
                 ShortageValidationAction::DispatchRejected,
                 ShortageValidationAction::DispatchAccepted,
             }));
    QCOMPARE(actionsOf(cases.at(7)), establishVt04
             + QList<ShortageValidationAction>({
                 ShortageValidationAction::DispatchAccepted,
                 ShortageValidationAction::MaterialUnloaded,
                 ShortageValidationAction::TaskSucceeded,
             }));
    QCOMPARE(actionsOf(cases.at(8)), establishVt04
             + QList<ShortageValidationAction>({
                 ShortageValidationAction::DispatchAccepted,
                 ShortageValidationAction::FailureBeforeUnload,
                 ShortageValidationAction::DispatchAccepted,
                 ShortageValidationAction::FailureBeforeUnload,
             }));
    QCOMPARE(actionsOf(cases.at(9)), establishVt04
             + QList<ShortageValidationAction>({
                 ShortageValidationAction::DispatchAccepted,
                 ShortageValidationAction::MaterialUnloaded,
                 ShortageValidationAction::FailureAfterUnload,
             }));
    QCOMPARE(actionsOf(cases.at(10)), establishVt04
             + QList<ShortageValidationAction>({
                 ShortageValidationAction::DispatchAccepted,
                 ShortageValidationAction::MaterialUnloaded,
                 ShortageValidationAction::ResendUnload,
             }));
    QCOMPARE(actionsOf(cases.at(13)),
             QList<ShortageValidationAction>({
                 ShortageValidationAction::SaveState,
                 ShortageValidationAction::SimulateRestart,
                 ShortageValidationAction::ReloadState,
                 ShortageValidationAction::ClearTestState,
             }));
    QCOMPARE(actionsOf(cases.at(14)),
             QList<ShortageValidationAction>({
                 ShortageValidationAction::SelectFieldSource,
                 ShortageValidationAction::StartFieldSampling,
                 ShortageValidationAction::SelectManualSource,
                 ShortageValidationAction::ApplyManualSample,
                 ShortageValidationAction::ShowInstruction,
             }));
}

void ShortageValidationDialogTest::nextStepUsesOnlyTestControllerPublicActions()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ShortageTestController controller(validationTestConfiguration(), directory.path(), nullptr,
                                      [] {
                                          return ShortageOperationResult {
                                              true, QStringLiteral("允许验证采样")};
                                      });
    ShortageValidationDialog dialog(&controller);
    auto *caseList = requiredChild<QListWidget>(&dialog, "validationCaseList");
    auto *nextButton = requiredChild<QPushButton>(&dialog, "validationNextStepButton");
    QSignalSpy snapshotSpy(&controller, &ShortageTestController::snapshotChanged);

    caseList->setCurrentRow(3);
    for (int step = 0; step < 4; ++step)
        nextButton->click();

    QVERIFY(snapshotSpy.count() >= 3);
    QCOMPARE(controller.stateNamespaceForTest(), ShortageStateNamespace::StandaloneTest);
    const ShortageRuntimeState state = controller.currentSnapshot().runtime;
    QVERIFY(state.initialized);
    QCOMPARE(state.orders.size(), 1);
    QCOMPARE(state.orders.first().state, ReplenishmentOrderState::AwaitingDispatch);
    QVERIFY(!QFile::exists(QDir(directory.path()).filePath(QStringLiteral("production-state.json"))));
}

void ShortageValidationDialogTest::automaticCaseShowsActualSnapshotEvidence()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ShortageTestController controller(validationTestConfiguration(), directory.path(), nullptr,
                                      [] {
                                          return ShortageOperationResult {
                                              true, QStringLiteral("允许验证采样")};
                                      });
    ShortageValidationDialog dialog(&controller);
    auto *caseList = requiredChild<QListWidget>(&dialog, "validationCaseList");
    auto *nextButton = requiredChild<QPushButton>(&dialog, "validationNextStepButton");
    auto *evidence = requiredChild<QLabel>(&dialog, "validationSnapshotEvidenceLabel");
    auto *status = requiredChild<QLabel>(&dialog, "validationStatusLabel");
    auto *log = requiredChild<QPlainTextEdit>(&dialog, "validationLogEdit");

    caseList->setCurrentRow(3);
    for (int step = 0; step < 4; ++step)
        nextButton->click();

    QVERIFY(status->text().contains(QStringLiteral("自动通过")));
    QVERIFY(evidence->text().contains(QStringLiteral("产品/模式")));
    QVERIFY(evidence->text().contains(QStringLiteral("actualQty/基线")));
    QVERIFY(evidence->text().contains(QStringLiteral("当前补料单")));
    QVERIFY(evidence->text().contains(QStringLiteral("最近增量")));
    QVERIFY(log->toPlainText().contains(QStringLiteral("预期")));
    QVERIFY(log->toPlainText().contains(QStringLiteral("实际")));
    QVERIFY(log->toPlainText().contains(QStringLiteral("VT-04")));
}

void ShortageValidationDialogTest::fieldEvidenceWaitsForManualConfirmation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ShortageTestController controller(validationTestConfiguration(), directory.path(), nullptr,
                                      [] {
                                          return ShortageOperationResult {
                                              true, QStringLiteral("允许验证采样")};
                                      });
    ShortageValidationDialog dialog(&controller);
    auto *caseList = requiredChild<QListWidget>(&dialog, "validationCaseList");
    auto *nextButton = requiredChild<QPushButton>(&dialog, "validationNextStepButton");
    auto *confirmButton = requiredChild<QPushButton>(&dialog, "validationManualConfirmButton");
    auto *status = requiredChild<QLabel>(&dialog, "validationStatusLabel");

    caseList->setCurrentRow(0);
    for (int step = 0; step < 5; ++step)
        nextButton->click();

    QVERIFY(status->text().contains(QStringLiteral("等待人工确认")));
    QVERIFY(confirmButton->isEnabled());
    confirmButton->click();
    QVERIFY(status->text().contains(QStringLiteral("人工确认通过")));
}

void ShortageValidationDialogTest::sourceFilesContainNoProductionOrHardwareDependency()
{
    const QString header = readProjectFile(QStringLiteral("src/shortagevalidationdialog.h"));
    const QString source = readProjectFile(QStringLiteral("src/shortagevalidationdialog.cpp"));
    QVERIFY2(!header.isEmpty(), "shortagevalidationdialog.h must be readable");
    QVERIFY2(!source.isEmpty(), "shortagevalidationdialog.cpp must be readable");
    const QString text = header + QLatin1Char('\n') + source;

    const QStringList forbidden {
        QStringLiteral("ShortageEngine"),
        QStringLiteral("LineManager"),
        QStringLiteral("TaskQueue"),
        QStringLiteral("AgvController"),
        QStringLiteral("HuayanScheduler"),
        QStringLiteral("CustomSysScheduler"),
    };
    for (const QString &token : forbidden) {
        QVERIFY2(!text.contains(token),
                 qPrintable(QStringLiteral("独立验证窗口不得依赖 %1").arg(token)));
    }
}

QTEST_MAIN(ShortageValidationDialogTest)
#include "test_shortage_validation_dialog.moc"

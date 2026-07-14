#include "shortagevalidationdialog.h"

#include <QDateTime>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

#include <algorithm>

namespace {

ShortageValidationStep step(const QString &instructionZh,
                            const QString &expectedZh,
                            ShortageValidationAction action = ShortageValidationAction::ShowInstruction,
                            ProductModel product = ProductModel::Model88,
                            ProductionMode mode = ProductionMode::LeftRight,
                            qint64 actualQty = 0)
{
    ShortageValidationStep item;
    item.instructionZh = instructionZh;
    item.expectedZh = expectedZh;
    item.action = action;
    item.product = product;
    item.mode = mode;
    item.actualQty = actualQty;
    return item;
}

ShortageValidationCase validationCase(const QString &id,
                                      const QString &nameZh,
                                      QList<ShortageValidationStep> steps,
                                      const QString &passCriteriaZh,
                                      bool requiresManualEvidence)
{
    ShortageValidationCase item;
    item.id = id;
    item.nameZh = nameZh;
    item.steps = std::move(steps);
    item.passCriteriaZh = passCriteriaZh;
    item.requiresManualEvidence = requiresManualEvidence;
    return item;
}

QString yesNo(bool value)
{
    return value ? QStringLiteral("是") : QStringLiteral("否");
}

const ShortageStationRuntime *stationById(const ShortageRuntimeState &state, int stationId)
{
    for (const ShortageStationRuntime &station : state.stations) {
        if (station.stationId == stationId)
            return &station;
    }
    return nullptr;
}

const ReplenishmentOrder *orderByNo(const ShortageRuntimeState &state, quint64 orderNo)
{
    for (const ReplenishmentOrder &order : state.orders) {
        if (order.orderNo == orderNo)
            return &order;
    }
    return nullptr;
}

bool hasTwelveStations(const ShortageRuntimeState &state)
{
    if (state.stations.size() != 12)
        return false;
    for (int stationId = 1; stationId <= 12; ++stationId) {
        if (stationById(state, stationId) == nullptr)
            return false;
    }
    return true;
}

bool allStationStocksEqual(const ShortageRuntimeState &state, qint64 expectedStock)
{
    if (!hasTwelveStations(state))
        return false;
    for (const ShortageStationRuntime &station : state.stations) {
        if (station.stock != expectedStock)
            return false;
    }
    return true;
}

bool waitingStationsAreOneToTwelve(const ShortageRuntimeState &state)
{
    if (state.waitingStationIds.size() != 12)
        return false;
    for (int index = 0; index < state.waitingStationIds.size(); ++index) {
        if (state.waitingStationIds.at(index) != index + 1)
            return false;
    }
    return true;
}

bool hasSingleAwaitingStationOneOrder(const ShortageRuntimeState &state)
{
    return state.orders.size() == 1
        && state.orders.first().stationId == 1
        && state.orders.first().state == ReplenishmentOrderState::AwaitingDispatch
        && state.orders.first().taskId == 0;
}

bool stockIncreasedOnlyForStation(const ShortageRuntimeState &before,
                                  const ShortageRuntimeState &after,
                                  int stationId)
{
    if (!hasTwelveStations(before) || !hasTwelveStations(after))
        return false;
    bool targetIncreased = false;
    for (const ShortageStationRuntime &afterStation : after.stations) {
        const ShortageStationRuntime *beforeStation = stationById(before, afterStation.stationId);
        if (beforeStation == nullptr)
            return false;
        if (afterStation.stationId == stationId) {
            targetIncreased = afterStation.stock > beforeStation->stock;
        } else if (afterStation.stock != beforeStation->stock) {
            return false;
        }
    }
    return targetIncreased;
}

bool stockUnchangedForAllStations(const ShortageRuntimeState &before,
                                  const ShortageRuntimeState &after)
{
    if (!hasTwelveStations(before) || !hasTwelveStations(after))
        return false;
    for (const ShortageStationRuntime &afterStation : after.stations) {
        const ShortageStationRuntime *beforeStation = stationById(before, afterStation.stationId);
        if (beforeStation == nullptr || afterStation.stock != beforeStation->stock)
            return false;
    }
    return true;
}

const ShortageStationConfig *configForStation(const ShortageConfiguration &configuration,
                                              ProductModel product,
                                              int stationId)
{
    for (const ShortageStationConfig &station : configuration.stations) {
        if (station.product == product && station.stationId == stationId)
            return &station;
    }
    return nullptr;
}

qint64 usageForMode(const ShortageStationConfig &station, ProductionMode mode)
{
    switch (mode) {
    case ProductionMode::LeftRight:
        return station.usageLeftRight;
    case ProductionMode::LeftOnly:
        return station.usageLeftOnly;
    case ProductionMode::RightOnly:
        return station.usageRightOnly;
    }
    return 0;
}

int normalizedPreUnloadFailureLimit(int preUnloadFailureLimit)
{
    return std::clamp(preUnloadFailureLimit, 1, 20);
}

QList<ShortageValidationStep> vt09Steps(int preUnloadFailureLimit)
{
    const int failureLimit = normalizedPreUnloadFailureLimit(preUnloadFailureLimit);
    QList<ShortageValidationStep> steps {
        step(QStringLiteral("清空、0 建账并提交首样本。"), QStringLiteral("形成待派单。"),
             ShortageValidationAction::ClearTestState),
        step(QStringLiteral("确认从 0 建账。"), QStringLiteral("库存为 0。"),
             ShortageValidationAction::InitializeZero),
        step(QStringLiteral("切换为手工源。"), QStringLiteral("使用公开手工测试入口。"),
             ShortageValidationAction::SelectManualSource),
        step(QStringLiteral("提交首样本。"), QStringLiteral("生成待派单。"),
             ShortageValidationAction::ApplyManualSample,
             ProductModel::Model88, ProductionMode::LeftRight, 100),
    };
    for (int attempt = 1; attempt <= failureLimit; ++attempt) {
        steps.append(step(QStringLiteral("第 %1 次接受补料单。").arg(attempt),
                          attempt == 1 ? QStringLiteral("任务运行。")
                                       : QStringLiteral("重试原工位。"),
                          ShortageValidationAction::DispatchAccepted));
        steps.append(step(QStringLiteral("第 %1 次倒料前失败。").arg(attempt),
                          attempt == failureLimit
                              ? QStringLiteral("达到配置阈值后暂停目标工位。")
                              : QStringLiteral("不增加库存并继续重试。"),
                          ShortageValidationAction::FailureBeforeUnload));
    }
    return steps;
}

bool configuredProductionDeductionMatches(const ShortageRuntimeState &before,
                                          const ShortageRuntimeState &after,
                                          const ShortageConfiguration &configuration,
                                          qint64 productionDelta,
                                          QStringList *evidenceRows)
{
    if (!hasTwelveStations(before) || !hasTwelveStations(after))
        return false;

    bool matched = true;
    bool sawEnabledStation = false;
    for (int stationId = 1; stationId <= 12; ++stationId) {
        const ShortageStationRuntime *beforeStation = stationById(before, stationId);
        const ShortageStationRuntime *afterStation = stationById(after, stationId);
        const ShortageStationConfig *stationConfig =
            configForStation(configuration, after.product, stationId);
        if (beforeStation == nullptr || afterStation == nullptr || stationConfig == nullptr) {
            matched = false;
            continue;
        }

        qint64 usage = 0;
        qint64 expectedStock = beforeStation->stock;
        if (stationConfig->enabled) {
            sawEnabledStation = true;
            usage = usageForMode(*stationConfig, after.mode);
            expectedStock = beforeStation->stock - productionDelta * usage;
        }

        if (afterStation->stock != expectedStock)
            matched = false;
        if (evidenceRows != nullptr) {
            evidenceRows->append(QStringLiteral("工位%1：旧库存=%2，用量=%3，预期=%4，实际=%5")
                                     .arg(stationId)
                                     .arg(beforeStation->stock)
                                     .arg(usage)
                                     .arg(expectedStock)
                                     .arg(afterStation->stock));
        }
    }

    return matched && sawEnabledStation;
}

bool hasAwaitingAutomaticOrderForStation(const ShortageRuntimeState &state, int stationId)
{
    for (const ReplenishmentOrder &order : state.orders) {
        if (order.stationId == stationId
            && order.origin == ReplenishmentOrigin::Automatic
            && order.state == ReplenishmentOrderState::AwaitingDispatch) {
            return true;
        }
    }
    return false;
}

} // namespace

ShortageValidationDialog::ShortageValidationDialog(ShortageTestController *testController,
                                                   QWidget *parent)
    : QDialog(parent),
      m_testController(testController),
      m_validationConfiguration(testController != nullptr
                                    ? testController->engineForTest().configuration()
                                    : ShortageConfiguration {}),
      m_cases(createValidationCases(
          normalizedPreUnloadFailureLimit(m_validationConfiguration.parameters.preUnloadFailureLimit)))
{
    setWindowTitle(QStringLiteral("独立缺料验证控制台"));
    setWindowFlags(Qt::Window | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint
                   | Qt::WindowCloseButtonHint);
    setModal(false);
    setMinimumSize(1280, 760);

    buildUi();

    if (m_testController != nullptr) {
        // snapshotChanged 是验证窗口接收测试状态证据的唯一连接点；只读刷新，不回写控制器。
        connect(m_testController, &ShortageTestController::snapshotChanged, this,
                [this](const ShortageUiSnapshot &snapshot) {
                    m_latestSnapshot = snapshot;
                    refreshSnapshotEvidence(snapshot);
                });
        // actionAvailabilityChanged 是验证窗口感知按钮能力变化的唯一连接点；当前向导只记录证据。
        connect(m_testController, &ShortageTestController::actionAvailabilityChanged, this,
                [this](const ShortageTestActionAvailability &availability) {
                    const QString messageZh = QStringLiteral(
                        "动作门禁刷新：手工=%1，现场启动=%2，停止=%3，接受=%4，拒收=%5，倒料=%6，重发=%7")
                                                  .arg(yesNo(availability.canSubmitManualSample))
                                                  .arg(yesNo(availability.canStartFieldSampling))
                                                  .arg(yesNo(availability.canStopFieldSampling))
                                                  .arg(yesNo(availability.canAcceptDispatch))
                                                  .arg(yesNo(availability.canRejectDispatch))
                                                  .arg(yesNo(availability.canRecordUnload))
                                                  .arg(yesNo(availability.canResendUnload));
                    m_currentCaseControllerLogs.append(messageZh);
                    appendValidationLog(messageZh);
                });
        // eventLogged 是测试控制器中文业务日志进入验证控制台的唯一连接点。
        connect(m_testController, &ShortageTestController::eventLogged, this,
                [this](const QString &messageZh) {
                    m_currentCaseControllerLogs.append(messageZh);
                    appendValidationLog(QStringLiteral("控制器日志：%1").arg(messageZh));
                });
        // operationRejected 是测试控制器拒绝原因进入验证控制台的唯一连接点。
        connect(m_testController, &ShortageTestController::operationRejected, this,
                [this](const QString &reasonZh) {
                    m_currentCaseRejections.append(reasonZh);
                    appendValidationLog(QStringLiteral("控制器拒绝：%1").arg(reasonZh));
                });
        m_latestSnapshot = m_testController->currentSnapshot();
        refreshSnapshotEvidence(m_latestSnapshot);
    } else {
        m_latestSnapshot = {};
        refreshSnapshotEvidence({});
    }

    if (!m_cases.isEmpty())
        m_caseList->setCurrentRow(0);
}

void ShortageValidationDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    auto *top = new QHBoxLayout;
    root->addLayout(top, 1);

    auto *caseGroup = new QGroupBox(QStringLiteral("十五项验证"), this);
    auto *caseLayout = new QVBoxLayout(caseGroup);
    m_caseList = new QListWidget(caseGroup);
    m_caseList->setObjectName(QStringLiteral("validationCaseList"));
    for (const ShortageValidationCase &item : std::as_const(m_cases))
        m_caseList->addItem(QStringLiteral("%1 %2").arg(item.id, item.nameZh));
    caseLayout->addWidget(m_caseList);
    top->addWidget(caseGroup, 1);

    auto *stepGroup = new QGroupBox(QStringLiteral("验证步骤和通过标准"), this);
    auto *stepLayout = new QVBoxLayout(stepGroup);
    m_statusLabel = new QLabel(QStringLiteral("状态：尚未开始"), stepGroup);
    m_statusLabel->setObjectName(QStringLiteral("validationStatusLabel"));
    stepLayout->addWidget(m_statusLabel);
    m_stepText = new QTextEdit(stepGroup);
    m_stepText->setObjectName(QStringLiteral("validationStepTextEdit"));
    m_stepText->setReadOnly(true);
    stepLayout->addWidget(m_stepText, 1);
    auto *buttonLayout = new QHBoxLayout;
    m_nextStepButton = new QPushButton(QStringLiteral("执行下一步"), stepGroup);
    m_nextStepButton->setObjectName(QStringLiteral("validationNextStepButton"));
    m_manualConfirmButton = new QPushButton(QStringLiteral("人工确认现场证据通过"), stepGroup);
    m_manualConfirmButton->setObjectName(QStringLiteral("validationManualConfirmButton"));
    buttonLayout->addWidget(m_nextStepButton);
    buttonLayout->addWidget(m_manualConfirmButton);
    stepLayout->addLayout(buttonLayout);
    top->addWidget(stepGroup, 2);

    auto *evidenceGroup = new QGroupBox(QStringLiteral("实际状态证据"), this);
    auto *evidenceLayout = new QVBoxLayout(evidenceGroup);
    m_snapshotEvidenceLabel = new QLabel(evidenceGroup);
    m_snapshotEvidenceLabel->setObjectName(QStringLiteral("validationSnapshotEvidenceLabel"));
    m_snapshotEvidenceLabel->setWordWrap(true);
    m_snapshotEvidenceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    evidenceLayout->addWidget(m_snapshotEvidenceLabel, 1);
    top->addWidget(evidenceGroup, 2);

    auto *logGroup = new QGroupBox(QStringLiteral("验证日志"), this);
    auto *logLayout = new QVBoxLayout(logGroup);
    m_logEdit = new QPlainTextEdit(logGroup);
    m_logEdit->setObjectName(QStringLiteral("validationLogEdit"));
    m_logEdit->setReadOnly(true);
    logLayout->addWidget(m_logEdit);
    root->addWidget(logGroup, 1);

    // caseList currentRowChanged 是验证项切换的唯一连接点；切换只重置显示和步骤游标。
    connect(m_caseList, &QListWidget::currentRowChanged, this,
            &ShortageValidationDialog::selectCase);
    // nextStepButton clicked 是执行验证动作的唯一连接点；动作再统一经 switch 分发。
    connect(m_nextStepButton, &QPushButton::clicked, this,
            &ShortageValidationDialog::executeNextStep);
    // manualConfirmButton clicked 是人工结论转换的唯一连接点；禁止跳过等待状态。
    connect(m_manualConfirmButton, &QPushButton::clicked, this,
            &ShortageValidationDialog::confirmManualEvidence);

    refreshStatusLabel();
}

void ShortageValidationDialog::selectCase(int row)
{
    if (row < 0 || row >= m_cases.size()) {
        m_currentCaseIndex = -1;
        m_currentStepIndex = 0;
        m_status = ShortageValidationStatus::NotStarted;
        m_currentCaseSnapshots.clear();
        m_currentCaseControllerLogs.clear();
        m_currentCaseRejections.clear();
        refreshCaseDetails();
        refreshStatusLabel();
        return;
    }

    m_currentCaseIndex = row;
    m_currentStepIndex = 0;
    m_status = ShortageValidationStatus::NotStarted;
    m_currentCaseSnapshots.clear();
    m_currentCaseControllerLogs.clear();
    m_currentCaseRejections.clear();
    refreshCaseDetails();
    refreshStatusLabel();
    appendValidationLog(QStringLiteral("已选择验证项：%1 %2")
                            .arg(m_cases.at(row).id, m_cases.at(row).nameZh));
}

void ShortageValidationDialog::executeNextStep()
{
    if (m_currentCaseIndex < 0 || m_currentCaseIndex >= m_cases.size())
        return;

    const ShortageValidationCase &item = m_cases.at(m_currentCaseIndex);
    if (m_currentStepIndex >= item.steps.size()) {
        appendValidationLog(QStringLiteral("验证步骤未执行：%1 已无待执行步骤").arg(item.id));
        return;
    }

    if (m_testController == nullptr) {
        appendValidationLog(QStringLiteral("验证步骤未执行：测试控制器不可用"));
        m_status = ShortageValidationStatus::FailedAutomatically;
        refreshStatusLabel();
        return;
    }

    const int displayStep = m_currentStepIndex + 1;
    const ShortageValidationStep &step = item.steps.at(m_currentStepIndex);
    m_status = ShortageValidationStatus::InProgress;
    refreshStatusLabel();

    switch (step.action) {
    case ShortageValidationAction::ShowInstruction:
        break;
    case ShortageValidationAction::ClearTestState:
        m_testController->clearTestStateAfterConfirmation();
        break;
    case ShortageValidationAction::InitializeZero:
        m_testController->initializeZeroAfterConfirmation();
        break;
    case ShortageValidationAction::SelectManualSource:
        m_testController->selectInputSource(ShortageTestInputSource::Manual);
        break;
    case ShortageValidationAction::SelectFieldSource:
        m_testController->selectInputSource(ShortageTestInputSource::Field);
        break;
    case ShortageValidationAction::ApplyManualSample:
        m_testController->applyManualSample(step.product, step.mode, step.actualQty);
        break;
    case ShortageValidationAction::StartFieldSampling:
        m_testController->startFieldSampling();
        break;
    case ShortageValidationAction::StopFieldSampling:
        m_testController->stop();
        break;
    case ShortageValidationAction::DispatchRejected:
        m_testController->simulateDispatchRejected();
        break;
    case ShortageValidationAction::DispatchAccepted:
        m_testController->simulateDispatchAccepted();
        break;
    case ShortageValidationAction::FailureBeforeUnload:
        m_testController->simulateFailureBeforeUnload();
        break;
    case ShortageValidationAction::MaterialUnloaded:
        m_testController->simulateMaterialUnloaded();
        break;
    case ShortageValidationAction::FailureAfterUnload:
        m_testController->simulateFailureAfterUnload();
        break;
    case ShortageValidationAction::TaskSucceeded:
        m_testController->simulateTaskSucceeded();
        break;
    case ShortageValidationAction::ResendUnload:
        m_testController->resendLastUnloadFact();
        break;
    case ShortageValidationAction::SaveState:
        m_testController->saveTestState();
        break;
    case ShortageValidationAction::ReloadState:
        m_testController->reloadTestState();
        break;
    case ShortageValidationAction::SimulateRestart:
        m_testController->simulateRestart();
        break;
    }

    const ShortageUiSnapshot snapshot = m_latestSnapshot;
    m_currentCaseSnapshots.append(snapshot);
    refreshSnapshotEvidence(snapshot);
    appendValidationLog(QStringLiteral(
                            "步骤完成：输入=%1，预期=%2，实际=%3")
                            .arg(step.instructionZh,
                                 step.expectedZh,
                                 m_snapshotEvidenceLabel->text().simplified()));

    ++m_currentStepIndex;
    if (m_currentStepIndex >= item.steps.size()) {
        if (item.requiresManualEvidence) {
            QString evidenceZh;
            if (!evaluateManualLocalCriteria(item.id, &evidenceZh)) {
                m_status = ShortageValidationStatus::FailedAutomatically;
            } else {
                if (!evidenceZh.isEmpty())
                    appendValidationLog(evidenceZh);
                m_status = ShortageValidationStatus::WaitingManualEvidence;
            }
        } else {
            evaluateCurrentCase(snapshot);
        }
    } else {
        m_status = ShortageValidationStatus::InProgress;
    }
    appendValidationLog(QStringLiteral("%1 第 %2/%3 步结束")
                            .arg(item.id)
                            .arg(displayStep)
                            .arg(item.steps.size()));
    refreshCaseDetails();
    refreshStatusLabel();
}

void ShortageValidationDialog::evaluateCurrentCase(const ShortageUiSnapshot &snapshot)
{
    if (m_currentCaseIndex < 0 || m_currentCaseIndex >= m_cases.size()) {
        m_status = ShortageValidationStatus::FailedAutomatically;
        return;
    }

    const QString id = m_cases.at(m_currentCaseIndex).id;
    const ShortageRuntimeState &state = snapshot.runtime;
    bool passed = false;
    QString evidenceZh;

    if (id == QStringLiteral("VT-04")) {
        passed = state.initialized
                 && state.operatorConfirmedRestore
                 && state.hasStableContext
                 && state.actualQty.hasBaseline
                 && state.actualQty.baseline == 100
                 && allStationStocksEqual(state, 0)
                 && waitingStationsAreOneToTwelve(state)
                 && state.activeStationId == 1
                 && hasSingleAwaitingStationOneOrder(state)
                 && !state.criticalLock;
        evidenceZh = passed
                     ? QStringLiteral("VT-04 自动判定通过：12工位均为0，活动工位=1，唯一待派单=工位1，基线=100。")
                     : QStringLiteral("VT-04 自动判定失败：未同时满足12工位为0、基线100、等待顺序、活动工位1和唯一待派单。");
    } else if (id == QStringLiteral("VT-06")) {
        const bool hasBeforeSample = m_currentCaseSnapshots.size() >= 5;
        bool stocksDeducted = false;
        QStringList stationEvidence;
        if (hasBeforeSample) {
            const ShortageRuntimeState &beforeDelta = m_currentCaseSnapshots.at(3).runtime;
            stocksDeducted = configuredProductionDeductionMatches(beforeDelta,
                                                                  state,
                                                                  m_validationConfiguration,
                                                                  5,
                                                                  &stationEvidence);
        }
        passed = state.initialized && snapshot.hasLastProductionDelta
                 && snapshot.lastProductionDelta == 5
                 && state.actualQty.hasBaseline
                 && state.actualQty.baseline == 105
                 && stocksDeducted
                 && !state.criticalLock;
        evidenceZh = passed
                     ? QStringLiteral("VT-06 自动判定通过：基线=105，最近增量=5；%1。")
                           .arg(stationEvidence.join(QStringLiteral("；")))
                     : QStringLiteral("VT-06 自动判定失败：缺少基线105、最近增量5或逐工位配置用量扣减证据；%1。")
                           .arg(stationEvidence.join(QStringLiteral("；")));
    } else if (id == QStringLiteral("VT-07")) {
        const bool hasSnapshots = m_currentCaseSnapshots.size() >= 6;
        if (hasSnapshots) {
            const ShortageRuntimeState &beforeReject = m_currentCaseSnapshots.at(3).runtime;
            const ShortageRuntimeState &afterReject = m_currentCaseSnapshots.at(4).runtime;
            const ShortageRuntimeState &afterAccept = m_currentCaseSnapshots.at(5).runtime;
            if (beforeReject.orders.size() == 1) {
                const quint64 orderNo = beforeReject.orders.first().orderNo;
                const ReplenishmentOrder *rejectedOrder = orderByNo(afterReject, orderNo);
                const ReplenishmentOrder *acceptedOrder = orderByNo(afterAccept, orderNo);
                passed = rejectedOrder != nullptr
                         && acceptedOrder != nullptr
                         && afterReject.orders.size() == 1
                         && rejectedOrder->state == ReplenishmentOrderState::AwaitingDispatch
                         && !rejectedOrder->lastReasonZh.isEmpty()
                         && afterAccept.orders.size() == 1
                         && acceptedOrder->state == ReplenishmentOrderState::Running
                         && acceptedOrder->taskId >= 900000000000ULL
                         && !afterAccept.criticalLock;
            }
        }
        evidenceZh = passed
                     ? QStringLiteral("VT-07 自动判定通过：拒收后原单号保持且不生成重复单，接受后绑定测试taskId并进入运行中。")
                     : QStringLiteral("VT-07 自动判定失败：拒收原单、重复单或运行中taskId证据不完整。");
    } else if (id == QStringLiteral("VT-08")) {
        passed = false;
        evidenceZh = QStringLiteral("VT-08 自动判定失败：连续补料和最高位释放切换需要现场/额外步骤证据，本项不报告纯自动通过。");
    } else if (id == QStringLiteral("VT-09")) {
        const bool hasSnapshots = m_currentCaseSnapshots.size() >= 8;
        qint64 originalStock = 0;
        qint64 firstFailureStock = 0;
        qint64 finalStock = 0;
        int failureCount = 0;
        QString actionZh;
        if (hasSnapshots) {
            const ShortageRuntimeState &beforeFailures = m_currentCaseSnapshots.at(3).runtime;
            const ShortageRuntimeState &afterFirstFailure = m_currentCaseSnapshots.at(5).runtime;
            const ShortageStationRuntime *beforeStation = stationById(beforeFailures, 1);
            const ShortageStationRuntime *afterFirstStation = stationById(afterFirstFailure, 1);
            const ShortageStationRuntime *targetStation = stationById(state, 1);
            bool othersContinue = false;
            for (int stationId : state.waitingStationIds)
                othersContinue = othersContinue || stationId != 1;
            othersContinue = othersContinue || (state.activeStationId != 0
                                                && state.activeStationId != 1);
            if (beforeStation != nullptr)
                originalStock = beforeStation->stock;
            if (afterFirstStation != nullptr)
                firstFailureStock = afterFirstStation->stock;
            if (targetStation != nullptr) {
                finalStock = targetStation->stock;
                failureCount = targetStation->consecutivePreUnloadFailures;
                actionZh = targetStation->pauseReasonZh;
            }
            passed = beforeStation != nullptr
                     && afterFirstStation != nullptr
                     && targetStation != nullptr
                     && afterFirstStation->stock == beforeStation->stock
                     && targetStation->stock == beforeStation->stock
                     && targetStation->automaticPaused
                     && targetStation->consecutivePreUnloadFailures
                            >= normalizedPreUnloadFailureLimit(
                                m_validationConfiguration.parameters.preUnloadFailureLimit)
                     && !targetStation->pauseReasonZh.isEmpty()
                     && othersContinue
                     && !state.criticalLock;
        }
        evidenceZh = passed
                     ? QStringLiteral("VT-09 自动判定通过：工位=1，原库存=%1，首次失败后库存=%2，当前库存=%3，失败次数=%4，处理动作=%5；暂停目标工位且其他工位继续计划。")
                           .arg(originalStock)
                           .arg(firstFailureStock)
                           .arg(finalStock)
                           .arg(failureCount)
                           .arg(actionZh)
                     : QStringLiteral("VT-09 自动判定失败：工位=1，原库存=%1，首次失败后库存=%2，当前库存=%3，失败次数=%4，处理动作=%5；未证明失败不加库存、达到阈值暂停目标工位和其他工位继续计划。")
                           .arg(originalStock)
                           .arg(firstFailureStock)
                           .arg(finalStock)
                           .arg(failureCount)
                           .arg(actionZh.isEmpty() ? QStringLiteral("无") : actionZh);
    } else if (id == QStringLiteral("VT-10")) {
        const bool hasSnapshots = m_currentCaseSnapshots.size() >= 7;
        if (hasSnapshots) {
            const ShortageRuntimeState &afterUnload = m_currentCaseSnapshots.at(5).runtime;
            const ShortageRuntimeState &afterFailure = m_currentCaseSnapshots.at(6).runtime;
            const ShortageStationRuntime *targetStation = stationById(afterFailure, 1);
            passed = stockUnchangedForAllStations(afterUnload, afterFailure)
                     && targetStation != nullptr
                     && targetStation->consecutivePreUnloadFailures == 0
                     && afterFailure.orders.size() == 1
                     && afterFailure.orders.first().state == ReplenishmentOrderState::FailedAfterUnload
                     && afterFailure.orders.first().unloadAccounted
                     && !afterFailure.criticalLock;
        }
        evidenceZh = passed
                     ? QStringLiteral("VT-10 自动判定通过：倒料后失败终态已记录，已倒料库存不回滚，失败计数不增加。")
                     : QStringLiteral("VT-10 自动判定失败：缺少倒料后失败、库存不回滚或失败计数不增加证据。");
    } else if (id == QStringLiteral("VT-11")) {
        const bool hasSnapshots = m_currentCaseSnapshots.size() >= 7;
        quint64 orderNo = 0;
        quint64 taskId = 0;
        int stationId = 0;
        QString duplicateActionZh;
        if (hasSnapshots) {
            const ShortageRuntimeState &afterUnload = m_currentCaseSnapshots.at(5).runtime;
            if (!afterUnload.orders.isEmpty()) {
                orderNo = afterUnload.orders.first().orderNo;
                taskId = afterUnload.orders.first().taskId;
                stationId = afterUnload.orders.first().stationId;
            }
            duplicateActionZh = state.criticalReasonZh.contains(QStringLiteral("处理动作=严重锁定且不加库存"))
                                    ? QStringLiteral("严重锁定且不加库存")
                                    : state.criticalReasonZh;
            const bool noAwaitingAutomatic = !hasAwaitingAutomaticOrderForStation(state, stationId);
            passed = stockUnchangedForAllStations(afterUnload, state)
                     && state.criticalLock
                     && !state.criticalReasonZh.isEmpty()
                     && !state.orders.isEmpty()
                     && state.orders.first().unloadAccounted
                     && orderNo != 0
                     && taskId != 0
                     && stationId != 0
                     && duplicateActionZh == QStringLiteral("严重锁定且不加库存")
                     && noAwaitingAutomatic;
        }
        evidenceZh = passed
                     ? QStringLiteral("VT-11 自动判定通过：补料单号=%1，taskId=%2，工位=%3，重复倒料处理动作=%4；库存不第二次增加，系统进入严重锁定并停止新自动意图。")
                           .arg(orderNo)
                           .arg(taskId)
                           .arg(stationId)
                           .arg(duplicateActionZh)
                     : QStringLiteral("VT-11 自动判定失败：补料单号=%1，taskId=%2，工位=%3，重复倒料处理动作=%4；缺少库存幂等、严重锁定或停止新自动意图证据。")
                           .arg(orderNo)
                           .arg(taskId)
                           .arg(stationId)
                           .arg(duplicateActionZh.isEmpty() ? QStringLiteral("无") : duplicateActionZh);
    } else {
        passed = false;
        evidenceZh = QStringLiteral("%1 自动判定失败：本验证项没有已定义的本地自动判定规则。")
                         .arg(id);
    }

    m_status = passed ? ShortageValidationStatus::PassedAutomatically
                      : ShortageValidationStatus::FailedAutomatically;
    appendValidationLog(evidenceZh);
}

bool ShortageValidationDialog::evaluateManualLocalCriteria(const QString &caseId,
                                                           QString *evidenceZh) const
{
    if (caseId == QStringLiteral("VT-08")) {
        bool passed = false;
        QString evidence = QStringLiteral("VT-08 本地证据失败：缺少倒料前、倒料后或终态快照。");
        if (m_currentCaseSnapshots.size() >= 7) {
            const ShortageRuntimeState &beforeUnload = m_currentCaseSnapshots.at(4).runtime;
            const ShortageRuntimeState &afterUnload = m_currentCaseSnapshots.at(5).runtime;
            const ShortageRuntimeState &afterSuccess = m_currentCaseSnapshots.at(6).runtime;
            const ReplenishmentOrder *runningOrder =
                beforeUnload.orders.isEmpty() ? nullptr : &beforeUnload.orders.first();
            const int stationId = runningOrder != nullptr ? runningOrder->stationId : 0;
            const ShortageStationRuntime *beforeStation = stationById(beforeUnload, stationId);
            const ShortageStationRuntime *afterUnloadStation = stationById(afterUnload, stationId);
            const ShortageStationRuntime *afterSuccessStation = stationById(afterSuccess, stationId);
            const ShortageStationConfig *stationConfig =
                configForStation(m_validationConfiguration, afterSuccess.product, stationId);
            const qint64 oldStock = beforeStation != nullptr ? beforeStation->stock : 0;
            const qint64 unloadStock = afterUnloadStation != nullptr ? afterUnloadStation->stock : 0;
            const qint64 terminalStock = afterSuccessStation != nullptr ? afterSuccessStation->stock : 0;
            const qint64 boxQuantity = stationConfig != nullptr ? stationConfig->boxQuantity : 0;
            const bool exactBox = beforeStation != nullptr
                                  && afterUnloadStation != nullptr
                                  && stationConfig != nullptr
                                  && unloadStock == oldStock + boxQuantity
                                  && stockIncreasedOnlyForStation(beforeUnload,
                                                                  afterUnload,
                                                                  stationId);
            const bool terminalNoRepeat = afterSuccessStation != nullptr
                                          && terminalStock == unloadStock
                                          && stockUnchangedForAllStations(afterUnload, afterSuccess);
            const bool succeeded = afterSuccess.orders.size() == 1
                                   && afterSuccess.orders.first().stationId == stationId
                                   && afterSuccess.orders.first().state
                                          == ReplenishmentOrderState::Succeeded
                                   && afterSuccess.orders.first().unloadAccounted
                                   && !afterSuccess.criticalLock;
            const bool belowMaximum = stationConfig != nullptr
                                      && afterSuccessStation != nullptr
                                      && terminalStock < stationConfig->maximumStock;
            const bool nextBoxVisible = hasAwaitingAutomaticOrderForStation(afterSuccess, stationId);

            passed = exactBox && terminalNoRepeat && succeeded;
            evidence = passed
                ? QStringLiteral("VT-08 本地证据通过：工位=%1，补料单号=%2，taskId=%3，原库存=%4，配置箱数=%5，倒料后库存=%6，终态库存=%7；未达最高位=%8，下一箱可见=%9，释放切换=未执行，处理动作=保留人工现场证据。")
                      .arg(stationId)
                      .arg(runningOrder != nullptr ? runningOrder->orderNo : 0)
                      .arg(runningOrder != nullptr ? runningOrder->taskId : 0)
                      .arg(oldStock)
                      .arg(boxQuantity)
                      .arg(unloadStock)
                      .arg(terminalStock)
                      .arg(yesNo(belowMaximum))
                      .arg(yesNo(nextBoxVisible))
                : QStringLiteral("VT-08 本地证据失败：工位=%1，原库存=%2，配置箱数=%3，倒料后库存=%4，终态库存=%5；未证明准确一箱、终态不重复加箱或成功终态。")
                      .arg(stationId)
                      .arg(oldStock)
                      .arg(boxQuantity)
                      .arg(unloadStock)
                      .arg(terminalStock);
        }
        if (evidenceZh != nullptr)
            *evidenceZh = evidence;
        return passed;
    }

    if (caseId != QStringLiteral("VT-15"))
        return true;

    const bool sawFieldSampling =
        std::any_of(m_currentCaseSnapshots.cbegin(), m_currentCaseSnapshots.cend(),
                    [](const ShortageUiSnapshot &snapshot) {
                        return snapshot.inputSource == ShortageInputSource::Live
                            && snapshot.communication == ShortageCommunicationState::Sampling;
                    });
    const bool sawManualDisabledWhileRunning =
        std::any_of(m_currentCaseControllerLogs.cbegin(), m_currentCaseControllerLogs.cend(),
                    [](const QString &messageZh) {
                        return messageZh.contains(QStringLiteral("手工=否"))
                            && messageZh.contains(QStringLiteral("停止=是"));
                    });
    const ShortageUiSnapshot finalSnapshot =
        m_currentCaseSnapshots.isEmpty() ? ShortageUiSnapshot {} : m_currentCaseSnapshots.last();
    const bool stoppedAfterSwitch =
        finalSnapshot.inputSource == ShortageInputSource::Mock
        && finalSnapshot.communication == ShortageCommunicationState::Stopped;

    const bool passed = sawFieldSampling && sawManualDisabledWhileRunning && stoppedAfterSwitch;
    if (evidenceZh != nullptr) {
        *evidenceZh = passed
            ? QStringLiteral("VT-15 本地门禁判定通过：现场采样运行中手工动作门禁禁用，停止后可切回手工源；直接调用拒绝原因仍需人工复核。")
            : QStringLiteral("VT-15 本地门禁判定失败：缺少现场采样运行、手工动作禁用或停止后切回手工源证据。");
    }
    return passed;
}

void ShortageValidationDialog::confirmManualEvidence()
{
    if (m_status != ShortageValidationStatus::WaitingManualEvidence) {
        appendValidationLog(QStringLiteral("人工确认被拒绝：当前状态不是等待人工确认"));
        return;
    }
    m_status = ShortageValidationStatus::PassedByOperator;
    appendValidationLog(QStringLiteral("人工确认完成：现场证据已由操作员核对通过"));
    refreshStatusLabel();
}

void ShortageValidationDialog::appendValidationLog(const QString &messageZh)
{
    if (m_logEdit == nullptr)
        return;
    QString inputZh = QStringLiteral("无");
    QString expectedZh = QStringLiteral("无");
    if (m_currentCaseIndex >= 0 && m_currentCaseIndex < m_cases.size()
        && m_currentStepIndex < m_cases.at(m_currentCaseIndex).steps.size()) {
        const ShortageValidationStep &step = m_cases.at(m_currentCaseIndex).steps.at(m_currentStepIndex);
        inputZh = step.instructionZh;
        expectedZh = step.expectedZh;
        if (step.action == ShortageValidationAction::ApplyManualSample) {
            inputZh += QStringLiteral("（产品=%1，模式=%2，actualQty=%3）")
                           .arg(productText(step.product),
                                modeText(step.mode))
                           .arg(step.actualQty);
        }
    }
    const QString actualZh = m_snapshotEvidenceLabel != nullptr
                             ? m_snapshotEvidenceLabel->text().simplified()
                             : QStringLiteral("无实际快照");
    const QString stepText =
        m_currentCaseIndex >= 0 && m_currentCaseIndex < m_cases.size()
        ? QStringLiteral("%1/%2").arg(qMin(m_currentStepIndex + 1,
                                           m_cases.at(m_currentCaseIndex).steps.size()))
              .arg(m_cases.at(m_currentCaseIndex).steps.size())
        : QStringLiteral("-");
    m_logEdit->appendPlainText(QStringLiteral("[%1] %2 步骤%3：输入=%4；预期=%5；实际=%6；消息=%7")
                                   .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),
                                        currentCaseId(),
                                        stepText,
                                        inputZh,
                                        expectedZh,
                                        actualZh,
                                        messageZh));
}

QList<ShortageValidationCase> ShortageValidationDialog::createValidationCases(int preUnloadFailureLimit)
{
    const QString criteria01 = QStringLiteral("两个窗口均可最小化、最大化和恢复；主窗口始终可以激活；重复点击只激活唯一窗口，不出现多个相同窗口；窗口操作不改变测试状态。");
    const QString criteria02 = QStringLiteral("正式状态文件哈希和主 FIFO 数量不变；AGV、机械臂和扫码硬件命令计数不增加；测试状态只写入 `test-*` 文件。");
    const QString criteria03 = QStringLiteral("页面接收手工 `actualQty=100`；真实请求计数不变；启动现场采样不可用；日志明确记录手工样本。");
    const QString criteria04 = QStringLiteral("建账后 12 工位均为 0；首样本只建基线；低位工位按同时间工位号排序；活动工位为 1；只生成一个工位 1 的 `AwaitingDispatch` 补料单并立即显示。");
    const QString criteria05 = QStringLiteral("真实采样只在现场源启动；两轮稳定前不产生稳定样本；首次稳定样本只建基线并立即显示一个 `AwaitingDispatch` 补料单；正式 Engine 和 FIFO 不变化。");
    const QString criteria06 = QStringLiteral("每个启用工位库存等于旧库存减 `5×当前模式用量`；用量 0 工位不变；基线为 105；最近增量为 5。");
    const QString criteria07 = QStringLiteral("拒收后补料单保持 `AwaitingDispatch` 且单号不变，不生成重复单；接受后绑定测试 taskId 并进入 `Running`。");
    const QString criteria08 = QStringLiteral("倒料完成时增加准确一箱；任务终态不重复加箱；未达最高位生成下一箱；达到最高位后释放活动工位并切换下一等待工位。");
    const QString criteria09 = QStringLiteral("每次倒料前失败不增加库存；达到阈值时只暂停目标工位；其他工位继续计划；日志包含工位、原库存、失败次数和处理动作。");
    const QString criteria10 = QStringLiteral("已倒料的一箱不回滚；倒料前失败计数不增加；补料单进入倒料后失败终态。");
    const QString criteria11 = QStringLiteral("库存不第二次增加；系统进入严重锁定；停止新自动意图；日志包含补料单号、taskId、工位和重复倒料处理动作。");
    const QString criteria12 = QStringLiteral("`1000→2→5` 按新周期累计 5 扣减；连续下降序列最终合计扣 5；`1000→2→1005` 只按旧周期增量 5 扣减；页面显示基线和候选变化。");
    const QString criteria13 = QStringLiteral("手工源九种组合使用正确配置用量；现场源一轮抖动不切换、两轮相同才确认；旧任务未终态时保存待切换上下文；排空后切换；库存沿用并使用新组合用量。");
    const QString criteria14 = QStringLiteral("安全状态逐字段恢复；不安全状态进入维护锁定；清空只删除 `test-*` 文件；正式文件哈希始终不变。");
    const QString criteria15 = QStringLiteral("现场采样运行中不能混用输入源；停止后可切换；非法动作按钮禁用；直接调用控制器仍被拒绝并输出中文原因。");

    return {
        validationCase(QStringLiteral("VT-01"), QStringLiteral("窗口切换能力"), {
                           step(QStringLiteral("打开独立缺料验证控制台并保留主窗口。"),
                                QStringLiteral("验证控制台以非模态窗口显示。")),
                           step(QStringLiteral("最小化验证控制台后激活主窗口。"),
                                QStringLiteral("主窗口可以自由操作且测试状态不变化。")),
                           step(QStringLiteral("恢复验证控制台并最大化。"),
                                QStringLiteral("窗口可恢复和最大化，内容仍完整。")),
                           step(QStringLiteral("重复点击入口或切回验证控制台。"),
                                QStringLiteral("只激活唯一验证窗口，不创建重复窗口。")),
                           step(QStringLiteral("关闭验证控制台。"),
                                QStringLiteral("关闭动作不改变测试状态。")),
                       }, criteria01, true),
        validationCase(QStringLiteral("VT-02"), QStringLiteral("测试与正式环境隔离"), {
                           step(QStringLiteral("记录正式状态文件哈希、主 FIFO 数量和硬件命令计数。"),
                                QStringLiteral("形成验证前基线，尚不执行测试动作。")),
                           step(QStringLiteral("执行清空测试状态。"), QStringLiteral("只删除 test-* 文件。"),
                                ShortageValidationAction::ClearTestState),
                           step(QStringLiteral("执行 0 建账。"),
                                QStringLiteral("独立测试状态从 0 开始，正式侧基线不变。"),
                                ShortageValidationAction::InitializeZero),
                           step(QStringLiteral("切换为手工源。"),
                                QStringLiteral("不会访问真实 MES/PLC。"),
                                ShortageValidationAction::SelectManualSource),
                           step(QStringLiteral("提交手工样本 88、L/R、actualQty=100。"),
                                QStringLiteral("独立测试状态变化，正式侧基线不变。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 100),
                           step(QStringLiteral("复核正式状态文件、主 FIFO 和硬件命令计数。"),
                                QStringLiteral("正式证据与基线一致。")),
                       }, criteria02, true),
        validationCase(QStringLiteral("VT-03"), QStringLiteral("手工源不访问真实系统"), {
                           step(QStringLiteral("切换为手工源。"), QStringLiteral("不会访问真实 MES/PLC。"),
                                ShortageValidationAction::SelectManualSource),
                           step(QStringLiteral("提交手工样本 88、L/R、actualQty=100。"),
                                QStringLiteral("页面接收手工样本并写入测试日志。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 100),
                           step(QStringLiteral("复核真实请求计数和现场采样按钮。"),
                                QStringLiteral("真实请求计数不变，现场采样不可用。")),
                       }, criteria03, true),
        validationCase(QStringLiteral("VT-04"), QStringLiteral("0 建账和首样本补料计划"), {
                           step(QStringLiteral("清空独立测试状态。"), QStringLiteral("StandaloneTest 重新开始。"),
                                ShortageValidationAction::ClearTestState),
                           step(QStringLiteral("确认从 0 建账。"), QStringLiteral("12 个工位库存均为 0。"),
                                ShortageValidationAction::InitializeZero),
                           step(QStringLiteral("切换为手工源。"), QStringLiteral("手工源可提交样本。"),
                                ShortageValidationAction::SelectManualSource),
                           step(QStringLiteral("提交手工样本 88、L/R、actualQty=100。"),
                                QStringLiteral("首样本建立基线并生成工位 1 待派单。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 100),
                       }, criteria04, false),
        validationCase(QStringLiteral("VT-05"), QStringLiteral("现场源首样本补料计划"), {
                           step(QStringLiteral("清空独立测试状态。"), QStringLiteral("测试文件重新开始。"),
                                ShortageValidationAction::ClearTestState),
                           step(QStringLiteral("确认从 0 建账。"), QStringLiteral("12 工位为 0。"),
                                ShortageValidationAction::InitializeZero),
                           step(QStringLiteral("切换为现场源。"), QStringLiteral("尚未启动真实采样。"),
                                ShortageValidationAction::SelectFieldSource),
                           step(QStringLiteral("启动现场采样。"), QStringLiteral("只在现场源启动。"),
                                ShortageValidationAction::StartFieldSampling),
                           step(QStringLiteral("等待两轮现场稳定样本。"), QStringLiteral("稳定前不产生样本。")),
                           step(QStringLiteral("停止现场采样。"), QStringLiteral("迟到样本被屏蔽。"),
                                ShortageValidationAction::StopFieldSampling),
                       }, criteria05, true),
        validationCase(QStringLiteral("VT-06"), QStringLiteral("正常产量扣减"), {
                           step(QStringLiteral("清空独立测试状态。"), QStringLiteral("旧状态清除。"),
                                ShortageValidationAction::ClearTestState),
                           step(QStringLiteral("确认从 0 建账。"), QStringLiteral("12 工位为 0。"),
                                ShortageValidationAction::InitializeZero),
                           step(QStringLiteral("切换为手工源。"), QStringLiteral("可提交手工样本。"),
                                ShortageValidationAction::SelectManualSource),
                           step(QStringLiteral("提交手工样本 88、L/R、actualQty=100。"),
                                QStringLiteral("建立基线。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 100),
                           step(QStringLiteral("提交手工样本 88、L/R、actualQty=105。"),
                                QStringLiteral("按增量 5 扣减。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 105),
                       }, criteria06, false),
        validationCase(QStringLiteral("VT-07"), QStringLiteral("派单拒绝和原单重试"), {
                           step(QStringLiteral("清空、0 建账并提交 88 L/R 100。"),
                                QStringLiteral("形成 VT-04 状态。"),
                                ShortageValidationAction::ClearTestState),
                           step(QStringLiteral("确认从 0 建账。"), QStringLiteral("库存为 0。"),
                                ShortageValidationAction::InitializeZero),
                           step(QStringLiteral("切换为手工源。"), QStringLiteral("使用公开手工测试入口。"),
                                ShortageValidationAction::SelectManualSource),
                           step(QStringLiteral("提交首样本。"), QStringLiteral("生成待派单。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 100),
                           step(QStringLiteral("模拟主调度拒收当前补料单。"),
                                QStringLiteral("补料单保持待派且单号不变。"),
                                ShortageValidationAction::DispatchRejected),
                           step(QStringLiteral("模拟主调度接受当前补料单。"),
                                QStringLiteral("绑定测试 taskId 并进入运行。"),
                                ShortageValidationAction::DispatchAccepted),
                       }, criteria07, false),
        validationCase(QStringLiteral("VT-08"), QStringLiteral("倒料入账与连续补料"), {
                           step(QStringLiteral("清空、0 建账并提交首样本。"), QStringLiteral("形成待派单。"),
                                ShortageValidationAction::ClearTestState),
                           step(QStringLiteral("确认从 0 建账。"), QStringLiteral("库存为 0。"),
                                ShortageValidationAction::InitializeZero),
                           step(QStringLiteral("切换为手工源。"), QStringLiteral("使用公开手工测试入口。"),
                                ShortageValidationAction::SelectManualSource),
                           step(QStringLiteral("提交首样本。"), QStringLiteral("生成待派单。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 100),
                           step(QStringLiteral("模拟接受补料单。"), QStringLiteral("任务运行。"),
                                ShortageValidationAction::DispatchAccepted),
                           step(QStringLiteral("模拟倒料完成。"), QStringLiteral("准确增加一箱。"),
                                ShortageValidationAction::MaterialUnloaded),
                           step(QStringLiteral("模拟任务成功。"), QStringLiteral("终态不重复加箱。"),
                                ShortageValidationAction::TaskSucceeded),
                       }, criteria08, true),
        validationCase(QStringLiteral("VT-09"), QStringLiteral("倒料前失败保护"),
                       vt09Steps(preUnloadFailureLimit), criteria09, false),
        validationCase(QStringLiteral("VT-10"), QStringLiteral("倒料后失败"), {
                           step(QStringLiteral("清空、0 建账并提交首样本。"), QStringLiteral("形成待派单。"),
                                ShortageValidationAction::ClearTestState),
                           step(QStringLiteral("确认从 0 建账。"), QStringLiteral("库存为 0。"),
                                ShortageValidationAction::InitializeZero),
                           step(QStringLiteral("切换为手工源。"), QStringLiteral("使用公开手工测试入口。"),
                                ShortageValidationAction::SelectManualSource),
                           step(QStringLiteral("提交首样本。"), QStringLiteral("生成待派单。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 100),
                           step(QStringLiteral("模拟接受补料单。"), QStringLiteral("任务运行。"),
                                ShortageValidationAction::DispatchAccepted),
                           step(QStringLiteral("模拟倒料完成。"), QStringLiteral("库存增加一箱。"),
                                ShortageValidationAction::MaterialUnloaded),
                           step(QStringLiteral("模拟倒料后失败。"), QStringLiteral("库存不回滚。"),
                                ShortageValidationAction::FailureAfterUnload),
                       }, criteria10, false),
        validationCase(QStringLiteral("VT-11"), QStringLiteral("重复倒料严重锁定"), {
                           step(QStringLiteral("清空、0 建账并提交首样本。"), QStringLiteral("形成待派单。"),
                                ShortageValidationAction::ClearTestState),
                           step(QStringLiteral("确认从 0 建账。"), QStringLiteral("库存为 0。"),
                                ShortageValidationAction::InitializeZero),
                           step(QStringLiteral("切换为手工源。"), QStringLiteral("使用公开手工测试入口。"),
                                ShortageValidationAction::SelectManualSource),
                           step(QStringLiteral("提交首样本。"), QStringLiteral("生成待派单。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 100),
                           step(QStringLiteral("模拟接受补料单。"), QStringLiteral("任务运行。"),
                                ShortageValidationAction::DispatchAccepted),
                           step(QStringLiteral("模拟倒料完成。"), QStringLiteral("库存增加一箱。"),
                                ShortageValidationAction::MaterialUnloaded),
                           step(QStringLiteral("重发最近倒料事实。"), QStringLiteral("库存不第二次增加并严重锁定。"),
                                ShortageValidationAction::ResendUnload),
                       }, criteria11, false),
        validationCase(QStringLiteral("VT-12"), QStringLiteral("actualQty 清零与毛刺"), {
                           step(QStringLiteral("清空独立测试状态。"), QStringLiteral("第一组序列从干净状态开始。"),
                                ShortageValidationAction::ClearTestState),
                           step(QStringLiteral("确认从 0 建账。"), QStringLiteral("库存为 0。"),
                                ShortageValidationAction::InitializeZero),
                           step(QStringLiteral("切换为手工源。"), QStringLiteral("actualQty 序列只走公开手工入口。"),
                                ShortageValidationAction::SelectManualSource),
                           step(QStringLiteral("第一组提交 actualQty=1000。"), QStringLiteral("建立旧周期基线。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 1000),
                           step(QStringLiteral("第一组提交 actualQty=2。"), QStringLiteral("识别为新周期候选。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 2),
                           step(QStringLiteral("第一组提交 actualQty=5。"), QStringLiteral("按新周期累计 5 扣减。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 5),
                           step(QStringLiteral("复核第一组基线、候选和库存证据。"), QStringLiteral("页面展示 1000→2→5 证据。")),
                           step(QStringLiteral("清空独立测试状态。"), QStringLiteral("第二组序列从干净状态开始。"),
                                ShortageValidationAction::ClearTestState),
                           step(QStringLiteral("确认从 0 建账。"), QStringLiteral("库存为 0。"),
                                ShortageValidationAction::InitializeZero),
                           step(QStringLiteral("第二组提交 actualQty=1000。"), QStringLiteral("建立旧周期基线。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 1000),
                           step(QStringLiteral("第二组提交 actualQty=900。"), QStringLiteral("连续下降候选更新。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 900),
                           step(QStringLiteral("第二组提交 actualQty=500。"), QStringLiteral("连续下降候选继续更新。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 500),
                           step(QStringLiteral("第二组提交 actualQty=0。"), QStringLiteral("连续下降候选到 0。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 0),
                           step(QStringLiteral("第二组提交 actualQty=2。"), QStringLiteral("新周期候选开始累计。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 2),
                           step(QStringLiteral("第二组提交 actualQty=5。"), QStringLiteral("连续下降最终合计扣 5。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 5),
                           step(QStringLiteral("复核第二组基线、候选和库存证据。"), QStringLiteral("页面展示 1000→900→500→0→2→5 证据。")),
                           step(QStringLiteral("清空独立测试状态。"), QStringLiteral("第三组序列从干净状态开始。"),
                                ShortageValidationAction::ClearTestState),
                           step(QStringLiteral("确认从 0 建账。"), QStringLiteral("库存为 0。"),
                                ShortageValidationAction::InitializeZero),
                           step(QStringLiteral("第三组提交 actualQty=1000。"), QStringLiteral("建立旧周期基线。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 1000),
                           step(QStringLiteral("第三组提交 actualQty=2。"), QStringLiteral("记录清零候选。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 2),
                           step(QStringLiteral("第三组提交 actualQty=1005。"), QStringLiteral("只按旧周期增量 5 扣减。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 1005),
                           step(QStringLiteral("复核页面基线、候选和库存证据。"), QStringLiteral("三组证据均可现场追踪。")),
                       }, criteria12, true),
        validationCase(QStringLiteral("VT-13"), QStringLiteral("换型和九种组合"), {
                           step(QStringLiteral("清空独立测试状态。"), QStringLiteral("九种组合从干净状态开始。"),
                                ShortageValidationAction::ClearTestState),
                           step(QStringLiteral("确认从 0 建账。"), QStringLiteral("库存为 0。"),
                                ShortageValidationAction::InitializeZero),
                           step(QStringLiteral("切换为手工源。"), QStringLiteral("九种组合只走公开手工入口。"),
                                ShortageValidationAction::SelectManualSource),
                           step(QStringLiteral("提交 88、L/R、actualQty=100。"), QStringLiteral("核对 88 L/R 用量。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 100),
                           step(QStringLiteral("提交 88、L/L、actualQty=105。"), QStringLiteral("核对 88 L/L 用量。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftOnly, 105),
                           step(QStringLiteral("提交 88、R/H、actualQty=110。"), QStringLiteral("核对 88 R/H 用量。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::RightOnly, 110),
                           step(QStringLiteral("提交 88R、L/R、actualQty=115。"), QStringLiteral("核对 88R L/R 用量。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88R, ProductionMode::LeftRight, 115),
                           step(QStringLiteral("提交 88R、L/L、actualQty=120。"), QStringLiteral("核对 88R L/L 用量。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88R, ProductionMode::LeftOnly, 120),
                           step(QStringLiteral("提交 88R、R/H、actualQty=125。"), QStringLiteral("核对 88R R/H 用量。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88R, ProductionMode::RightOnly, 125),
                           step(QStringLiteral("提交 92、L/R、actualQty=130。"), QStringLiteral("核对 92 L/R 用量。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model92, ProductionMode::LeftRight, 130),
                           step(QStringLiteral("提交 92、L/L、actualQty=135。"), QStringLiteral("核对 92 L/L 用量。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model92, ProductionMode::LeftOnly, 135),
                           step(QStringLiteral("提交 92、R/H、actualQty=140。"), QStringLiteral("核对 92 R/H 用量。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model92, ProductionMode::RightOnly, 140),
                           step(QStringLiteral("切换为现场源。"), QStringLiteral("现场稳定性验证尚未自动判定。"),
                                ShortageValidationAction::SelectFieldSource),
                           step(QStringLiteral("启动现场采样。"), QStringLiteral("只在现场源启动真实采样。"),
                                ShortageValidationAction::StartFieldSampling),
                           step(QStringLiteral("现场源制造一轮抖动。"), QStringLiteral("一轮抖动不切换。")),
                           step(QStringLiteral("现场源制造连续两轮新上下文。"), QStringLiteral("两轮相同才确认。")),
                           step(QStringLiteral("旧任务未终态时确认上下文切换。"), QStringLiteral("保存待切换上下文。")),
                           step(QStringLiteral("停止现场采样。"), QStringLiteral("迟到样本被屏蔽。"),
                                ShortageValidationAction::StopFieldSampling),
                           step(QStringLiteral("排空旧任务后复核库存。"), QStringLiteral("切换后沿用库存并使用新组合用量。")),
                       }, criteria13, true),
        validationCase(QStringLiteral("VT-14"), QStringLiteral("保存、重载、清空和重启恢复"), {
                           step(QStringLiteral("保存当前测试状态。"), QStringLiteral("记录 StandaloneTest 保存证据。"),
                                ShortageValidationAction::SaveState),
                           step(QStringLiteral("模拟程序重启。"), QStringLiteral("重建测试运行对象并恢复。"),
                                ShortageValidationAction::SimulateRestart),
                           step(QStringLiteral("重新加载测试状态。"), QStringLiteral("走恢复校验。"),
                                ShortageValidationAction::ReloadState),
                           step(QStringLiteral("清空测试状态。"), QStringLiteral("只删除 test-* 文件。"),
                                ShortageValidationAction::ClearTestState),
                       }, criteria14, true),
        validationCase(QStringLiteral("VT-15"), QStringLiteral("来源切换和误操作门禁"), {
                           step(QStringLiteral("切换为现场源。"), QStringLiteral("手工提交不可用。"),
                                ShortageValidationAction::SelectFieldSource),
                           step(QStringLiteral("启动现场采样。"), QStringLiteral("采样运行中不能混用输入源。"),
                                ShortageValidationAction::StartFieldSampling),
                           step(QStringLiteral("切回手工源。"), QStringLiteral("先停止现场采样后允许切换。"),
                                ShortageValidationAction::SelectManualSource),
                           step(QStringLiteral("提交手工样本。"), QStringLiteral("停止后才可提交。"),
                                ShortageValidationAction::ApplyManualSample,
                                ProductModel::Model88, ProductionMode::LeftRight, 100),
                           step(QStringLiteral("检查无前置状态按钮和直接调用拒绝原因。"),
                                QStringLiteral("非法动作禁用且拒绝原因是中文。")),
                       }, criteria15, true),
    };
}

void ShortageValidationDialog::refreshCaseDetails()
{
    if (m_stepText == nullptr)
        return;
    if (m_currentCaseIndex < 0 || m_currentCaseIndex >= m_cases.size()) {
        m_stepText->clear();
        return;
    }

    const ShortageValidationCase &item = m_cases.at(m_currentCaseIndex);
    QString text;
    text += QStringLiteral("%1 %2\n\n").arg(item.id, item.nameZh);
    for (int index = 0; index < item.steps.size(); ++index) {
        const ShortageValidationStep &itemStep = item.steps.at(index);
        const QString marker = index == m_currentStepIndex ? QStringLiteral("▶") : QStringLiteral(" ");
        text += QStringLiteral("%1 步骤 %2：%3\n   动作：%4\n   预期：%5\n")
                    .arg(marker)
                    .arg(index + 1)
                    .arg(itemStep.instructionZh,
                         actionText(itemStep.action),
                         itemStep.expectedZh);
        if (itemStep.action == ShortageValidationAction::ApplyManualSample) {
            text += QStringLiteral("   输入：产品=%1，模式=%2，actualQty=%3\n")
                        .arg(productText(itemStep.product),
                             modeText(itemStep.mode))
                        .arg(itemStep.actualQty);
        }
    }
    text += QStringLiteral("\n通过标准：%1\n").arg(item.passCriteriaZh);
    if (item.requiresManualEvidence)
        text += QStringLiteral("\n人工边界：本项涉及现场、窗口或外部隔离证据，自动执行后必须人工确认。\n");
    m_stepText->setPlainText(text);
}

void ShortageValidationDialog::refreshStatusLabel()
{
    if (m_statusLabel == nullptr || m_manualConfirmButton == nullptr || m_nextStepButton == nullptr)
        return;

    QString statusText;
    switch (m_status) {
    case ShortageValidationStatus::NotStarted:
        statusText = QStringLiteral("尚未开始");
        break;
    case ShortageValidationStatus::InProgress:
        statusText = QStringLiteral("执行中");
        break;
    case ShortageValidationStatus::PassedAutomatically:
        statusText = QStringLiteral("自动通过");
        break;
    case ShortageValidationStatus::FailedAutomatically:
        statusText = QStringLiteral("自动失败");
        break;
    case ShortageValidationStatus::WaitingManualEvidence:
        statusText = QStringLiteral("等待人工确认");
        break;
    case ShortageValidationStatus::PassedByOperator:
        statusText = QStringLiteral("人工确认通过");
        break;
    }
    m_statusLabel->setText(QStringLiteral("状态：%1").arg(statusText));
    m_manualConfirmButton->setEnabled(m_status == ShortageValidationStatus::WaitingManualEvidence);
    m_nextStepButton->setEnabled(m_currentCaseIndex >= 0);
}

void ShortageValidationDialog::refreshSnapshotEvidence(const ShortageUiSnapshot &snapshot)
{
    if (m_snapshotEvidenceLabel == nullptr)
        return;

    const ShortageRuntimeState &state = snapshot.runtime;
    QString waiting;
    for (int stationId : state.waitingStationIds) {
        if (!waiting.isEmpty())
            waiting += QStringLiteral("、");
        waiting += QString::number(stationId);
    }
    if (waiting.isEmpty())
        waiting = QStringLiteral("无");

    QString currentOrder = QStringLiteral("无");
    if (!state.orders.isEmpty()) {
        const ReplenishmentOrder &order = state.orders.last();
        currentOrder = QStringLiteral("单号=%1，工位=%2，状态=%3，taskId=%4，已倒料=%5")
                           .arg(order.orderNo)
                           .arg(order.stationId)
                           .arg(orderStateText(order.state))
                           .arg(order.taskId)
                           .arg(yesNo(order.unloadAccounted));
    }

    m_snapshotEvidenceLabel->setText(QStringLiteral(
        "产品/模式：%1 / %2\n"
        "actualQty/基线：hasBaseline=%3，baseline=%4，候选=%5\n"
        "活动工位：%6\n"
        "等待顺序：%7\n"
        "当前补料单：%8\n"
        "严重锁定：%9，原因=%10\n"
        "最近增量：%11，delta=%12\n"
        "摘要一：%13\n"
        "摘要二：%14")
                                         .arg(productText(state.product),
                                              modeText(state.mode),
                                              yesNo(state.actualQty.hasBaseline))
                                         .arg(state.actualQty.baseline)
                                         .arg(state.actualQty.hasResetCandidate
                                                  ? QString::number(state.actualQty.resetCandidate)
                                                  : QStringLiteral("无"))
                                         .arg(state.activeStationId)
                                         .arg(waiting,
                                              currentOrder,
                                              yesNo(state.criticalLock),
                                              state.criticalReasonZh.isEmpty()
                                                  ? QStringLiteral("无")
                                                  : state.criticalReasonZh,
                                              yesNo(snapshot.hasLastProductionDelta))
                                         .arg(snapshot.lastProductionDelta)
                                         .arg(snapshot.summaryLine1Zh,
                                              snapshot.summaryLine2Zh));
}

QString ShortageValidationDialog::currentCaseId() const
{
    if (m_currentCaseIndex < 0 || m_currentCaseIndex >= m_cases.size())
        return QStringLiteral("VT-??");
    return m_cases.at(m_currentCaseIndex).id;
}

QString ShortageValidationDialog::productText(ProductModel product)
{
    switch (product) {
    case ProductModel::Model88:
        return QStringLiteral("88");
    case ProductModel::Model88R:
        return QStringLiteral("88R");
    case ProductModel::Model92:
        return QStringLiteral("92");
    }
    return QStringLiteral("未知产品");
}

QString ShortageValidationDialog::modeText(ProductionMode mode)
{
    switch (mode) {
    case ProductionMode::LeftRight:
        return QStringLiteral("L/R");
    case ProductionMode::LeftOnly:
        return QStringLiteral("L/L");
    case ProductionMode::RightOnly:
        return QStringLiteral("R/H");
    }
    return QStringLiteral("未知模式");
}

QString ShortageValidationDialog::orderStateText(ReplenishmentOrderState state)
{
    switch (state) {
    case ReplenishmentOrderState::AwaitingDispatch:
        return QStringLiteral("等待派单");
    case ReplenishmentOrderState::Queued:
        return QStringLiteral("已入队");
    case ReplenishmentOrderState::Running:
        return QStringLiteral("运行中");
    case ReplenishmentOrderState::Unloaded:
        return QStringLiteral("已倒料");
    case ReplenishmentOrderState::Succeeded:
        return QStringLiteral("成功");
    case ReplenishmentOrderState::FailedBeforeUnload:
        return QStringLiteral("倒料前失败");
    case ReplenishmentOrderState::FailedAfterUnload:
        return QStringLiteral("倒料后失败");
    case ReplenishmentOrderState::Canceled:
        return QStringLiteral("已取消");
    }
    return QStringLiteral("未知状态");
}

QString ShortageValidationDialog::actionText(ShortageValidationAction action)
{
    switch (action) {
    case ShortageValidationAction::ShowInstruction:
        return QStringLiteral("显示说明");
    case ShortageValidationAction::ClearTestState:
        return QStringLiteral("清空测试状态");
    case ShortageValidationAction::InitializeZero:
        return QStringLiteral("0 建账");
    case ShortageValidationAction::SelectManualSource:
        return QStringLiteral("选择手工源");
    case ShortageValidationAction::SelectFieldSource:
        return QStringLiteral("选择现场源");
    case ShortageValidationAction::ApplyManualSample:
        return QStringLiteral("提交手工样本");
    case ShortageValidationAction::StartFieldSampling:
        return QStringLiteral("启动现场采样");
    case ShortageValidationAction::StopFieldSampling:
        return QStringLiteral("停止现场采样");
    case ShortageValidationAction::DispatchRejected:
        return QStringLiteral("派单拒收");
    case ShortageValidationAction::DispatchAccepted:
        return QStringLiteral("派单接受");
    case ShortageValidationAction::FailureBeforeUnload:
        return QStringLiteral("倒料前失败");
    case ShortageValidationAction::MaterialUnloaded:
        return QStringLiteral("倒料完成");
    case ShortageValidationAction::FailureAfterUnload:
        return QStringLiteral("倒料后失败");
    case ShortageValidationAction::TaskSucceeded:
        return QStringLiteral("任务成功");
    case ShortageValidationAction::ResendUnload:
        return QStringLiteral("重发倒料事实");
    case ShortageValidationAction::SaveState:
        return QStringLiteral("保存测试状态");
    case ShortageValidationAction::ReloadState:
        return QStringLiteral("重载测试状态");
    case ShortageValidationAction::SimulateRestart:
        return QStringLiteral("模拟重启");
    }
    return QStringLiteral("未知动作");
}

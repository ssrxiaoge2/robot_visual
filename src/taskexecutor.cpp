#include "taskexecutor.h"

#include <QDateTime>
#include <QTimer>

#include "huayanScheduler.h"

TaskExecutor::TaskExecutor(AgvController *agv,
                           HuayanScheduler *arm,
                           NScanScheduler *scanner,
                           PalletScheduler *pallet,
                           QObject *parent)
    : QObject(parent)
    , m_agv(agv)
    , m_arm(arm)
    , m_scanner(scanner)
    , m_pallet(pallet)
    , m_agvTimeout(new QTimer(this))
{
    Q_UNUSED(m_scanner);

    m_agvTimeout->setSingleShot(true);
    connect(m_agvTimeout, &QTimer::timeout, this, &TaskExecutor::onAgvTimeout);

    if (m_agv) {
        connect(m_agv, &AgvController::monitorUpdated,
                this, &TaskExecutor::onAgvMonitor);
        connect(m_agv, &AgvController::errorOccurred, this, [this](const QString &msg) {
            if (!isBusy()) {
                return;
            }
            raiseSystemError(QStringLiteral("AGV 通信错误：%1").arg(msg));
        });
    }

    if (m_arm) {
        connect(m_arm, &HuayanScheduler::stageCompleted,
                this, &TaskExecutor::onArmStageCompleted);
        connect(m_arm, &HuayanScheduler::stageError,
                this, &TaskExecutor::onArmStageError);
        connect(m_arm, &HuayanScheduler::preGripScanRequested,
                this, &TaskExecutor::onPreGripScanRequested);
        connect(m_arm, &HuayanScheduler::preGripScanSearchMoveCompleted,
                this, &TaskExecutor::onPreGripScanSearchMoveCompleted);
        connect(m_arm, &HuayanScheduler::preGripScanSearchMoveError,
                this, &TaskExecutor::onPreGripScanSearchMoveError);
        connect(m_arm, &HuayanScheduler::preGripScanCaptureReturnCompleted,
                this, &TaskExecutor::onPreGripScanCaptureReturnCompleted);
        connect(m_arm, &HuayanScheduler::preGripScanCaptureReturnError,
                this, &TaskExecutor::onPreGripScanCaptureReturnError);
        connect(m_arm, &HuayanScheduler::toolRotationCompleted,
                this, &TaskExecutor::onToolRotationCompleted);
        connect(m_arm, &HuayanScheduler::toolRotationError,
                this, &TaskExecutor::onToolRotationError);
        connect(m_arm, &HuayanScheduler::palletPlaceCompleted,
                this, &TaskExecutor::onPalletPlaceCompleted);
        connect(m_arm, &HuayanScheduler::palletPlaceError,
                this, &TaskExecutor::onPalletPlaceError);
        connect(m_arm, &HuayanScheduler::logMessage, this, [this](const QString &message) {
            if (m_task.taskId == 0 || !isBusy()) {
                return;
            }
            emit logMessage(prefix(QStringLiteral("ARM")) + QStringLiteral(" ") + message);
        });
    }
}

bool TaskExecutor::isBusy() const
{
    return m_state != ExecState::Idle;
}

Task TaskExecutor::currentTask() const
{
    return m_task;
}

void TaskExecutor::start(const Task &task)
{
    if (isBusy()) {
        Task rejected = task;
        rejected.state = TaskState::Failed;
        rejected.step = TaskStep::Done;
        rejected.stepIndex = stepIndexForTaskStep(TaskStep::Done);
        rejected.statusText = QStringLiteral("TaskExecutor 忙碌，不能并行启动新任务");
        rejected.lastError = rejected.statusText;
        emit taskUpdated(rejected);
        emit systemError(rejected, rejected.lastError);
        return;
    }

    resetRuntimeState();
    m_task = task;
    if (!m_task.createdAt.isValid()) {
        m_task.createdAt = QDateTime::currentDateTimeUtc();
    }

    setTaskRunning(TaskStep::Waiting, QStringLiteral("任务启动，加载工位与码垛配置"));

    if (!resolveTaskConfigs()) {
        return;
    }

    if (!m_agv || !m_arm || !m_pallet) {
        raiseSystemError(QStringLiteral("TaskExecutor 依赖未完整注入"));
        return;
    }

    HuayanScheduler::StationArmFunctions stationFuncs;
    stationFuncs.captureFunc = m_stationCfg->captureFunc;
    stationFuncs.unloadPointFunc = m_stationCfg->unloadPointFunc;
    stationFuncs.unloadFunc = m_stationCfg->unloadFunc;
    m_arm->setStationFunctions(stationFuncs);

    HuayanScheduler::PalletArmFunctions palletFuncs;
    palletFuncs.palletBaseFunc = m_palletCfg->palletBaseFunc;
    palletFuncs.releaseFunc = m_palletCfg->releaseFunc;
    m_arm->setPalletFunctions(palletFuncs);
    m_arm->setPreGripScanEnabled(true);

    clearPalletStackingFlag();

    emit logMessage(prefix(QStringLiteral("TASK"))
                    + QStringLiteral(" 已注入工位 %1 和 %2 的机械臂/码垛配置")
                          .arg(m_stationCfg->stationId)
                          .arg(palletAreaDisplayName(m_stationCfg->palletArea)));

    startAgvStep(ExecState::AgvToPickup,
                 m_stationCfg->pickupLm,
                 QStringLiteral("AGV 前往取料位 LM%1").arg(m_stationCfg->pickupLm));
}

void TaskExecutor::stopForSystemError(const QString &reason)
{
    if (!isBusy()) {
        emit logMessage(prefix(QStringLiteral("ERROR"))
                        + QStringLiteral(" 收到系统级停机请求，但当前无活动任务：%1").arg(reason));
        return;
    }

    raiseSystemError(reason);
}

void TaskExecutor::onScanFinished(const NScanScheduler::ScanResult &result)
{
    if (!isBusy() || m_state != ExecState::PreGripScan) {
        return;
    }

    const QString trimmedCode = result.code.trimmed();
    if (result.isSuccess() && !trimmedCode.isEmpty()) {
        emit logMessage(prefix(QStringLiteral("SCAN"))
                        + QStringLiteral(" 第 %1 轮扫码成功，条码=%2")
                              .arg(m_scanAttempt)
                              .arg(trimmedCode));

        if (m_scanSearchIndex > 0) {
            m_pendingScanCodeAfterSearch = trimmedCode;
            m_state = ExecState::PreGripScanSearchReturn;
            setTaskRunning(TaskStep::PreGripScan,
                           QStringLiteral("扫码搜索成功，回到原夹取位置后继续夹紧"));
            emit logMessage(prefix(QStringLiteral("SCAN"))
                            + QStringLiteral(" 扫码搜索成功，回到原夹取位置"));
            m_arm->movePreGripScanSearchTo(0.0);
            return;
        }

        resetScanSearchState();
        setTaskRunning(TaskStep::GripAndLift, QStringLiteral("扫码通过，机械臂继续夹紧并抬升"));
        m_arm->continueAfterPreGripScan();
        return;
    }

    QString failureDetail;
    if (result.isSuccess() && trimmedCode.isEmpty()) {
        failureDetail = QStringLiteral("返回空码");
    } else if (!result.errorMessage.isEmpty()) {
        failureDetail = result.errorMessage;
    } else {
        failureDetail = NScanScheduler::statusText(result.status);
    }

    m_lastScanFailureDetail = failureDetail;
    if (m_scanSearchIndex < kScanSearchTargetCount) {
        startNextScanSearchMove(failureDetail);
        return;
    }

    if (kEnableScanFailureRotationRecovery) {
        emit logMessage(prefix(QStringLiteral("SCAN"))
                        + QStringLiteral(" Y 轴搜码全部失败，但旋转补救开关已打开；当前版本仍按回拍照位失败收尾处理"));
    }

    m_state = ExecState::PreGripScanReturnCapture;
    setTaskRunning(TaskStep::PreGripScan, QStringLiteral("扫码搜索仍失败，机械臂返回拍照位"));
    emit logMessage(prefix(QStringLiteral("SCAN"))
                    + QStringLiteral(" Y 轴搜码全部失败：%1，返回拍照位").arg(failureDetail));
    m_arm->returnToCaptureForScanFailure();
}

void TaskExecutor::onAgvMonitor(const AgvMonitorData &data)
{
    if (!isBusy() || !isAgvNavigationState(m_state)) {
        return;
    }

    if (data.navStatus == static_cast<quint16>(AgvController::NavStatus::Waiting)
        || data.navStatus == static_cast<quint16>(AgvController::NavStatus::Running)) {
        m_agvSeenMoving = true;
    }

    if (data.navStatus >= static_cast<quint16>(AgvController::NavStatus::Failed)
        && data.navStatus <= static_cast<quint16>(AgvController::NavStatus::Timeout)) {
        if (data.navStation != m_expectedLm) {
            return;
        }

        static const char *kStatusText[] = {"", "", "", "", "", "失败", "取消", "超时"};
        raiseSystemError(QStringLiteral("AGV 导航%1（navStatus=%2, expectedLm=%3）")
                             .arg(QString::fromUtf8(kStatusText[data.navStatus]))
                             .arg(data.navStatus)
                             .arg(m_expectedLm));
        return;
    }

    if (!m_agvSeenMoving) {
        return;
    }

    if (data.navStatus == static_cast<quint16>(AgvController::NavStatus::Arrived)
        && data.navStation == m_expectedLm
        && data.curStation == m_expectedLm) {
        m_agvTimeout->stop();
        emit logMessage(prefix(QStringLiteral("AGV"))
                        + QStringLiteral(" 已到达 LM%1").arg(m_expectedLm));

        switch (m_state) {
        case ExecState::AgvToPickup:
            enterState(ExecState::ArmPickup, QStringLiteral("机械臂开始取料"));
            break;
        case ExecState::AgvToUnload:
            enterState(ExecState::ArmUnload, QStringLiteral("机械臂开始倒料"));
            break;
        case ExecState::AgvToPallet:
            enterState(ExecState::PreparePalletPoint, QStringLiteral("校验码垛配置并计算下一个点"));
            break;
        default:
            break;
        }
    }
}

void TaskExecutor::onArmStageCompleted(const QString &stageName)
{
    if (!isBusy()) {
        return;
    }

    emit logMessage(prefix(QStringLiteral("ARM"))
                    + QStringLiteral(" %1 已完成").arg(stageName));

    switch (m_state) {
    case ExecState::ArmPickup:
    case ExecState::PreGripScan:
    case ExecState::RotateForScan:
        enterState(ExecState::StowAfterPickup, QStringLiteral("取料完成，机械臂收姿态"));
        break;
    case ExecState::StowAfterPickup:
        if (m_stationCfg->pickupLm == m_stationCfg->unloadLm) {
            emit logMessage(prefix(QStringLiteral("AGV"))
                            + QStringLiteral(" 取料位与倒料位同一 LM%1，跳过 AGV 导航，直接开始倒料")
                                  .arg(m_stationCfg->unloadLm));
            enterState(ExecState::ArmUnload, QStringLiteral("取料位与倒料位相同，机械臂开始倒料"));
            break;
        }

        startAgvStep(ExecState::AgvToUnload,
                     m_stationCfg->unloadLm,
                     QStringLiteral("AGV 前往倒料位 LM%1").arg(m_stationCfg->unloadLm));
        break;
    case ExecState::ArmUnload:
        enterState(ExecState::StowAfterUnload, QStringLiteral("倒料完成，机械臂收姿态"));
        break;
    case ExecState::StowAfterUnload:
        startAgvStep(ExecState::AgvToPallet,
                     m_palletCfg->palletLm,
                     QStringLiteral("AGV 前往码垛位 LM%1").arg(m_palletCfg->palletLm));
        break;
    case ExecState::StowAfterPallet:
        finishTaskSuccess();
        break;
    case ExecState::CleanupStow:
        finalizeTaskFailureAfterCleanup();
        break;
    default:
        break;
    }
}

void TaskExecutor::onArmStageError(const QString &reason)
{
    if (!isBusy()) {
        return;
    }

    if (m_state == ExecState::CleanupStow) {
        emit logMessage(prefix(QStringLiteral("ERROR"))
                        + QStringLiteral(" CLEANUP 安全收姿态失败，需升级为系统级 ERROR：%1")
                              .arg(reason));
        raiseSystemError(QStringLiteral("任务失败后的安全收姿态失败：%1").arg(reason));
        return;
    }

    emit logMessage(prefix(QStringLiteral("ERROR"))
                    + QStringLiteral(" ARM 阶段失败：%1").arg(reason));
    beginCleanupAfterTaskFailure(QStringLiteral("机械臂动作失败：%1").arg(reason));
}

void TaskExecutor::onPreGripScanRequested()
{
    if (!isBusy()) {
        return;
    }

    resetScanSearchState();
    m_scanAttempt = 1;
    requestScan(QStringLiteral("机械臂到达预夹取扫码点，等待首轮扫码结果"));
}

void TaskExecutor::onPreGripScanSearchMoveCompleted(double currentYOffsetMm)
{
    if (!isBusy()) {
        return;
    }

    if (m_state == ExecState::PreGripScanSearchReturn) {
        emit logMessage(prefix(QStringLiteral("SCAN"))
                        + QStringLiteral(" 已回到原夹取位置，条码=%1，继续夹紧")
                              .arg(m_pendingScanCodeAfterSearch));
        resetScanSearchState();
        setTaskRunning(TaskStep::GripAndLift, QStringLiteral("扫码通过，机械臂继续夹紧并抬升"));
        m_arm->continueAfterPreGripScan();
        return;
    }

    if (m_state != ExecState::PreGripScanSearchMove) {
        return;
    }

    emit logMessage(prefix(QStringLiteral("SCAN"))
                    + QStringLiteral(" 已到 Y=%1mm 搜码位置，重新扫码")
                          .arg(currentYOffsetMm, 0, 'f', 1));
    m_scanAttempt = m_scanSearchIndex + 1;
    requestScan(QStringLiteral("已到 Y 轴搜码位置，等待扫码结果"));
}

void TaskExecutor::onPreGripScanSearchMoveError(const QString &reason)
{
    if (!isBusy()) {
        return;
    }

    resetScanSearchState();
    beginCleanupAfterTaskFailure(QStringLiteral("扫码搜索移动失败：%1").arg(reason));
}

void TaskExecutor::onPreGripScanCaptureReturnCompleted()
{
    if (!isBusy() || m_state != ExecState::PreGripScanReturnCapture) {
        return;
    }

    finishScanSearchFailureAtCapture(
        QStringLiteral("扫码失败且 Y 轴搜码无结果：%1").arg(m_lastScanFailureDetail));
}

void TaskExecutor::onPreGripScanCaptureReturnError(const QString &reason)
{
    if (!isBusy()) {
        return;
    }

    resetScanSearchState();
    beginCleanupAfterTaskFailure(QStringLiteral("扫码失败后返回拍照位失败：%1").arg(reason));
}

void TaskExecutor::onToolRotationCompleted()
{
    if (!isBusy() || m_state != ExecState::RotateForScan) {
        return;
    }

    emit logMessage(prefix(QStringLiteral("SCAN"))
                    + QStringLiteral(" 工具旋转完成，开始第二轮扫码"));
    requestScan(QStringLiteral("工具旋转完成，等待第二轮扫码结果"));
}

void TaskExecutor::onToolRotationError(const QString &reason)
{
    if (!isBusy()) {
        return;
    }

    beginCleanupAfterTaskFailure(QStringLiteral("扫码补救旋转失败：%1").arg(reason));
}

void TaskExecutor::onPalletPlaceCompleted()
{
    if (!isBusy() || m_state != ExecState::ArmPalletPlace) {
        return;
    }

    m_state = ExecState::CommitPallet;
    setTaskRunning(TaskStep::CommitPallet, QStringLiteral("机械臂放置完成，提交码垛缓存"));

    QString error;
    if (!m_pallet->commitPlaced(m_stationCfg->palletArea, &error)) {
        raiseSystemError(QStringLiteral("commitPlaced() 失败：%1").arg(error));
        return;
    }

    emit logMessage(prefix(QStringLiteral("PALLET"))
                    + QStringLiteral(" 已提交 %1 的放置缓存")
                          .arg(palletAreaDisplayName(m_stationCfg->palletArea)));
    enterState(ExecState::StowAfterPallet, QStringLiteral("码垛提交完成，机械臂收姿态"));
}

void TaskExecutor::onPalletPlaceError(const QString &reason)
{
    if (!isBusy()) {
        return;
    }

    beginCleanupAfterTaskFailure(QStringLiteral("码垛放置失败：%1").arg(reason));
}

void TaskExecutor::onAgvTimeout()
{
    if (!isBusy()) {
        return;
    }

    raiseSystemError(QStringLiteral("AGV 导航到 LM%1 超时（%2 ms）")
                         .arg(m_expectedLm)
                         .arg(kAgvTimeoutMs));
}

void TaskExecutor::resetRuntimeState()
{
    m_agvTimeout->stop();
    m_state = ExecState::Idle;
    m_stationCfg = nullptr;
    m_palletCfg = nullptr;
    m_pendingPalletOffset = PalletPose{};
    m_expectedLm = 0;
    m_scanAttempt = 0;
    resetScanSearchState();
    m_agvSeenMoving = false;
    m_cleanupAfterTaskFailure = false;
    m_pendingTaskFailureReason.clear();
}

QString TaskExecutor::prefix(const QString &stage) const
{
    if (m_task.taskId == 0) {
        return QStringLiteral("[TaskExecutor][%1]").arg(stage);
    }
    return formatTaskPrefix(m_task, stage);
}

void TaskExecutor::setTaskRunning(TaskStep step, const QString &statusText)
{
    m_task.state = TaskState::Running;
    m_task.step = step;
    m_task.stepIndex = stepIndexForTaskStep(step);
    m_task.statusText = statusText;
    emit taskUpdated(m_task);
}

void TaskExecutor::setTaskTerminal(TaskState state,
                                   const QString &statusText,
                                   const QString &lastError)
{
    m_task.state = state;
    m_task.step = TaskStep::Done;
    m_task.stepIndex = stepIndexForTaskStep(TaskStep::Done);
    m_task.statusText = statusText;
    m_task.lastError = lastError;
    emit taskUpdated(m_task);
}

TaskStep TaskExecutor::taskStepForState(ExecState state) const
{
    switch (state) {
    case ExecState::AgvToPickup:
        return TaskStep::AgvToPickup;
    case ExecState::ArmPickup:
        return TaskStep::ArmPickup;
    case ExecState::PreGripScan:
    case ExecState::PreGripScanSearchMove:
    case ExecState::PreGripScanSearchReturn:
    case ExecState::PreGripScanReturnCapture:
    case ExecState::RotateForScan:
        return TaskStep::PreGripScan;
    case ExecState::StowAfterPickup:
        return TaskStep::StowAfterPickup;
    case ExecState::AgvToUnload:
        return TaskStep::AgvToUnload;
    case ExecState::ArmUnload:
        return TaskStep::ArmUnload;
    case ExecState::StowAfterUnload:
        return TaskStep::StowAfterUnload;
    case ExecState::AgvToPallet:
        return TaskStep::AgvToPallet;
    case ExecState::PreparePalletPoint:
        return TaskStep::PreparePalletPoint;
    case ExecState::ArmPalletPlace:
        return TaskStep::ArmPalletPlace;
    case ExecState::CommitPallet:
        return TaskStep::CommitPallet;
    case ExecState::StowAfterPallet:
    case ExecState::CleanupStow:
        return TaskStep::StowAfterPallet;
    case ExecState::Idle:
    default:
        return TaskStep::Waiting;
    }
}

int TaskExecutor::stepIndexForTaskStep(TaskStep step) const
{
    switch (step) {
    case TaskStep::Waiting:
        return 0;
    case TaskStep::AgvToPickup:
        return 1;
    case TaskStep::ArmPickup:
        return 2;
    case TaskStep::PreGripScan:
        return 3;
    case TaskStep::GripAndLift:
        return 4;
    case TaskStep::StowAfterPickup:
        return 5;
    case TaskStep::AgvToUnload:
        return 6;
    case TaskStep::ArmUnload:
        return 7;
    case TaskStep::StowAfterUnload:
        return 8;
    case TaskStep::AgvToPallet:
        return 9;
    case TaskStep::PreparePalletPoint:
        return 10;
    case TaskStep::ArmPalletPlace:
        return 11;
    case TaskStep::CommitPallet:
        return 12;
    case TaskStep::StowAfterPallet:
        return 13;
    case TaskStep::ReturningHome:
        return 14;
    case TaskStep::Done:
        return 15;
    }
    return 0;
}

bool TaskExecutor::isAgvNavigationState(ExecState state) const
{
    return state == ExecState::AgvToPickup
        || state == ExecState::AgvToUnload
        || state == ExecState::AgvToPallet;
}

bool TaskExecutor::resolveTaskConfigs()
{
    m_stationCfg = stationConfig(m_task.stationId);
    if (!m_stationCfg) {
        raiseSystemError(QStringLiteral("工位 %1 缺少 StationTaskConfig").arg(m_task.stationId));
        return false;
    }

    m_palletCfg = palletAreaConfig(m_stationCfg->palletArea);
    if (!m_palletCfg) {
        raiseSystemError(QStringLiteral("码垛区 %1 缺少 PalletAreaTaskConfig")
                             .arg(palletAreaDisplayName(m_stationCfg->palletArea)));
        return false;
    }

    return true;
}

void TaskExecutor::enterState(ExecState state, const QString &statusText)
{
    m_state = state;
    setTaskRunning(taskStepForState(state), statusText);

    switch (state) {
    case ExecState::ArmPickup:
        emit logMessage(prefix(QStringLiteral("ARM")) + QStringLiteral(" 启动阶段一取料"));
        m_arm->startStageOne();
        break;
    case ExecState::StowAfterPickup:
    case ExecState::StowAfterUnload:
    case ExecState::StowAfterPallet:
        emit logMessage(prefix(QStringLiteral("ARM")) + QStringLiteral(" 启动收姿态"));
        m_arm->startStow();
        break;
    case ExecState::ArmUnload:
        emit logMessage(prefix(QStringLiteral("ARM")) + QStringLiteral(" 启动倒料流程"));
        m_arm->startUnload();
        break;
    case ExecState::PreparePalletPoint:
        beginPalletPreparation();
        break;
    case ExecState::ArmPalletPlace:
        clearPalletStackingFlag();
        m_pallet->setStackingActive(m_stationCfg->palletArea, true);
        emit logMessage(prefix(QStringLiteral("PALLET"))
                        + QStringLiteral(" 启动放置动作 offset X=%1 Y=%2 Z=%3 Rz=%4")
                              .arg(m_pendingPalletOffset.x, 0, 'f', 1)
                              .arg(m_pendingPalletOffset.y, 0, 'f', 1)
                              .arg(m_pendingPalletOffset.z, 0, 'f', 1)
                              .arg(m_pendingPalletOffset.rz, 0, 'f', 1));
        m_arm->startPalletPlace(m_pendingPalletOffset);
        break;
    case ExecState::CleanupStow:
        emit logMessage(prefix(QStringLiteral("CLEANUP"))
                        + QStringLiteral(" 开始执行安全收姿态：%1")
                              .arg(m_pendingTaskFailureReason));
        m_arm->startStow();
        break;
    case ExecState::Idle:
    case ExecState::PreGripScan:
    case ExecState::PreGripScanSearchMove:
    case ExecState::PreGripScanSearchReturn:
    case ExecState::PreGripScanReturnCapture:
    case ExecState::RotateForScan:
    case ExecState::CommitPallet:
    case ExecState::AgvToPickup:
    case ExecState::AgvToUnload:
    case ExecState::AgvToPallet:
        break;
    }
}

void TaskExecutor::startAgvStep(ExecState state, int lm, const QString &statusText)
{
    m_expectedLm = lm;
    m_agvSeenMoving = false;
    m_state = state;
    setTaskRunning(taskStepForState(state), statusText);
    emit logMessage(prefix(QStringLiteral("AGV"))
                    + QStringLiteral(" 请求导航到 LM%1").arg(lm));
    m_agvTimeout->start(kAgvTimeoutMs);
    emit agvDispatchRequested(lm);
}

void TaskExecutor::requestScan(const QString &statusText)
{
    m_state = ExecState::PreGripScan;
    setTaskRunning(TaskStep::PreGripScan, statusText);

    NScanScheduler::ScanOptions options = defaultScanOptions();
    emit logMessage(prefix(QStringLiteral("SCAN"))
                    + QStringLiteral(" 发起第 %1 轮扫码请求（port=%2, timeout=%3ms, maxAttempts=%4）")
                          .arg(m_scanAttempt)
                          .arg(options.port)
                          .arg(options.timeoutMs)
                          .arg(options.maxAttempts));
    emit scanRequested(options);
}

NScanScheduler::ScanOptions TaskExecutor::defaultScanOptions() const
{
    NScanScheduler::ScanOptions options;
    options.port = 30000;
    options.timeoutMs = 2000;
    options.maxAttempts = 3;
    options.retryIntervalMs = 500;
    return options;
}

void TaskExecutor::beginPalletPreparation()
{
    QStringList errors;
    QStringList suggestions;
    if (!m_pallet->validateConfig(m_stationCfg->palletArea, &errors, &suggestions)) {
        raiseSystemError(QStringLiteral("码垛配置无效：%1").arg(errors.join(QStringLiteral("；"))));
        return;
    }

    if (!suggestions.isEmpty()) {
        emit logMessage(prefix(QStringLiteral("PALLET"))
                        + QStringLiteral(" 配置建议：%1").arg(suggestions.join(QStringLiteral("；"))));
    }

    QString error;
    if (!m_pallet->nextRelativeOffset(m_stationCfg->palletArea, &m_pendingPalletOffset, &error)) {
        raiseSystemError(QStringLiteral("码垛区无可用点位：%1").arg(error));
        return;
    }

    emit logMessage(prefix(QStringLiteral("PALLET"))
                    + QStringLiteral(" 已计算下一个放置点 offset X=%1 Y=%2 Z=%3 Rz=%4")
                          .arg(m_pendingPalletOffset.x, 0, 'f', 1)
                          .arg(m_pendingPalletOffset.y, 0, 'f', 1)
                          .arg(m_pendingPalletOffset.z, 0, 'f', 1)
                          .arg(m_pendingPalletOffset.rz, 0, 'f', 1));

    enterState(ExecState::ArmPalletPlace, QStringLiteral("机械臂执行码垛放置"));
}

void TaskExecutor::startNextScanSearchMove(const QString &failureDetail)
{
    static const double kTargets[] = { kScanSearchYOffsetMm, -kScanSearchYOffsetMm };
    const double target = kTargets[m_scanSearchIndex];
    ++m_scanSearchIndex;
    m_state = ExecState::PreGripScanSearchMove;
    setTaskRunning(TaskStep::PreGripScan,
                   QStringLiteral("扫码失败（%1），Y 轴搜码 %2/%3")
                       .arg(failureDetail)
                       .arg(m_scanSearchIndex)
                       .arg(kScanSearchTargetCount));
    emit logMessage(prefix(QStringLiteral("SCAN"))
                    + QStringLiteral(" 扫码失败：%1；移动到工具系 Y=%2mm 后重试")
                          .arg(failureDetail)
                          .arg(target, 0, 'f', 1));
    m_arm->movePreGripScanSearchTo(target);
}

void TaskExecutor::finishScanSearchFailureAtCapture(const QString &reason)
{
    resetScanSearchState();
    beginCleanupAfterTaskFailure(reason);
}

void TaskExecutor::resetScanSearchState()
{
    m_scanSearchIndex = 0;
    m_lastScanFailureDetail.clear();
    m_pendingScanCodeAfterSearch.clear();
}

void TaskExecutor::beginCleanupAfterTaskFailure(const QString &reason)
{
    if (!isBusy()) {
        return;
    }

    const QString failedStage = [this]() {
        switch (m_state) {
        case ExecState::AgvToPickup:
        case ExecState::AgvToUnload:
        case ExecState::AgvToPallet:
            return QStringLiteral("AGV");
        case ExecState::ArmPickup:
        case ExecState::ArmUnload:
        case ExecState::StowAfterPickup:
        case ExecState::StowAfterUnload:
        case ExecState::StowAfterPallet:
            return QStringLiteral("ARM");
        case ExecState::PreGripScan:
        case ExecState::PreGripScanSearchMove:
        case ExecState::PreGripScanSearchReturn:
        case ExecState::PreGripScanReturnCapture:
        case ExecState::RotateForScan:
            return QStringLiteral("SCAN");
        case ExecState::PreparePalletPoint:
        case ExecState::ArmPalletPlace:
        case ExecState::CommitPallet:
            return QStringLiteral("PALLET");
        case ExecState::CleanupStow:
            return QStringLiteral("CLEANUP");
        case ExecState::Idle:
        default:
            return QStringLiteral("TASK");
        }
    }();

    m_agvTimeout->stop();
    m_pendingTaskFailureReason = reason;
    m_cleanupAfterTaskFailure = true;
    emit logMessage(prefix(QStringLiteral("ERROR"))
                    + QStringLiteral(" 任务级失败：阶段=%1，原因=%2")
                          .arg(failedStage, reason));
    emit logMessage(prefix(QStringLiteral("CLEANUP"))
                    + QStringLiteral(" 开始安全收姿态，完成后允许继续下一任务"));
    enterState(ExecState::CleanupStow, QStringLiteral("任务级失败，机械臂执行安全收姿态"));
}

void TaskExecutor::finalizeTaskFailureAfterCleanup()
{
    const QString reason = m_pendingTaskFailureReason.isEmpty()
        ? QStringLiteral("任务失败")
        : m_pendingTaskFailureReason;

    emit logMessage(prefix(QStringLiteral("CLEANUP"))
                    + QStringLiteral(" 安全收姿态成功，任务失败收尾完成：%1").arg(reason));
    clearPalletStackingFlag();
    resetRuntimeState();
    setTaskTerminal(TaskState::Failed, QStringLiteral("任务失败并已完成安全收姿态"), reason);
    emit taskFailed(m_task, reason);
}

void TaskExecutor::finishTaskSuccess()
{
    clearPalletStackingFlag();
    resetRuntimeState();
    setTaskTerminal(TaskState::Succeeded, QStringLiteral("任务执行完成"), QString());
    emit taskSucceeded(m_task);
}

void TaskExecutor::raiseSystemError(const QString &reason)
{
    emit logMessage(prefix(QStringLiteral("ERROR"))
                    + QStringLiteral(" 系统级 ERROR：%1").arg(reason));
    clearPalletStackingFlag();
    m_agvTimeout->stop();

    if (m_agv) {
        m_agv->cancelNavigation();
    }
    if (m_arm) {
        m_arm->stop();
    }

    resetRuntimeState();
    setTaskTerminal(TaskState::Failed, QStringLiteral("系统级 ERROR：%1").arg(reason), reason);
    emit systemError(m_task, reason);
}

void TaskExecutor::clearPalletStackingFlag()
{
    if (m_pallet && m_stationCfg) {
        m_pallet->setStackingActive(m_stationCfg->palletArea, false);
    }
}

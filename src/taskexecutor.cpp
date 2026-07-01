#include "taskexecutor.h"

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
    // 单 AGV + 单机械臂只能串行执行；忙碌时再次 start 属于调度层不变量被破坏，
    // 因而不是普通任务失败，而是需要停止整线检查的系统级错误。
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

    // 任务启动时清除上一次可能残留的“正在码垛”标志，避免 UI/配置窗口误判。
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
    // worker 结果通过排队连接返回，可能在 Stop 或状态切换后迟到；只有当前确实
    // 等待扫码时才能消费，防止测试扫码或旧请求误推进机械臂。
    if (!isBusy() || m_state != ExecState::PreGripScan) {
        return;
    }

    const QString trimmedCode = result.code.trimmed();

    // 后续客户比对规则接入前，非空码默认通过，空码不可追溯所以失败。
    if (result.isSuccess() && !trimmedCode.isEmpty()) {
        emit logMessage(prefix(QStringLiteral("SCAN"))
                        + QStringLiteral(" 第 %1 轮扫码成功，条码=%2")
                              .arg(m_scanAttempt)
                              .arg(trimmedCode));
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

    if (m_scanAttempt <= 1) {
        // 安全策略：默认禁止首轮失败后的 180° 旋转，避免机械臂碰撞旁边人员或物体。
        // 此时按普通任务失败收尾并收安全姿态，不得先切换旋转状态或下发旋转命令。
        if (!kEnableScanFailureRotationRecovery) {
            emit logMessage(prefix(QStringLiteral("SCAN"))
                            + QStringLiteral(" 首轮失败：%1；扫码旋转补救开关已关闭，禁止旋转")
                                  .arg(failureDetail));
            beginCleanupAfterTaskFailure(
                QStringLiteral("首轮扫码失败且旋转补救已关闭：%1").arg(failureDetail));
            return;
        }

        // 仅在现场安全确认并显式开启开关后，才保留暂停中的 StageOne，执行旋转并发起第二轮扫码。
        m_scanAttempt = 2;
        m_state = ExecState::RotateForScan;
        setTaskRunning(TaskStep::PreGripScan,
                       QStringLiteral("首轮扫码失败（%1），旋转工具 180 度后重试").arg(failureDetail));
        emit logMessage(prefix(QStringLiteral("SCAN"))
                        + QStringLiteral(" 首轮失败：%1，进入旋转补救").arg(failureDetail));
        m_arm->rotateToolRz180();
        return;
    }

    beginCleanupAfterTaskFailure(QStringLiteral("两轮扫码均失败：%1").arg(failureDetail));
}

void TaskExecutor::onAgvMonitor(const AgvMonitorData &data)
{
    if (!isBusy()) {
        return;
    }

    if (!isAgvNavigationState(m_state)) {
        return;
    }

    if (data.navStatus == static_cast<quint16>(AgvController::NavStatus::Waiting)
        || data.navStatus == static_cast<quint16>(AgvController::NavStatus::Running)) {
        m_agvSeenMoving = true;
    }

    if (data.navStatus >= static_cast<quint16>(AgvController::NavStatus::Failed)
        && data.navStatus <= static_cast<quint16>(AgvController::NavStatus::Timeout)) {
        // 忽略其他/上一导航任务残留的失败状态，只处理当前 expectedLm 对应任务。
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

    // 禁止直接消费启动前残留的 Arrived(4)。本次必须先真实出现 Waiting/Running，
    // 才能证明后续 Arrived 属于刚刚发出的导航命令。
    if (!m_agvSeenMoving) {
        return;
    }

    // navStatus/navStation 只反映导航任务自身状态，可能残留上一单的“已到达”；
    // 必须同时检查 curStation，确认 AGV 物理位置也真正到达本次目标 LM。
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
            enterState(ExecState::PreparePalletPoint, QStringLiteral("校验码垛配置并计算下一点"));
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

    // HuayanScheduler 的完成信号是通用信号；业务推进只依赖当前 ExecState，
    // 不解析可变的中文 stageName，从而避免文案变化破坏状态机。
    switch (m_state) {
    case ExecState::ArmPickup:
    case ExecState::PreGripScan:
    case ExecState::RotateForScan:
        enterState(ExecState::StowAfterPickup, QStringLiteral("取料完成，机械臂收姿态"));
        break;
    case ExecState::StowAfterPickup:
        // 取料位与倒料位相同表示 AGV 在机械臂取料期间始终停留于目标站。
        // 若仍下发同站导航，AGV 通常不会重新出现 Waiting/Running，而严格到站
        // 判定又不会接受残留 Arrived，调度将一直等待。因此这里不创建导航任务、
        // 不启动导航超时，直接推进机械臂倒料；不同站点仍沿用严格导航校验。
        if (m_stationCfg->pickupLm == m_stationCfg->unloadLm) {
            emit logMessage(prefix(QStringLiteral("AGV"))
                            + QStringLiteral(" 取料位与倒料位同为 LM%1，跳过 AGV 导航，直接开始倒料")
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

    // CleanupStow 已是任务失败后的最后安全恢复手段；它再失败说明无法保证机械臂
    // 处于可运输姿态，必须升级为系统级 ERROR，禁止继续下一任务。
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

    m_scanAttempt = 1;
    requestScan(QStringLiteral("机械臂到达预夹取扫码点，等待首轮扫码结果"));
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

    // 先显式进入 CommitPallet，使日志/UI 能区分“机械臂已放好”和“缓存已推进”。
    m_state = ExecState::CommitPallet;
    setTaskRunning(TaskStep::CommitPallet, QStringLiteral("机械臂放置完成，提交码垛缓存"));

    QString error;
    // 只有机械臂到位并松爪成功后才能推进码垛缓存。
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
    // 只重置运行期字段；调用方会在 reset 后设置 m_task 终态并发送最终信号。
    m_agvTimeout->stop();
    m_state = ExecState::Idle;
    m_stationCfg = nullptr;
    m_palletCfg = nullptr;
    m_pendingPalletOffset = PalletPose{};
    m_expectedLm = 0;
    m_scanAttempt = 0;
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
        return TaskStep::StowAfterPallet;
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
    // 先发布任务状态，再启动设备动作；这样即使设备立即同步报错，日志/UI 也能
    // 准确显示错误发生在哪个业务步骤。
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
        // 这是任务级失败后的安全恢复路径；如果收姿态失败必须升级系统级 ERROR。
        emit logMessage(prefix(QStringLiteral("CLEANUP"))
                        + QStringLiteral(" 开始执行安全收姿态：%1")
                              .arg(m_pendingTaskFailureReason));
        m_arm->startStow();
        break;
    case ExecState::Idle:
    case ExecState::PreGripScan:
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
    // 每次导航都重新清除 SeenMoving，避免沿用上一段导航的运动证据。
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
    // ScanOptions 只包含协议参数；scannerIP 由 DeviceManager 在跨线程转发时填入。
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

    // nextRelativeOffset 只预览下一点，不改变 placedCount；真正提交在机械臂成功松爪后。
    QString error;
    if (!m_pallet->nextRelativeOffset(m_stationCfg->palletArea, &m_pendingPalletOffset, &error)) {
        raiseSystemError(QStringLiteral("码垛区无可用点位：%1").arg(error));
        return;
    }

    emit logMessage(prefix(QStringLiteral("PALLET"))
                    + QStringLiteral(" 已计算下一放置点 offset X=%1 Y=%2 Z=%3 Rz=%4")
                          .arg(m_pendingPalletOffset.x, 0, 'f', 1)
                          .arg(m_pendingPalletOffset.y, 0, 'f', 1)
                          .arg(m_pendingPalletOffset.z, 0, 'f', 1)
                          .arg(m_pendingPalletOffset.rz, 0, 'f', 1));

    enterState(ExecState::ArmPalletPlace, QStringLiteral("机械臂执行码垛放置"));
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
    // 允许收姿态成功后继续下一任务。
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
    // 系统级错误必须同时停 AGV、停机械臂并清理运行期状态；LineManager 收到
    // systemError 后负责清空 Pending 和阻止新任务入队。
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

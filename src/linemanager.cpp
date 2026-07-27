#include "linemanager.h"

#include <QTimer>
#include <utility>

#include "huayanScheduler.h"
#include "taskexecutor.h"

LineManager::LineManager(AgvController *agv,
                         HuayanScheduler *arm,
                         NScanScheduler *scanner,
                         PalletScheduler *pallet,
                         QObject *parent)
    : QObject(parent)
    , m_agv(agv)
    , m_arm(arm)
    , m_executor(new TaskExecutor(agv, arm, scanner, pallet, this))
    , m_returnHomeTimeout(new QTimer(this))
{
    m_returnHomeTimeout->setSingleShot(true);
    connect(m_returnHomeTimeout, &QTimer::timeout,
            this, &LineManager::onReturnHomeTimeout);

    connect(m_executor, &TaskExecutor::taskUpdated,
            this, &LineManager::onExecutorTaskUpdated);
    connect(m_executor, &TaskExecutor::taskSucceeded,
            this, &LineManager::onExecutorTaskSucceeded);
    connect(m_executor, &TaskExecutor::taskFailed,
            this, &LineManager::onExecutorTaskFailed);
    connect(m_executor, &TaskExecutor::systemError,
            this, &LineManager::onExecutorSystemError);
    connect(m_executor, &TaskExecutor::logMessage,
            this, &LineManager::logMessage);
    connect(m_executor, &TaskExecutor::agvDispatchRequested,
            this, &LineManager::agvDispatchRequested);
    connect(m_executor, &TaskExecutor::scanRequested,
            this, &LineManager::scanRequested);

    if (m_agv) {
        connect(m_agv, &AgvController::monitorUpdated,
                this, &LineManager::onAgvMonitorUpdated);
        connect(m_agv, &AgvController::errorOccurred,
                this, &LineManager::onAgvErrorOccurred);
    }

    emit systemStateChanged(m_state, m_stateText);
    emit queueChanged(queueSnapshot());
    emit currentTaskChanged(m_currentTask);
}

LineSystemState LineManager::state() const
{
    return m_state;
}

QList<Task> LineManager::queueSnapshot() const
{
    QList<Task> tasks;
    if (m_currentTask.taskId != 0 && m_currentTask.state == TaskState::Running) {
        tasks.append(m_currentTask);
    }

    const QList<Task> pending = m_queue.pendingSnapshot();
    for (const Task &task : pending) {
        tasks.append(task);
    }
    return tasks;
}

Task LineManager::currentTask() const
{
    return m_currentTask;
}

bool LineManager::chargeDispatchHeld() const
{
    return m_chargeDispatchHold;
}

void LineManager::applyRuntimeSettings(const RuntimeSettings &settings)
{
    Q_ASSERT(m_state == LineSystemState::Idle);
    Q_ASSERT(!m_executor->isBusy());
    m_executor->applyRuntimeSettings(settings);
}

void LineManager::setExternalWorkflowRunning(std::function<bool()> predicate)
{
    m_externalWorkflowRunning = std::move(predicate);
}

void LineManager::start()
{
    if (m_state == LineSystemState::Error) {
        emit logMessage(QStringLiteral("[LineManager] 系统处于报警状态，请先 Reset Error"));
        return;
    }

    if (m_state != LineSystemState::Idle) {
        emit logMessage(QStringLiteral("[LineManager] Start 被忽略：系统未处于 Idle"));
        return;
    }

    if (m_externalWorkflowRunning && m_externalWorkflowRunning()) {
        emit logMessage(QStringLiteral("[LineManager] Start 被拒绝：兼容整线流程正在运行"));
        return;
    }

    if (m_arm && m_arm->isBusy()) {
        emit logMessage(QStringLiteral("[LineManager] Start 被拒绝：机械臂正被单独测试占用"));
        return;
    }

    // Running 同时表示“执行任务”和“已启动但等待任务”；是否忙碌由 executor 区分。
    setState(LineSystemState::Running, QStringLiteral("等待缺料"));

    if (m_queue.hasPending()) {
        tryStartNext();
        return;
    }

    // LM1 是空闲待机点，不是每次任务链路的强制起点；只有当前没有待执行任务时才回站待机。
    returnHomeIfNeeded();
}

void LineManager::stop()
{
    if (m_chargeDispatchHold) {
        updateChargeDispatchHold(
            false,
            QStringLiteral("人工 Stop 清除充电派单保持：%1")
                .arg(m_chargeDispatchHoldReason),
            false);
    }

    if (m_state == LineSystemState::Error) {
        emit logMessage(QStringLiteral("[LineManager] Stop 被忽略：系统已在报警状态"));
        return;
    }

    const QString reason = QStringLiteral("人工 Stop 急停");

    // 现场需要人工介入，所以清空队列并禁止继续派单。
    if (m_currentTask.taskId != 0 && m_currentTask.state == TaskState::Running) {
        Task canceled = m_currentTask;
        canceled.state = TaskState::Canceled;
        canceled.step = TaskStep::Done;
        canceled.stepIndex = 15;
        canceled.statusText = QStringLiteral("人工 Stop，当前任务已取消");
        canceled.lastError = reason;
        setCurrentTask(canceled);
    } else {
        clearCurrentTask();
    }

    stopReturnHomeTracking();

    if (m_agv) {
        m_agv->cancelNavigation();
    }
    if (m_arm) {
        m_arm->stop();
    }

    if (m_executor->isBusy()) {
        m_manualStopInProgress = true;
        m_executor->stopForSystemError(reason);
        m_manualStopInProgress = false;
    }

    clearPendingForError(reason);
    setState(LineSystemState::Error, QStringLiteral("系统报警：%1").arg(reason));
    emit alarmRaised(reason);
    emit logMessage(QStringLiteral("系统报警：%1").arg(reason));
}

void LineManager::resetError()
{
    if (m_state != LineSystemState::Error) {
        emit logMessage(QStringLiteral("[LineManager] Reset Error 被忽略：当前不在 Error"));
        return;
    }

    stopReturnHomeTracking();
    if (m_chargeDispatchHold) {
        updateChargeDispatchHold(
            false,
            QStringLiteral("Error 复位清除充电派单保持"),
            false);
    }
    clearCurrentTask();
    setState(LineSystemState::Idle, QStringLiteral("未启动"));
    emit logMessage(QStringLiteral("[LineManager] 已复位到 Idle，等待重新 Start"));
}

void LineManager::setChargeDispatchHold(const bool hold, const QString &reason)
{
    // 这是自动充电与主调度之间的最小耦合点：只冻结新任务派发，
    // 不改变 FIFO 队列内容，也不接管正在执行的任务。
    updateChargeDispatchHold(hold, reason, true);
}

void LineManager::updateChargeDispatchHold(
    const bool hold, const QString &reason, const bool resumePending)
{
    const bool wasHeld = m_chargeDispatchHold;
    if (wasHeld == hold) {
        if (hold && !reason.trimmed().isEmpty())
            m_chargeDispatchHoldReason = reason.trimmed();
        return;
    }

    m_chargeDispatchHold = hold;
    m_chargeDispatchHoldReason = hold ? reason.trimmed() : QString();
    const QString actualReason =
        hold
            ? (m_chargeDispatchHoldReason.isEmpty()
                   ? QStringLiteral("自动充电请求")
                   : m_chargeDispatchHoldReason)
            : reason;
    emit logMessage(
        hold
            ? QStringLiteral("[LineManager] 已保持新任务派单：%1")
                  .arg(actualReason)
            : QStringLiteral("[LineManager] 已解除充电派单保持：%1").arg(reason));
    emit chargeDispatchHoldChanged(hold, actualReason);

    // 解除保持不负责启动主调度，也不能在返航或 Error 期间抢占状态；
    // 只有本来就在 Running 等待的队列才恢复既有 FIFO 入口。
    if (resumePending
        && wasHeld && !hold
        && m_state == LineSystemState::Running
        && !m_executor->isBusy()
        && m_queue.hasPending()) {
        tryStartNext();
    }
}

void LineManager::requestChargeReturnHome()
{
    // 仅在派单锁已经实际建立且当前任务执行器空闲时复用原返航逻辑，
    // 确保任务中的低电请求先完成当次任务，再返回 LM1。
    if (!m_chargeDispatchHold) {
        emit logMessage(QStringLiteral("[LineManager] 忽略未建立派单保持的充电返航请求"));
        return;
    }
    if (m_state != LineSystemState::Running || m_executor->isBusy())
        return;

    returnHomeIfNeeded();
}

void LineManager::raiseExternalSystemError(const QString &reason)
{
    if (m_state == LineSystemState::Error)
        return;

    const QString detail = reason.trimmed().isEmpty()
                               ? QStringLiteral("外部子系统发生未知故障")
                               : reason.trimmed();

    // 外部充电故障与人工 Stop 共享同一任务安全边界：先固定当前任务终态，
    // 再让 TaskExecutor 停止实际状态机。stopForSystemError() 会同步发出
    // taskUpdated/systemError，因此必须使用防重入标志，避免第二次 enterError()
    // 清队列和重复发布报警。
    if (m_currentTask.taskId != 0
        && m_currentTask.state == TaskState::Running) {
        Task canceled = m_currentTask;
        canceled.state = TaskState::Canceled;
        canceled.step = TaskStep::Done;
        canceled.stepIndex = 15;
        canceled.statusText = QStringLiteral("外部系统故障，当前任务已取消");
        canceled.lastError = detail;
        setCurrentTask(canceled);
    } else {
        clearCurrentTask();
    }

    if (m_executor->isBusy()) {
        m_manualStopInProgress = true;
        m_executor->stopForSystemError(detail);
        m_manualStopInProgress = false;
    }

    enterError(detail);
}

void LineManager::reportShortage(int stationId)
{
    if (stationId < 1 || stationId > 12) {
        emit logMessage(QStringLiteral("[LineManager] 忽略非法缺料工位：%1").arg(stationId));
        return;
    }

    if (m_state == LineSystemState::Error) {
        emit logMessage(QStringLiteral("[LineManager] 系统报警中，拒绝工位 %1 的缺料请求").arg(stationId));
        return;
    }

    // 每次点击都生成独立任务，不按工位去重；客户可能连续需要多个料箱。
    const Task task = m_queue.enqueue(stationId, TaskSource::UiMock);
    Q_UNUSED(task);

    emitQueueChanged();
    emit logMessage(QStringLiteral("工位%1已加入送料队列").arg(stationId));

    if (m_state == LineSystemState::Idle) {
        return;
    }

    if (m_state == LineSystemState::ReturningHome) {
        if (m_chargeDispatchHold) {
            // 低电量返航优先于后到任务；任务只追加到 FIFO，不能取消本次回 LM1。
            emit logMessage(QStringLiteral(
                "[LineManager] 充电返航途中收到新任务，保留返航并继续保持队列"));
            return;
        }
        // 新任务优先于空闲回站；取消回 LM1 是正常调度切换，不属于 AGV 故障。
        cancelReturnHomeForNewTask();
        tryStartNext();
        return;
    }

    if (m_state == LineSystemState::Running && !m_executor->isBusy()) {
        tryStartNext();
    }
}

void LineManager::onScanFinished(const NScanScheduler::ScanResult &result)
{
    m_executor->onScanFinished(result);
}

void LineManager::onExecutorTaskUpdated(const Task &task)
{
    if (m_manualStopInProgress) {
        return;
    }

    setCurrentTask(task);
    emitQueueChanged();
}

void LineManager::onExecutorTaskSucceeded(const Task &task)
{
    if (m_manualStopInProgress) {
        return;
    }

    emit logMessage(QStringLiteral("工位%1送料完成").arg(task.stationId));

    if (m_chargeDispatchHold) {
        // 当前任务已经安全结束；低电策略只禁止取下一单，并保留完整 FIFO 后返 LM1。
        clearCurrentTask();
        returnHomeIfNeeded();
        return;
    }

    // 连续任务之间不回 LM1，直接启动队首可减少无效往返。
    if (m_queue.hasPending()) {
        tryStartNext();
        return;
    }

    clearCurrentTask();
    returnHomeIfNeeded();
}

void LineManager::onExecutorTaskFailed(const Task &task, const QString &reason)
{
    if (m_manualStopInProgress) {
        return;
    }

    // 任务失败只影响当前单，不影响后续 FIFO 调度；后面有单就继续，没有单再回 LM1。
    emit logMessage(QStringLiteral("工位%1送料失败：%2").arg(task.stationId).arg(reason));

    if (m_chargeDispatchHold) {
        // 普通任务失败已由 TaskExecutor 完成安全收姿态，仍按低电优先规则返 LM1。
        clearCurrentTask();
        returnHomeIfNeeded();
        return;
    }

    // 能走到此信号说明 TaskExecutor 已成功完成安全收姿态，因此允许继续队列。
    if (m_queue.hasPending()) {
        tryStartNext();
        return;
    }

    clearCurrentTask();
    returnHomeIfNeeded();
}

void LineManager::onExecutorSystemError(const Task &task, const QString &reason)
{
    Q_UNUSED(task);

    if (m_manualStopInProgress) {
        return;
    }

    enterError(reason);
}

void LineManager::onAgvMonitorUpdated(const AgvMonitorData &data)
{
    m_lastAgvMonitor = data;
    m_hasAgvMonitor = true;

    if (!m_returnHomeActive) {
        return;
    }

    if (data.navStatus == static_cast<quint16>(AgvController::NavStatus::Waiting)
        || data.navStatus == static_cast<quint16>(AgvController::NavStatus::Running)) {
        m_returnHomeSeenMoving = true;
    }

    if (data.navStatus == static_cast<quint16>(AgvController::NavStatus::Failed)
        || data.navStatus == static_cast<quint16>(AgvController::NavStatus::Canceled)
        || data.navStatus == static_cast<quint16>(AgvController::NavStatus::Timeout)) {
        static const char *kStatusText[] = {"", "", "", "", "", "失败", "取消", "超时"};
        enterError(QStringLiteral("AGV 回 LM1 %1（navStatus=%2）")
                       .arg(QString::fromUtf8(kStatusText[data.navStatus]))
                       .arg(data.navStatus));
        return;
    }

    // 与任务导航相同：必须先看到本次回站开始运动，不能消费上一单残留 Arrived。
    if (!m_returnHomeSeenMoving) {
        return;
    }

    if (data.navStatus == static_cast<quint16>(AgvController::NavStatus::Arrived)
        && data.navStation == kHomeLm
        && data.curStation == kHomeLm) {
        stopReturnHomeTracking();
        setState(LineSystemState::Running, QStringLiteral("等待缺料"));
        emit logMessage(QStringLiteral("[LineManager] AGV 已回到 LM1，等待缺料"));
        if (!m_chargeDispatchHold && m_queue.hasPending())
            tryStartNext();
    }
}

void LineManager::onAgvErrorOccurred(const QString &message)
{
    if (!m_returnHomeActive) {
        return;
    }

    enterError(QStringLiteral("AGV 回 LM1 通信错误：%1").arg(message));
}

void LineManager::onReturnHomeTimeout()
{
    if (!m_returnHomeActive) {
        return;
    }

    const qint64 waitedMs = m_returnHomeElapsed.isValid()
        ? m_returnHomeElapsed.elapsed()
        : kReturnHomeTimeoutMs;
    enterError(QStringLiteral("AGV 回 LM1 超时：已等待 %1 ms（上限 %2 ms）")
                   .arg(waitedMs)
                   .arg(kReturnHomeTimeoutMs));
}

void LineManager::setState(LineSystemState state, const QString &text)
{
    m_state = state;
    m_stateText = text;
    emit systemStateChanged(m_state, m_stateText);
}

void LineManager::setCurrentTask(const Task &task)
{
    m_currentTask = task;
    emit currentTaskChanged(m_currentTask);
}

void LineManager::clearCurrentTask()
{
    m_currentTask = Task{};
    emit currentTaskChanged(m_currentTask);
}

void LineManager::emitQueueChanged()
{
    emit queueChanged(queueSnapshot());
}

void LineManager::tryStartNext()
{
    if (m_state == LineSystemState::Error || m_state == LineSystemState::Idle) {
        return;
    }

    // 必须在 takeNext() 之前检查：保持期间 FIFO 队首仍是 Pending，不能先取出再回滚。
    if (m_chargeDispatchHold)
        return;

    if (m_executor->isBusy() || !m_queue.hasPending()) {
        return;
    }

    if (m_returnHomeActive) {
        cancelReturnHomeForNewTask();
    }

    setState(LineSystemState::Running, QStringLiteral("正在送料"));

    // takeNext() 严格取 FIFO 队首，并把该副本标记为 Running。
    Task nextTask = m_queue.takeNext();
    emit logMessage(QStringLiteral("工位%1开始送料").arg(nextTask.stationId));
    m_executor->start(nextTask);
}

void LineManager::returnHomeIfNeeded()
{
    if (m_state == LineSystemState::Error || m_state == LineSystemState::Idle) {
        return;
    }

    if (m_executor->isBusy()
        || (!m_chargeDispatchHold && m_queue.hasPending())) {
        return;
    }

    if (!m_agv) {
        enterError(QStringLiteral("LineManager 未注入 AGV，无法回 LM1"));
        return;
    }

    // 已经物理位于 LM1 且没有活动导航时无需重复派单，直接保持等待缺料。
    if (m_hasAgvMonitor
        && m_lastAgvMonitor.curStation == kHomeLm
        && m_lastAgvMonitor.navStatus != static_cast<quint16>(AgvController::NavStatus::Waiting)
        && m_lastAgvMonitor.navStatus != static_cast<quint16>(AgvController::NavStatus::Running)) {
        setState(LineSystemState::Running, QStringLiteral("等待缺料"));
        emit logMessage(QStringLiteral("[LineManager] 当前已在 LM1 待机，保持等待缺料"));
        return;
    }

    clearCurrentTask();
    m_returnHomeActive = true;
    m_returnHomeSeenMoving = false;
    m_returnHomeElapsed.start();
    m_returnHomeTimeout->start(kReturnHomeTimeoutMs);
    setState(LineSystemState::ReturningHome, QStringLiteral("回待机点"));
    emit logMessage(QStringLiteral("[LineManager] 当前无待执行任务，AGV 返回 LM1 待机"));
    emit agvDispatchRequested(kHomeLm);
}

void LineManager::cancelReturnHomeForNewTask()
{
    if (!m_returnHomeActive) {
        return;
    }

    // 主动取消回 LM1 是正常切换，不能当 AGV fatal；真正的失败只看未被打断时的回站结果。
    emit logMessage(QStringLiteral("[LineManager] 回 LM1 途中收到新任务，主动取消回站并切换到送料"));
    stopReturnHomeTracking();
    if (m_agv) {
        m_agv->cancelNavigation();
    }
}

void LineManager::stopReturnHomeTracking()
{
    m_returnHomeTimeout->stop();
    m_returnHomeElapsed.invalidate();
    m_returnHomeActive = false;
    m_returnHomeSeenMoving = false;
}

void LineManager::enterError(const QString &reason)
{
    // 整线 Error 是设备安全边界：先停止所有在途动作，再清 Pending，最后通知 UI。
    if (m_chargeDispatchHold) {
        updateChargeDispatchHold(
            false,
            QStringLiteral("系统 Error 清除充电派单保持：%1")
                .arg(m_chargeDispatchHoldReason),
            false);
    }
    stopReturnHomeTracking();

    if (m_agv) {
        m_agv->cancelNavigation();
    }
    if (m_arm) {
        m_arm->stop();
    }

    // 现场需要人工介入，所以清空队列并禁止继续派单。
    clearPendingForError(reason);
    setState(LineSystemState::Error, QStringLiteral("系统报警：%1").arg(reason));
    emit alarmRaised(reason);
    emit logMessage(QStringLiteral("系统报警：%1").arg(reason));
}

void LineManager::clearPendingForError(const QString &reason)
{
    const int pendingCount = m_queue.pendingCount();
    if (pendingCount <= 0) {
        emitQueueChanged();
        return;
    }

    emit logMessage(QStringLiteral("[LineManager] 清空 %1 个 Pending 任务：%2")
                        .arg(pendingCount)
                        .arg(reason));
    m_queue.clearPendingAsCanceled(reason);
    emitQueueChanged();
}

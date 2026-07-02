#include "linemanager.h"

#include <QTimer>

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

    setState(LineSystemState::Running, QStringLiteral("等待缺料"));
    if (m_queue.hasPending()) {
        tryStartNext();
        return;
    }
    returnHomeIfNeeded();
}

void LineManager::stop()
{
    if (m_state == LineSystemState::Error) {
        emit logMessage(QStringLiteral("[LineManager] Stop 被忽略：系统已在报警状态"));
        return;
    }

    const QString reason = QStringLiteral("人工 Stop 急停");
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
    clearCurrentTask();
    setState(LineSystemState::Idle, QStringLiteral("未启动"));
    emit logMessage(QStringLiteral("[LineManager] 已复位到 Idle，等待重新 Start"));
}

bool LineManager::reportShortage(int stationId, TaskSource source)
{
    if (stationId < 1 || stationId > 12) {
        emit logMessage(QStringLiteral("[LineManager] 忽略非法缺料工位：%1").arg(stationId));
        return false;
    }

    if (m_state == LineSystemState::Error) {
        emit logMessage(QStringLiteral("[LineManager] 系统报警中，拒绝工位 %1 的缺料请求").arg(stationId));
        return false;
    }

    m_queue.enqueue(stationId, source);
    emitQueueChanged();
    emit logMessage(QStringLiteral("工位%1已加入送料队列，来源：%2")
                        .arg(stationId)
                        .arg(taskSourceText(source)));

    if (m_state == LineSystemState::Idle) {
        return true;
    }

    if (m_state == LineSystemState::ReturningHome) {
        cancelReturnHomeForNewTask();
        tryStartNext();
        return true;
    }

    if (m_state == LineSystemState::Running && !m_executor->isBusy()) {
        tryStartNext();
    }
    return true;
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

    emit logMessage(QStringLiteral("工位%1送料失败：%2").arg(task.stationId).arg(reason));
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
        enterError(QStringLiteral("AGV 回 LM1 失败，navStatus=%1").arg(data.navStatus));
        return;
    }

    if (!m_returnHomeSeenMoving) {
        return;
    }

    if (data.navStatus == static_cast<quint16>(AgvController::NavStatus::Arrived)
        && data.navStation == kHomeLm
        && data.curStation == kHomeLm) {
        stopReturnHomeTracking();
        setState(LineSystemState::Running, QStringLiteral("等待缺料"));
        emit logMessage(QStringLiteral("[LineManager] AGV 已回到 LM1，等待缺料"));
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
    enterError(QStringLiteral("AGV 回 LM1 超时：%1 ms").arg(kReturnHomeTimeoutMs));
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
    if (m_executor->isBusy() || !m_queue.hasPending()) {
        return;
    }
    if (m_returnHomeActive) {
        cancelReturnHomeForNewTask();
    }

    setState(LineSystemState::Running, QStringLiteral("正在送料"));
    Task nextTask = m_queue.takeNext();
    emit logMessage(QStringLiteral("工位%1开始送料").arg(nextTask.stationId));
    m_executor->start(nextTask);
}

void LineManager::returnHomeIfNeeded()
{
    if (m_state == LineSystemState::Error || m_state == LineSystemState::Idle) {
        return;
    }
    if (m_executor->isBusy() || m_queue.hasPending()) {
        return;
    }
    if (!m_agv) {
        enterError(QStringLiteral("LineManager 未注入 AGV，无法回 LM1"));
        return;
    }

    if (m_hasAgvMonitor
        && m_lastAgvMonitor.curStation == kHomeLm
        && m_lastAgvMonitor.navStatus != static_cast<quint16>(AgvController::NavStatus::Waiting)
        && m_lastAgvMonitor.navStatus != static_cast<quint16>(AgvController::NavStatus::Running)) {
        setState(LineSystemState::Running, QStringLiteral("等待缺料"));
        emit logMessage(QStringLiteral("[LineManager] 当前已在 LM1 待命，保持等待缺料"));
        return;
    }

    clearCurrentTask();
    m_returnHomeActive = true;
    m_returnHomeSeenMoving = false;
    m_returnHomeTimeout->start(kReturnHomeTimeoutMs);
    setState(LineSystemState::ReturningHome, QStringLiteral("回待机点"));
    emit logMessage(QStringLiteral("[LineManager] 当前无待执行任务，AGV 返回 LM1 待命"));
    emit agvDispatchRequested(kHomeLm);
}

void LineManager::cancelReturnHomeForNewTask()
{
    if (!m_returnHomeActive) {
        return;
    }

    emit logMessage(QStringLiteral("[LineManager] 回 LM1 途中收到新任务，主动取消回站并切换到送料"));
    stopReturnHomeTracking();
    if (m_agv) {
        m_agv->cancelNavigation();
    }
}

void LineManager::stopReturnHomeTracking()
{
    m_returnHomeTimeout->stop();
    m_returnHomeActive = false;
    m_returnHomeSeenMoving = false;
}

void LineManager::enterError(const QString &reason)
{
    stopReturnHomeTracking();
    if (m_agv) {
        m_agv->cancelNavigation();
    }
    if (m_arm) {
        m_arm->stop();
    }

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

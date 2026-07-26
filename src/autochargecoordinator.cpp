#include "autochargecoordinator.h"

namespace {

constexpr int kHomeLm = 1;
constexpr int kRoboshopCriticalBatteryPercent = 10;

bool navigationIsIdle(const AgvMonitorData &agv)
{
    // Paused/Failed/Canceled/Timeout 都不是可以安全启动推杆的“导航空闲”证明；
    // 仅无导航任务或已到达，并结合 curStation==LM1，才接受为启动条件。
    return agv.navStatus == static_cast<quint16>(AgvController::NavStatus::None)
           || agv.navStatus == static_cast<quint16>(AgvController::NavStatus::Arrived);
}

bool lineAllowsAutomaticPolicy(const LineSystemState state)
{
    return state != LineSystemState::Idle && state != LineSystemState::Error;
}

} // namespace

AutoChargeDecision decideAutoCharge(const AutoChargeInputs &inputs,
                                    const ChargeSettings &settings)
{
    AutoChargeDecision decision;

    // 自动关闭且没有遗留自动会话时必须首先旁路，连“解除保持”动作也不产生；
    // 有活动会话时则维持已有保持并只请求安全收尾，避免收尾完成前启动队首任务。
    if (!inputs.enabled) {
        if (inputs.automaticSessionActive) {
            decision.holdDispatch = true;
            decision.requestSafeStop = true;
            decision.statusText = QStringLiteral("自动充电已关闭，正在安全收尾");
        }
        return decision;
    }

    // 活动自动会话优先处理监控完整性、调度停止和双停止阈值。保持在整个
    // 停止/缩回/复位流程结束前持续为真，不能因已发出停止请求提前释放队列。
    if (inputs.automaticSessionActive) {
        decision.holdDispatch = true;

        if (!inputs.hasAgvMonitor) {
            decision.requestSafeStop = true;
            decision.raiseLineError = true;
            decision.statusText = QStringLiteral("自动充电监控丢失，正在安全收尾");
            decision.errorText = QStringLiteral("自动充电期间丢失 AGV 电量或位置监控");
            return decision;
        }

        if (inputs.chargeControllerUnknown) {
            decision.requestSafeStop = true;
            decision.raiseLineError = true;
            decision.statusText = QStringLiteral("充电桩状态未知，正在安全收尾");
            decision.errorText = QStringLiteral("自动充电期间充电桩状态未知");
            return decision;
        }

        if (!lineAllowsAutomaticPolicy(inputs.lineState)) {
            decision.requestSafeStop = true;
            decision.statusText = QStringLiteral("主调度已停止，正在安全收尾");
            return decision;
        }

        if (inputs.agv.battery <= kRoboshopCriticalBatteryPercent) {
            decision.errorText =
                QStringLiteral("AGV 电量已到 %1%，达到 Roboshop 10%严重报警线")
                    .arg(inputs.agv.battery);
        }

        if (inputs.pendingCount > 0
            && inputs.agv.battery >= settings.dispatchReadyPercent) {
            decision.requestSafeStop = true;
            decision.statusText =
                QStringLiteral("电量已达到允许接单阈值，正在安全收尾");
            return decision;
        }

        if (inputs.pendingCount == 0
            && inputs.agv.battery >= settings.stopChargePercent) {
            decision.requestSafeStop = true;
            decision.statusText =
                QStringLiteral("电量已达到正常停止阈值，正在安全收尾");
            return decision;
        }

        decision.statusText = inputs.pendingCount > 0
                                  ? QStringLiteral("有待执行任务，继续充电至允许接单电量")
                                  : QStringLiteral("无待执行任务，继续充电至正常停止电量");
        return decision;
    }

    // 只打开自动开关不会启动主调度；Idle/Error 下既不保持也不发起充电。
    if (!lineAllowsAutomaticPolicy(inputs.lineState)) {
        decision.statusText = inputs.lineState == LineSystemState::Idle
                                  ? QStringLiteral("等待主调度启动")
                                  : QStringLiteral("主调度处于错误状态");
        return decision;
    }

    // 未得到一轮完整 AGV 快照时不能判断电量和物理位置，因此先保持派单等待。
    if (!inputs.hasAgvMonitor) {
        decision.holdDispatch = true;
        decision.statusText = QStringLiteral("等待 AGV 电量和位置状态");
        return decision;
    }

    // 状态未知既不能视为空闲也不能开始新会话，并升级为主调度系统故障。
    if (inputs.chargeControllerUnknown) {
        decision.holdDispatch = true;
        decision.raiseLineError = true;
        decision.statusText = QStringLiteral("充电桩状态未知，禁止派单");
        decision.errorText = QStringLiteral("充电桩状态未知，必须人工检查");
        return decision;
    }

    if (inputs.agv.battery < settings.startChargePercent) {
        decision.holdDispatch = true;

        if (inputs.agv.battery <= kRoboshopCriticalBatteryPercent) {
            // 这里只生成严重报警详情，不请求 LineManager Error，保证正在执行的
            // 机械安全动作可以完成；holdDispatch 会阻止随后取出下一任务。
            decision.errorText =
                QStringLiteral("AGV 电量已到 %1%，达到 Roboshop 10%严重报警线")
                    .arg(inputs.agv.battery);
        }

        if (inputs.currentTaskRunning) {
            decision.statusText =
                QStringLiteral("电量低于启动阈值，等待当前任务安全完成");
            return decision;
        }

        if (inputs.agv.curStation != kHomeLm) {
            decision.requestReturnHome = true;
            decision.statusText =
                QStringLiteral("电量低于启动阈值，保持派单并返回 LM1");
            return decision;
        }

        if (!navigationIsIdle(inputs.agv)) {
            decision.statusText =
                QStringLiteral("AGV 位于 LM1，等待导航停止后开始充电");
            return decision;
        }

        if (inputs.chargeControllerBusy) {
            decision.statusText =
                QStringLiteral("AGV 位于 LM1，等待充电控制器空闲");
            return decision;
        }

        decision.requestStartCharge = true;
        decision.statusText = QStringLiteral("AGV 位于 LM1，申请自动充电");
        return decision;
    }

    decision.releaseDispatch = true;
    decision.statusText = QStringLiteral("电量正常，沿用现有主调度逻辑");
    return decision;
}

AutoChargeCoordinator::AutoChargeCoordinator(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<ChargePileController::State>("ChargePileController::State");
    qRegisterMetaType<ChargePileController::SessionOrigin>(
        "ChargePileController::SessionOrigin");
    qRegisterMetaType<ChargePileController::StopReason>(
        "ChargePileController::StopReason");
}

bool AutoChargeCoordinator::isEnabled() const
{
    return m_inputs.enabled;
}

bool AutoChargeCoordinator::automaticSessionActive() const
{
    return m_inputs.automaticSessionActive;
}

void AutoChargeCoordinator::setEnabled(const bool enabled)
{
    if (m_inputs.enabled != enabled) {
        m_inputs.enabled = enabled;
        m_startRejectedUntilInputChanges = false;
        emit logMessage(enabled ? QStringLiteral("自动充电模式已开启")
                                : QStringLiteral("自动充电模式已关闭"));
    }
    evaluate();
}

void AutoChargeCoordinator::applySettings(const ChargeSettings &settings)
{
    m_settings = settings;
    m_startRejectedUntilInputChanges = false;
    evaluate();
}

void AutoChargeCoordinator::onAgvMonitorUpdated(const AgvMonitorData &data)
{
    m_inputs.agv = data;
    m_inputs.hasAgvMonitor = true;
    m_monitorLostReason.clear();
    m_startRejectedUntilInputChanges = false;
    evaluate();
}

void AutoChargeCoordinator::onAgvMonitorLost(const QString &reason)
{
    m_inputs.hasAgvMonitor = false;
    m_monitorLostReason = reason;
    m_startRejectedUntilInputChanges = false;
    evaluate();
}

void AutoChargeCoordinator::onLineStateChanged(const LineSystemState state,
                                               const QString &text)
{
    m_inputs.lineState = state;
    m_lineStateText = text;
    m_startRejectedUntilInputChanges = false;
    evaluate();
}

void AutoChargeCoordinator::onQueueChanged(const QList<Task> &tasks)
{
    int pendingCount = 0;
    bool currentTaskRunning = false;
    for (const Task &task : tasks) {
        if (task.state == TaskState::Pending) {
            ++pendingCount;
        } else if (task.state == TaskState::Running) {
            currentTaskRunning = true;
        }
    }

    m_inputs.pendingCount = pendingCount;
    m_inputs.currentTaskRunning = currentTaskRunning;
    m_startRejectedUntilInputChanges = false;
    evaluate();
}

void AutoChargeCoordinator::onChargeControllerStateChanged(
    const ChargePileController::State state, const QString &text)
{
    m_inputs.chargeControllerBusy = isControllerBusyState(state);
    m_inputs.chargeControllerUnknown =
        state == ChargePileController::State::Unknown
        || state == ChargePileController::State::Fault;
    m_startRejectedUntilInputChanges = false;
    Q_UNUSED(text);
    evaluate();
}

void AutoChargeCoordinator::onAutomaticChargeStartResult(
    const bool accepted, const QString &message)
{
    if (!m_startIntentPending) {
        if (accepted) {
            emit logMessage(
                QStringLiteral("忽略没有对应自动启动意图的接受回执：%1").arg(message));
        }
        return;
    }

    m_startIntentPending = false;
    if (accepted) {
        m_inputs.automaticSessionActive = true;
        m_stopIntentIssued = false;
        emit logMessage(QStringLiteral("自动充电会话已由控制器接受"));
        evaluate();
        return;
    }

    // 同步拒绝后不在当前调用栈立即重发；下一次设备、队列或监控变化会解除锁存。
    m_startRejectedUntilInputChanges = true;
    emit logMessage(QStringLiteral("自动充电启动请求被拒绝：%1").arg(message));
}

void AutoChargeCoordinator::onChargeSessionFinished(
    const bool safe, const ChargePileController::SessionOrigin origin,
    const QString &message)
{
    if (origin != ChargePileController::SessionOrigin::Automatic) {
        return;
    }

    if (safe) {
        m_inputs.automaticSessionActive = false;
        // safe=true 是控制器已经确认无输出且缩到位的权威终态；清除先前由
        // unsafe 结果锁存的未知/忙碌状态，允许本轮自动保持在此后正常释放。
        m_inputs.chargeControllerUnknown = false;
        m_inputs.chargeControllerBusy = false;
        m_startIntentPending = false;
        m_stopIntentIssued = false;
        m_lineErrorIssued = false;
        emit logMessage(QStringLiteral("自动充电会话已安全完成：%1").arg(message));
        evaluate();
        return;
    }

    // unsafe 表示控制器尚未证明无输出且缩到位，必须继续持有自动会话所有权，
    // 保持队列并只通过同一控制器安全收尾，不把故障误当成会话已经结束。
    m_inputs.automaticSessionActive = true;
    // 将不安全终态锁存为“状态未知”，使后续重复输入仍落在同一故障分支，
    // 避免 evaluate() 因普通阈值分支过早清除 Error 边沿抑制。
    m_inputs.chargeControllerUnknown = true;
    if (!m_stopIntentIssued) {
        m_stopIntentIssued = true;
        emit automaticChargeSafeStopRequested(
            ChargePileController::StopReason::Fault);
    }
    if (!m_lineErrorIssued) {
        m_lineErrorIssued = true;
        emit lineErrorRequested(
            QStringLiteral("自动充电未安全完成：%1").arg(message));
    }
    evaluate();
}

void AutoChargeCoordinator::evaluate()
{
    const AutoChargeDecision decision = decideAutoCharge(m_inputs, m_settings);

    if (decision.statusText != m_lastDecisionText) {
        m_lastDecisionText = decision.statusText;
        emit decisionTextChanged(decision.statusText);
    }

    // 保持信号按电平变化发出。即使 disabled 决策本身是全假动作，这里也会
    // 撤销协调器过去设置的保持，从而恢复“关闭即零影响”的外部状态。
    if (decision.holdDispatch != m_dispatchHoldAsserted) {
        m_dispatchHoldAsserted = decision.holdDispatch;
        const QString reason = decision.holdDispatch
                                   ? decision.statusText
                                   : QStringLiteral("自动充电不再需要派单保持");
        emit dispatchHoldRequested(decision.holdDispatch, reason);
        emit logMessage(reason);
    }

    if (decision.requestReturnHome) {
        if (!m_returnIntentIssued) {
            m_returnIntentIssued = true;
            emit returnHomeRequested();
            emit logMessage(QStringLiteral("自动充电请求由主调度返回 LM1"));
        }
    } else {
        m_returnIntentIssued = false;
    }

    if (decision.requestStartCharge
        && !m_inputs.automaticSessionActive
        && !m_startIntentPending
        && !m_startRejectedUntilInputChanges) {
        m_startIntentPending = true;
        emit logMessage(QStringLiteral("自动充电请求开始充电"));
        emit automaticChargeStartRequested();
    }

    if (decision.requestSafeStop) {
        if (!m_stopIntentIssued) {
            m_stopIntentIssued = true;
            emit logMessage(QStringLiteral("自动充电请求安全收尾"));
            emit automaticChargeSafeStopRequested(currentStopReason());
        }
    } else if (!m_inputs.automaticSessionActive) {
        m_stopIntentIssued = false;
    }

    if (decision.raiseLineError) {
        if (!m_lineErrorIssued) {
            m_lineErrorIssued = true;
            const QString reason =
                decision.errorText.isEmpty()
                    ? QStringLiteral("自动充电发生系统故障")
                    : decision.errorText;
            emit lineErrorRequested(reason);
            emit logMessage(reason);
        }
    } else {
        m_lineErrorIssued = false;
    }

    const bool criticalBattery =
        m_inputs.enabled
        && lineAllowsAutomaticPolicy(m_inputs.lineState)
        && m_inputs.hasAgvMonitor
        && m_inputs.agv.battery <= kRoboshopCriticalBatteryPercent;
    if (criticalBattery) {
        if (!m_criticalAlarmIssued) {
            m_criticalAlarmIssued = true;
            const QString reason =
                QStringLiteral("AGV 电量 %1%，已达到 Roboshop 10%严重报警线，禁止下一任务")
                    .arg(m_inputs.agv.battery);
            emit criticalBatteryAlarm(reason);
            emit logMessage(reason);
        }
    } else {
        m_criticalAlarmIssued = false;
    }
}

ChargePileController::StopReason AutoChargeCoordinator::currentStopReason() const
{
    if (!m_inputs.enabled) {
        return ChargePileController::StopReason::AutomaticDisabled;
    }
    if (!m_inputs.hasAgvMonitor || m_inputs.chargeControllerUnknown) {
        return ChargePileController::StopReason::Fault;
    }
    if (!lineAllowsAutomaticPolicy(m_inputs.lineState)) {
        return ChargePileController::StopReason::LineStop;
    }
    if (m_inputs.pendingCount > 0
        && m_inputs.agv.battery >= m_settings.dispatchReadyPercent) {
        return ChargePileController::StopReason::AutomaticTaskReady;
    }
    return ChargePileController::StopReason::AutomaticTargetReached;
}

bool AutoChargeCoordinator::isControllerBusyState(
    const ChargePileController::State state)
{
    switch (state) {
    case ChargePileController::State::Idle:
    case ChargePileController::State::SafeComplete:
    case ChargePileController::State::Fault:
    case ChargePileController::State::Unknown:
        return false;
    case ChargePileController::State::Connecting:
    case ChargePileController::State::Prechecking:
    case ChargePileController::State::WritingParameters:
    case ChargePileController::State::ReadingBackParameters:
    case ChargePileController::State::SendingStart:
    case ChargePileController::State::WaitingForStart:
    case ChargePileController::State::Monitoring:
    case ChargePileController::State::SendingStop:
    case ChargePileController::State::WaitingForNoOutput:
    case ChargePileController::State::Retracting:
    case ChargePileController::State::WaitingForRetracted:
    case ChargePileController::State::Resetting:
        return true;
    }
    return true;
}

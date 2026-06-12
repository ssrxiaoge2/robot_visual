/**
 * @file lineorchestrator.cpp
 * @brief LineOrchestrator 整线流程编排器实现
 *
 * 推进机制：
 *   AGV 移动步 — emit agvDispatchRequested(工位号) → 监听 monitorUpdated 判到达
 *   机械臂步   — 调 HuayanScheduler::startXxx → 等 stageCompleted
 *   出错       — abort()：停机 + lineError
 * 完成判定靠自身 m_state（任意时刻只有一个子动作在飞），不靠 stageName 字符串。
 */

#include "lineorchestrator.h"
#include "huayanScheduler.h"

LineOrchestrator::LineOrchestrator(AgvController *agv,
                                   HuayanScheduler *arm,
                                   QObject *parent)
    : QObject(parent), m_agv(agv), m_arm(arm)
{
    connect(m_agv, &AgvController::monitorUpdated,
            this, &LineOrchestrator::onAgvMonitor);
    connect(m_agv, &AgvController::errorOccurred, this, [this](const QString &msg) {
        if (m_state == LineState::AgvToPickup || m_state == LineState::AgvToUnload
            || m_state == LineState::AgvReturnHome)
            abort(QStringLiteral("AGV 通信错误：%1").arg(msg));
    });
    connect(m_arm, &HuayanScheduler::stageCompleted,
            this, &LineOrchestrator::onArmCompleted);
    connect(m_arm, &HuayanScheduler::stageError,
            this, &LineOrchestrator::onArmError);

    m_agvTimeout = new QTimer(this);
    m_agvTimeout->setSingleShot(true);
    m_agvTimeout->setInterval(kAgvTimeoutMs);
    connect(m_agvTimeout, &QTimer::timeout, this, [this]() {
        abort(QStringLiteral("AGV 单步超时（%1s 未到达）").arg(kAgvTimeoutMs / 1000));
    });
}

void LineOrchestrator::setStations(int home, int pickup, int unload)
{
    m_homeStation = home;
    m_pickupStation = pickup;
    m_unloadStation = unload;
}

int LineOrchestrator::resolvedStation(int workstation) const
{
    return m_resolveStation ? m_resolveStation(workstation) : workstation;
}

void LineOrchestrator::start()
{
    if (m_state != LineState::Idle) return;
    emit lineStarted();
    emit lineLog(QStringLiteral("[整线] 流程启动，初始检查"));
    enterState(LineState::InitCheck);
}

void LineOrchestrator::stop()
{
    if (m_state == LineState::Idle) return;
    haltDevices();
    emit lineLog(QStringLiteral("[整线] 已手动停止"));
    emit lineStopped();
}

void LineOrchestrator::haltDevices()
{
    m_agvTimeout->stop();
    m_agv->cancelNavigation();
    m_arm->stop();
    m_state = LineState::Idle;
}

void LineOrchestrator::abort(const QString &reason)
{
    haltDevices();
    emit lineLog(QStringLiteral("[整线][错误] %1").arg(reason));
    emit lineError(reason);
}

/// 0-4 流程图节点：收姿态折进相邻 AGV 移动节点
void LineOrchestrator::emitStepForState(LineState s)
{
    int idx = -1;
    switch (s) {
    case LineState::AgvToPickup:     idx = 0; break;
    case LineState::ArmPicking:      idx = 1; break;
    case LineState::StowAfterPick:   idx = 2; break;
    case LineState::AgvToUnload:     idx = 2; break;
    case LineState::ArmUnloading:    idx = 3; break;
    case LineState::StowAfterUnload: idx = 4; break;
    case LineState::AgvReturnHome:   idx = 4; break;
    default:                         idx = -1; break;
    }
    if (idx >= 0)
        emit stepChanged(idx);
}

void LineOrchestrator::enterState(LineState s)
{
    m_state = s;
    emitStepForState(s);

    switch (s) {
    case LineState::InitCheck:
        // AGV 在站1（真检测）+ 强制机械臂收姿态
        if (!m_monitorValid) {
            abort(QStringLiteral("初检失败：AGV 监控数据未就绪，请等待 AGV 连接稳定后重试"));
            return;
        }
        if (m_lastMonitor.curStation != resolvedStation(m_homeStation)) {
            abort(QStringLiteral("初检失败：AGV 当前站 %1 ≠ 待机站 %2，请先归位")
                      .arg(m_lastMonitor.curStation).arg(resolvedStation(m_homeStation)));
            return;
        }
        emit lineLog(QStringLiteral("[整线] AGV 已在站%1，强制机械臂收姿态").arg(m_homeStation));
        m_arm->startStow();  // 完成 → onArmCompleted 推进到 AgvToPickup
        break;
    case LineState::AgvToPickup:
        emit lineLog(QStringLiteral("[整线] AGV 前往取料站 %1").arg(m_pickupStation));
        m_expectedStation = resolvedStation(m_pickupStation);
        m_agvSeenMoving = false;
        m_agvTimeout->start();
        emit agvDispatchRequested(m_pickupStation);
        break;
    case LineState::ArmPicking:
        emit lineLog(QStringLiteral("[整线] 机械臂取料"));
        m_arm->startStageOne();
        break;
    case LineState::StowAfterPick:
        emit lineLog(QStringLiteral("[整线] 取料后收姿态"));
        m_arm->startStow();
        break;
    case LineState::AgvToUnload:
        emit lineLog(QStringLiteral("[整线] AGV 前往倒料站 %1").arg(m_unloadStation));
        m_expectedStation = resolvedStation(m_unloadStation);
        m_agvSeenMoving = false;
        m_agvTimeout->start();
        emit agvDispatchRequested(m_unloadStation);
        break;
    case LineState::ArmUnloading:
        emit lineLog(QStringLiteral("[整线] 机械臂倒料"));
        m_arm->startUnload();
        break;
    case LineState::StowAfterUnload:
        emit lineLog(QStringLiteral("[整线] 倒料后收姿态"));
        m_arm->startStow();
        break;
    case LineState::AgvReturnHome:
        emit lineLog(QStringLiteral("[整线] AGV 返回待机站 %1").arg(m_homeStation));
        m_expectedStation = resolvedStation(m_homeStation);
        m_agvSeenMoving = false;
        m_agvTimeout->start();
        emit agvDispatchRequested(m_homeStation);
        break;
    case LineState::Idle:
        break;
    }
}

void LineOrchestrator::onAgvMonitor(const AgvMonitorData &d)
{
    m_lastMonitor = d;
    m_monitorValid = true;

    const bool waitingAgv = (m_state == LineState::AgvToPickup
                          || m_state == LineState::AgvToUnload
                          || m_state == LineState::AgvReturnHome);
    if (!waitingAgv) return;

    // 先观察到 AGV 真正进入导航(等待1/执行2)，再判到达/失败，避免上一单残留态误判
    if (d.navStatus == 1 || d.navStatus == 2)
        m_agvSeenMoving = true;
    if (!m_agvSeenMoving) return;

    // navStatus/navStation 只反映导航任务自身状态，可能在人工干预后残留"已到达"；
    // 必须结合 curStation（AGV 实际所在站，独立轮询得到）确认物理位置真正到达目标站
    if (d.navStatus == 4 && d.navStation == m_expectedStation
        && d.curStation == m_expectedStation) {
        m_agvTimeout->stop();
        switch (m_state) {
        case LineState::AgvToPickup: enterState(LineState::ArmPicking);   break;
        case LineState::AgvToUnload: enterState(LineState::ArmUnloading); break;
        case LineState::AgvReturnHome:
            emit lineLog(QStringLiteral("[整线] 回到待机站，流程完成"));
            m_state = LineState::Idle;
            emit lineFinished();
            break;
        default:
            break;
        }
    } else if (d.navStatus >= 5 && d.navStatus <= 7) {
        static const char *txt[] = {"", "", "", "", "", "失败", "取消", "超时"};
        abort(QStringLiteral("AGV 导航%1（状态 %2）")
                  .arg(QString::fromUtf8(txt[d.navStatus])).arg(d.navStatus));
    }
}

void LineOrchestrator::onArmCompleted(const QString &stageName)
{
    Q_UNUSED(stageName);  // 用自身状态判断，不靠字符串
    switch (m_state) {
    case LineState::InitCheck:       enterState(LineState::AgvToPickup);     break;
    case LineState::ArmPicking:      enterState(LineState::StowAfterPick);   break;
    case LineState::StowAfterPick:   enterState(LineState::AgvToUnload);     break;
    case LineState::ArmUnloading:    enterState(LineState::StowAfterUnload); break;
    case LineState::StowAfterUnload: enterState(LineState::AgvReturnHome);   break;
    default:
        break;
    }
}

void LineOrchestrator::onArmError(const QString &msg)
{
    if (m_state != LineState::Idle)
        abort(QStringLiteral("机械臂错误：%1").arg(msg));
}

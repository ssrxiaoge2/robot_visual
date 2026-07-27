#include "agvmonitorfreshnessguard.h"

#include "autochargecoordinator.h"

// 仅首次丢失产生边沿，避免断线定时检查重复触发同一自动停止意图。
bool AgvMonitorFreshnessGuard::markLost()
{
    if (!m_fresh)
        return false;
    m_fresh = false;
    return true;
}

bool notifyAgvMonitorLostIfFresh(
    AgvMonitorFreshnessGuard &freshness,
    AutoChargeCoordinator &coordinator,
    const QString &reason)
{
    // 只有确实发生 fresh→lost 时才转发，使 DeviceManager 的断线与超时
    // 路径共享同一失效语义，并禁止旧电量与站点快照继续参与决策。
    if (!freshness.markLost())
        return false;
    coordinator.onAgvMonitorLost(reason);
    return true;
}

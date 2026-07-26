#include "agvmonitorfreshnessguard.h"

#include "autochargecoordinator.h"

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
    if (!freshness.markLost())
        return false;
    coordinator.onAgvMonitorLost(reason);
    return true;
}

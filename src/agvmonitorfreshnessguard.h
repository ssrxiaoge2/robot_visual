#pragma once

#include <QString>

class AutoChargeCoordinator;

/**
 * @brief 记录 DeviceManager 最近一轮完整 AGV 监控快照是否仍可使用。
 *
 * 该对象不拥有定时器，也不保存电量内容；DeviceManager 的单次 QTimer 只负责
 * 触发超时，所有断线和超时都通过本对象做同一边沿去重。
 */
class AgvMonitorFreshnessGuard
{
public:
    /// 新完整快照到达后恢复为 fresh，允许下一次断线/超时产生一个丢失边沿。
    void markUpdated() { m_fresh = true; }
    /// 将 fresh 变为 lost；只有实际发生一次 true→false 变化时返回 true。
    bool markLost();
    bool hasFreshSnapshot() const { return m_fresh; }

private:
    bool m_fresh = false; ///< false 时旧电量、站点和导航状态均不得参与安全决策。
};

/**
 * @brief 若快照刚刚失效，则把同一个丢失边沿通知给自动充电协调器。
 *
 * 返回 false 表示已经处于 lost，调用方不得重复打印日志或重复触发安全收尾。
 */
bool notifyAgvMonitorLostIfFresh(
    AgvMonitorFreshnessGuard &freshness,
    AutoChargeCoordinator &coordinator,
    const QString &reason);

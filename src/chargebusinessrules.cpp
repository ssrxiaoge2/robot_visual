#include "chargebusinessrules.h"

QString manualChargeStartRejectionReason(
    const ManualChargeStartContext &context)
{
    if (context.lineState != LineSystemState::Idle)
        return QStringLiteral("主调度已启动，不能手动开始充电");
    if (!context.hasAgvMonitor || context.agv.curStation != 1)
        return QStringLiteral("AGV 未确认位于 LM1");

    // Paused、Failed、Canceled 和 Timeout 均不能作为“车辆已经静止”的安全证明；
    // 现场只接受没有导航任务或已到达两个明确状态。
    const quint16 navStatus = context.agv.navStatus;
    if (navStatus != static_cast<quint16>(AgvController::NavStatus::None)
        && navStatus != static_cast<quint16>(AgvController::NavStatus::Arrived)) {
        return QStringLiteral("AGV 正在导航或导航状态不允许开始充电");
    }
    if (context.controllerBusy)
        return QStringLiteral("充电控制器正在执行其他操作");
    if (context.controllerState == ChargePileController::State::Unknown
        || context.controllerState == ChargePileController::State::Fault) {
        return QStringLiteral("充电桩状态未知或存在故障，不能开始新会话");
    }
    if (context.automaticEnabled || context.automaticSessionActive)
        return QStringLiteral("自动充电模式已开启或自动会话尚未安全完成");
    return {};
}

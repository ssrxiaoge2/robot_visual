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
    if (context.controllerShutdownRequired) {
        return QStringLiteral(
            "充电控制器仍需完成安全恢复，不能开始新的手动充电会话");
    }
    if (context.controllerState == ChargePileController::State::Unknown
        || context.controllerState == ChargePileController::State::Fault) {
        return QStringLiteral("充电桩状态未知或存在故障，不能开始新会话");
    }
    if (context.automaticEnabled || context.automaticSessionActive)
        return QStringLiteral("自动充电模式已开启或自动会话尚未安全完成");
    return {};
}

QString automaticChargeEnableRejectionReason(
    const AutomaticChargeEnableContext &context)
{
    // shutdownRequired 比枚举状态更接近控制器内部真实安全上下文，必须优先判断。
    // 这样即使异步状态通知尚未刷新，授权入口也不会把“快照安全”误当成可开新会话。
    if (context.controllerShutdownRequired) {
        return QStringLiteral(
            "充电控制器仍需完成安全恢复，不能开启自动充电模式");
    }
    if (context.controllerBusy || context.automaticSessionActive) {
        return QStringLiteral(
            "手动查询或充电会话正在执行，不能开启自动模式");
    }
    if (context.controllerState == ChargePileController::State::Unknown
        || context.controllerState == ChargePileController::State::Fault) {
        return QStringLiteral(
            "充电桩状态未知或存在故障，不能开启自动模式");
    }
    return {};
}

QString manualChargePreflightRejectionReason(
    const ManualChargeStartContext &context)
{
    ManualChargeStartContext queryContext = context;
    queryContext.controllerShutdownRequired = false;
    return manualChargeStartRejectionReason(queryContext);
}

QString automaticChargeEnablePreflightRejectionReason(
    const AutomaticChargeEnableContext &context)
{
    AutomaticChargeEnableContext queryContext = context;
    queryContext.controllerShutdownRequired = false;
    return automaticChargeEnableRejectionReason(queryContext);
}

ChargePreflightOutcome classifyChargePreflightOutcome(
    const bool queryOk,
    const ChargePileController::State controllerState,
    const bool controllerShutdownRequired,
    const bool businessConditionsStillValid)
{
    if (!businessConditionsStillValid)
        return ChargePreflightOutcome::Canceled;
    if (!queryOk
        || controllerState != ChargePileController::State::SafeComplete
        || controllerShutdownRequired) {
        return ChargePreflightOutcome::DeviceSafetyFailure;
    }
    return ChargePreflightOutcome::Safe;
}

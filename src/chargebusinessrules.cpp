#include "chargebusinessrules.h"

// 手动充电入口的纯业务校验：这里只判断当前快照是否允许发起实时预检，
// 不访问设备，也不把旧快照当成充电桩最终安全证明。
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

// 自动授权开启前的纯业务校验。授权关闭不经过本函数，保证操作员始终可以
// 撤销自动策略，并由已有会话继续走统一的安全收尾。
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

// 一键预检开始前允许控制器带有 shutdownRequired；该标志必须由紧随其后的
// 实时查询重新确认，而不能在查询发起前直接拒绝并要求操作员另点“查询状态”。
QString manualChargePreflightRejectionReason(
    const ManualChargeStartContext &context)
{
    ManualChargeStartContext queryContext = context;
    queryContext.controllerShutdownRequired = false;
    return manualChargeStartRejectionReason(queryContext);
}

// 与手动入口相同，开启自动授权时先允许一次只读查询，再以查询结果决定
// 是否真正授予自动充电权限。
QString automaticChargeEnablePreflightRejectionReason(
    const AutomaticChargeEnableContext &context)
{
    AutomaticChargeEnableContext queryContext = context;
    queryContext.controllerShutdownRequired = false;
    return automaticChargeEnableRejectionReason(queryContext);
}

// 将异步查询结果归一为三类：业务条件变化属于正常取消，设备状态不安全
// 属于安全故障，只有实时查询成功且控制器明确安全时才允许继续后续动作。
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

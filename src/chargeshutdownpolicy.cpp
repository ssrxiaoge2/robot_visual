#include "chargeshutdownpolicy.h"

bool chargeCloseInterceptionRequired(
    const ChargeCloseInterceptionContext &context)
{
    if (!context.deviceManagerAvailable)
        return true;

    const bool realChargeResponsibility =
        context.chargeSessionActive
        || context.automaticSessionActive
        || context.do0OpeningOrActive
        || context.applicationShutdownInProgress;
    if (realChargeResponsibility)
        return true;

    if (context.controllerUnsafeTerminal)
        return true;

    if (context.controllerUnsafeEvidence)
        return true;

    return context.controllerShutdownRequired
           && context.automaticPolicyParticipatingInLine;
}

// 把窗口关闭请求转换为明确动作；策略对象本身不操作设备，只负责防止
// 历史收尾结果被后续控制器操作错误复用。
ChargeShutdownPolicy::CloseAction ChargeShutdownPolicy::onCloseRequested(
    const bool shutdownRequired, const bool controllerBusy)
{
    if (!shutdownRequired) {
        m_phase = Phase::Normal;
        return CloseAction::AcceptClose;
    }

    if (m_phase == Phase::ApplicationShutdownPending)
        return CloseAction::WaitForApplicationShutdown;

    if (m_phase == Phase::FailedExitEligible && !controllerBusy)
        return CloseAction::OfferForceExit;

    // SafeCloseQueued 遇到新的不安全上下文、恢复正在执行，或者普通状态第一次
    // 关闭，都必须建立新代次；旧 safe/failed 结果不能跨代沿用。
    ++m_generation;
    m_phase = Phase::ApplicationShutdownPending;
    return CloseAction::RequestApplicationShutdown;
}

// 只消费当前等待代次的收尾结果。safe 还必须与控制器不再要求收尾同时成立，
// 才能排队执行真正的窗口关闭。
bool ChargeShutdownPolicy::onApplicationShutdownFinished(
    const bool safe, const bool shutdownRequired)
{
    if (m_phase != Phase::ApplicationShutdownPending)
        return false;

    m_phase = safe && !shutdownRequired
                  ? Phase::SafeCloseQueued
                  : Phase::FailedExitEligible;
    return true;
}

void ChargeShutdownPolicy::onControllerOperationStarted()
{
    // ApplicationShutdownPending 中出现的忙碌就是该代收尾本身，不能误撤销；
    // 其余阶段的新操作会使历史 safe/failed 资格立即过期。
    if (m_phase != Phase::ApplicationShutdownPending)
        m_phase = Phase::ControllerOperationInProgress;
}

void ChargeShutdownPolicy::onControllerOperationFinished()
{
    if (m_phase == Phase::ControllerOperationInProgress)
        m_phase = Phase::Normal;
}

// 队列关闭执行前再次读取实时安全标志，避免收尾完成信号与关闭事件之间
// 设备状态发生变化时仍沿用旧许可。
bool ChargeShutdownPolicy::canRunQueuedClose(const bool shutdownRequired)
{
    if (m_phase != Phase::SafeCloseQueued)
        return false;
    if (shutdownRequired) {
        m_phase = Phase::Normal;
        return false;
    }
    return true;
}

void ChargeShutdownPolicy::onForceExitConfirmed()
{
    if (m_phase == Phase::FailedExitEligible)
        m_phase = Phase::Normal;
}

// 人工停止或恢复必须继承活动自动会话的来源，确保自动协调器能够收到
// 对应终态并释放派单锁；没有自动会话时按手动来源处理。
ChargePileController::SessionOrigin
selectStopRecoveryOrigin(const bool automaticSessionActive)
{
    return automaticSessionActive
               ? ChargePileController::SessionOrigin::Automatic
               : ChargePileController::SessionOrigin::Manual;
}

#include "chargeshutdownpolicy.h"

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

ChargePileController::SessionOrigin
selectStopRecoveryOrigin(const bool automaticSessionActive)
{
    return automaticSessionActive
               ? ChargePileController::SessionOrigin::Automatic
               : ChargePileController::SessionOrigin::Manual;
}

#pragma once

#include "chargepilecontroller.h"

#include <QtGlobal>

/**
 * @brief DeviceManager 的应用关闭冻结门禁。
 *
 * beginRequest() 必须先于关闭自动授权和请求控制器收尾；失败结果解除门禁，
 * 让操作员能够查询或发起保守恢复，成功结果则持续冻结到对象随窗口析构。
 */
class ChargeApplicationShutdownGate
{
public:
    void beginRequest() { m_blocksNewActions = true; }
    void finishRequest(bool safe)
    {
        if (!safe)
            m_blocksNewActions = false;
    }
    bool blocksNewActions() const { return m_blocksNewActions; }

private:
    bool m_blocksNewActions = false;
};

/**
 * @brief 主窗口关闭代次和强退资格的纯状态策略。
 *
 * 强退资格只由某一代已经结束且失败的应用关闭请求产生。任何后续控制器操作
 * 开始都会立即撤销旧资格；恢复在途再次关闭会建立新代次并请求应用关闭接管。
 */
class ChargeShutdownPolicy
{
public:
    enum class Phase {
        Normal,
        ApplicationShutdownPending,
        FailedExitEligible,
        ControllerOperationInProgress,
        SafeCloseQueued
    };

    enum class CloseAction {
        AcceptClose,
        RequestApplicationShutdown,
        WaitForApplicationShutdown,
        OfferForceExit
    };

    CloseAction onCloseRequested(bool shutdownRequired, bool controllerBusy);
    bool onApplicationShutdownFinished(bool safe, bool shutdownRequired);
    void onControllerOperationStarted();
    void onControllerOperationFinished();
    bool canRunQueuedClose(bool shutdownRequired);
    void onForceExitConfirmed();

    Phase phase() const { return m_phase; }
    quint64 generation() const { return m_generation; }
    bool canOfferForceExit() const {
        return m_phase == Phase::FailedExitEligible;
    }
    bool applicationShutdownPending() const {
        return m_phase == Phase::ApplicationShutdownPending;
    }

private:
    Phase m_phase = Phase::Normal;
    quint64 m_generation = 0;
};

/**
 * @brief 不安全终态后的人工“停止充电”应沿用自动会话所有权。
 *
 * 来源决定 AutoChargeCoordinator 是否接收最终安全结果并释放派单保持；
 * StopReason 仍可使用 Manual 表达本次恢复由操作员触发。
 */
ChargePileController::SessionOrigin
selectStopRecoveryOrigin(bool automaticSessionActive);

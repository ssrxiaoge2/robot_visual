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
 * @brief 关闭窗口时判断充电安全拦截所需的一次业务快照。
 *
 * controllerShutdownRequired 仍沿用 ChargePileController 的保守判断；其它字段
 * 用来说明是否已经存在真实充电责任，避免“仅缺少安全基线”被误当作必须收尾。
 */
struct ChargeCloseInterceptionContext
{
    bool deviceManagerAvailable = true; ///< DeviceManager/控制器边界是否可用于证明安全。
    bool controllerShutdownRequired = false; ///< 控制器通用停机判断是否仍保守要求收尾。
    bool controllerUnsafeTerminal = false; ///< 控制器是否已明确处于 Fault 或 Unknown。
    bool controllerUnsafeEvidence = false; ///< 只读快照或未知写命令是否已经证明不安全。
    bool chargeSessionActive = false; ///< 控制器是否持有手动/自动充电或恢复会话。
    bool automaticSessionActive = false; ///< 自动协调器是否仍拥有未安全释放的自动会话。
    bool do0OpeningOrActive = false; ///< DO0 是否正在置高或已作为充电许可保持。
    bool applicationShutdownInProgress = false; ///< 应用关闭收尾是否已经接管控制器。
    bool automaticPolicyParticipatingInLine = false; ///< 自动策略是否已参与主调度生命周期。
};

/**
 * @brief 判断关闭窗口是否必须因充电安全被拦截。
 *
 * 规则只放宽“没有真实充电责任、仅缺少安全基线”的窗口关闭场景；Fault、
 * Unknown、已知不安全快照、DO0 打开、活动会话和已参与主调度的自动策略
 * 仍按安全事实拦截。
 */
bool chargeCloseInterceptionRequired(
    const ChargeCloseInterceptionContext &context);

/**
 * @brief 主窗口关闭代次和强退资格的纯状态策略。
 *
 * 强退资格只由某一代已经结束且失败的应用关闭请求产生。任何后续控制器操作
 * 开始都会立即撤销旧资格；恢复在途再次关闭会建立新代次并请求应用关闭接管。
 */
class ChargeShutdownPolicy
{
public:
    /** @brief 主窗口一次关闭请求当前所处的资格阶段。 */
    enum class Phase {
        Normal,                       ///< 无关闭请求或历史资格已失效。
        ApplicationShutdownPending,   ///< 等待唯一控制器的应用关闭终态。
        FailedExitEligible,           ///< 本代收尾失败，可向操作员提供双重强退确认。
        ControllerOperationInProgress, ///< 人工查询/恢复在途，旧强退资格失效。
        SafeCloseQueued               ///< 本代已安全，等待队列事件再次复核后关闭。
    };

    /** @brief closeEvent 根据实时风险和当前代次应执行的动作。 */
    enum class CloseAction {
        AcceptClose,               ///< 实时状态已安全，可直接接受窗口关闭。
        RequestApplicationShutdown, ///< 建立新代次并请求控制器安全收尾。
        WaitForApplicationShutdown, ///< 同一代仍在途，仅保持窗口打开。
        OfferForceExit              ///< 本代已失败，允许显示人工双重确认。
    };

    /// 消费一次 closeEvent；历史安全/失败结果不能跨 generation 使用。
    CloseAction onCloseRequested(bool shutdownRequired, bool controllerBusy);
    /// 仅当前 ApplicationShutdownPending 代次能够消费控制器最终结果。
    bool onApplicationShutdownFinished(bool safe, bool shutdownRequired);
    /// 人工查询或恢复开始时撤销旧的安全关闭和强退资格。
    void onControllerOperationStarted();
    /// 普通控制器操作结束后恢复到无资格状态。
    void onControllerOperationFinished();
    /// 队列关闭真正执行前再次读取 shutdownRequired，防止间隙状态变化。
    bool canRunQueuedClose(bool shutdownRequired);
    /// 双重确认完成后清除强退资格，避免后续 closeEvent 沿用旧选择。
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
    Phase m_phase = Phase::Normal; ///< 当前关闭代次的唯一状态。
    quint64 m_generation = 0;      ///< 每次新建应用关闭请求时单调递增。
};

/**
 * @brief 不安全终态后的人工“停止充电”应沿用自动会话所有权。
 *
 * 来源决定 AutoChargeCoordinator 是否接收最终安全结果并释放派单保持；
 * StopReason 仍可使用 Manual 表达本次恢复由操作员触发。
 */
ChargePileController::SessionOrigin
selectStopRecoveryOrigin(bool automaticSessionActive);

#pragma once

#include "agvcontroller.h"
#include "chargepilecontroller.h"
#include "lineconfig.h"

#include <QString>

/**
 * @brief 手动充电开始前的完整业务门禁快照。
 *
 * 该结构不持有任何设备对象，字段必须由 DeviceManager 在同一 UI 线程从唯一
 * LineManager、AGV 快照、控制器和协调器采集。把判断提取为无副作用函数后，
 * 界面和测试都不会复制现场安全条件。
 */
struct ManualChargeStartContext
{
    LineSystemState lineState = LineSystemState::Idle;
    bool hasAgvMonitor = false;
    AgvMonitorData agv;
    bool controllerBusy = false;
    ChargePileController::State controllerState =
        ChargePileController::State::Idle;
    // true 表示控制器仍保留未完成/不确定的写入恢复上下文；即使界面缓存状态
    // 暂时显示 SafeComplete，也不得据此建立新的手动充电会话。
    bool controllerShutdownRequired = false;
    bool automaticEnabled = false;
    bool automaticSessionActive = false;
};

/**
 * @brief 返回手动充电不能开始的中文原因；空字符串表示业务门禁通过。
 *
 * 通过只代表可以向 ChargePileController 提交请求，不能替代控制器内部的
 * 预检、写入回读和安全状态机，也不代表充电桩已经开始输出。
 */
QString manualChargeStartRejectionReason(
    const ManualChargeStartContext &context);

/**
 * @brief 开启自动充电授权前的控制器安全快照。
 *
 * 关闭自动充电不使用此门禁，确保操作员始终能够撤销授权；该结构只约束从关闭
 * 到开启的方向，避免状态文本与控制器内部恢复上下文短暂不同步时误开自动策略。
 */
struct AutomaticChargeEnableContext
{
    bool controllerBusy = false;
    ChargePileController::State controllerState =
        ChargePileController::State::Idle;
    bool automaticSessionActive = false;
    // 控制器仍需安全收尾时必须优先拒绝，不允许仅凭枚举状态开启自动充电。
    bool controllerShutdownRequired = false;
};

/**
 * @brief 返回自动充电授权不能开启的中文原因；空字符串表示门禁通过。
 */
QString automaticChargeEnableRejectionReason(
    const AutomaticChargeEnableContext &context);

/**
 * @brief 只读预检完成后的分类结果。
 *
 * DeviceSafetyFailure 仅表示充电桩通信、协议或实时安全快照不可信；Canceled
 * 表示查询期间上层业务条件变化。只有前者在主调度自动开始场景升级为 Error。
 */
enum class ChargePreflightOutcome {
    Safe,
    DeviceSafetyFailure,
    Canceled
};

/**
 * @brief 判断手动开始是否允许发起只读预检。
 *
 * 与最终启动门禁相比，本门禁仅暂时忽略 shutdownRequired，使初始 Idle 状态
 * 可以通过一次现场只读查询建立安全事实；忙碌、故障、位置和模式冲突仍会拒绝。
 */
QString manualChargePreflightRejectionReason(
    const ManualChargeStartContext &context);

/**
 * @brief 判断自动授权是否允许发起只读基线预检。
 */
QString automaticChargeEnablePreflightRejectionReason(
    const AutomaticChargeEnableContext &context);

/**
 * @brief 根据查询结果、控制器实时终态和业务条件分类预检结果。
 *
 * 业务条件失效优先归类为 Canceled，避免操作员关闭授权或主调度停止的同时，
 * 一个迟到的查询失败被误升级为系统故障。
 */
ChargePreflightOutcome classifyChargePreflightOutcome(
    bool queryOk,
    ChargePileController::State controllerState,
    bool controllerShutdownRequired,
    bool businessConditionsStillValid);

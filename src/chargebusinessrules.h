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

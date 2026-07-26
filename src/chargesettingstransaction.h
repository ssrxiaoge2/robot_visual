#pragma once

#include "chargesettings.h"

#include <QString>

class AutoChargeCoordinator;
class ChargePileController;

/**
 * @brief 一次充电参数事务涉及的四个真实提交目标。
 *
 * settingsPath 是磁盘配置；controller 和 coordinator 是唯一运行对象；
 * deviceManagerSnapshot 指向 DeviceManager 对外公开的当前快照。所有指针必须
 * 在同一 UI 线程有效，函数不取得所有权。
 */
struct ChargeSettingsTransactionTargets
{
    QString settingsPath;
    ChargePileController *controller = nullptr;
    AutoChargeCoordinator *coordinator = nullptr;
    ChargeSettings *deviceManagerSnapshot = nullptr;
};

/**
 * @brief 将候选充电参数作为一个事务提交到磁盘和三份运行快照。
 *
 * 先执行控制器无副作用预检，再原子保存文件；控制器确认接受后才更新协调器和
 * DeviceManager 快照。若保存后控制器意外拒绝，会立即用旧快照原子回滚文件，
 * 且绝不更新协调器或 DeviceManager 快照。
 */
bool applyChargeSettingsTransaction(
    const ChargeSettingsTransactionTargets &targets,
    const ChargeSettings &candidate,
    QString *error);

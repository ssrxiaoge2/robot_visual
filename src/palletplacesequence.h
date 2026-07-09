#ifndef PALLETPLACESEQUENCE_H
#define PALLETPLACESEQUENCE_H

#include <QList>
#include <QString>

#include "palletscheduler.h"

#ifndef PALLET_GRIPPER_RELEASE_Z_OFFSET_MM
#define PALLET_GRIPPER_RELEASE_Z_OFFSET_MM 420.0
#endif

/**
 * @brief 单次空箱码垛的标准动作类型。
 *
 * 该枚举只描述产线标准顺序，不直接调用华研 SDK；HuayanScheduler 按这里的顺序下发真实命令。
 */
enum class PalletPlaceStepKind {
    ClampAtSafety,
    RunPalletBaseFunction,
    MoveXYAboveTarget,
    DescendToReleaseHeight,
    ReleaseGripper,
    LiftAfterRelease,
    RunStowFunction
};

/**
 * @brief 单个码垛动作步骤。
 *
 * offset 仅对相对移动步骤有效，单位 mm/deg；函数步骤由 HuayanScheduler 使用现有函数名执行。
 */
struct PalletPlaceStep {
    PalletPlaceStepKind kind = PalletPlaceStepKind::ClampAtSafety;
    PalletPose offset;
    QString description;
};

/**
 * @brief 根据目标层地面高度、基座离地高度和当前基准点 Z，生成一次空箱码垛标准动作。
 *
 * targetOffset.x/y/rz 是基准点到目标格的基坐标系相对偏移；targetOffset.z 是
 * 目标层表面离地高度。真实释放地面高度 = targetOffset.z + releaseZOffsetMm；
 * 释放点基座 Z = 释放地面高度 - robotBaseHeightFromGroundMm；
 * Z 下降量 = 释放点基座 Z + 夹爪释放点补偿 - palletBaseTcpZMm，通常为负值。
 * releaseZOffsetMm 小于 0 或 robotBaseHeightFromGroundMm 非正时返回空列表。
 */
QList<PalletPlaceStep> buildPalletPlaceSequence(const PalletPose &targetOffset,
                                                double releaseZOffsetMm,
                                                double robotBaseHeightFromGroundMm,
                                                double palletBaseTcpZMm);

#endif // PALLETPLACESEQUENCE_H

#ifndef PALLETPLACESEQUENCE_H
#define PALLETPLACESEQUENCE_H

#include <QList>
#include <QString>

#include "palletscheduler.h"

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
 * @brief 根据目标层中心偏移和目标层上方释放高度，生成一次空箱码垛标准动作。
 *
 * targetOffset 是 PalletScheduler::nextRelativeOffset() 输出的目标层中心偏移；
 * releaseZOffsetMm 是“目标层上方释放高度”。真实松爪 Z = targetOffset.z + releaseZOffsetMm。
 * releaseZOffsetMm 小于 0 时返回空列表，调用方必须 fail-closed。
 */
QList<PalletPlaceStep> buildPalletPlaceSequence(const PalletPose &targetOffset,
                                                double releaseZOffsetMm);

#endif // PALLETPLACESEQUENCE_H

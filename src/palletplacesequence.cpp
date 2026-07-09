#include "palletplacesequence.h"

QList<PalletPlaceStep> buildPalletPlaceSequence(const PalletPose &targetOffset,
                                                double releaseZOffsetMm,
                                                double robotBaseHeightFromGroundMm,
                                                double palletBaseTcpZMm)
{
    if (releaseZOffsetMm < 0.0 || robotBaseHeightFromGroundMm <= 0.0) {
        return {};
    }

    PalletPose xyOffset;
    xyOffset.x = targetOffset.x;
    xyOffset.y = targetOffset.y;
    xyOffset.rz = targetOffset.rz;

    const double releaseGroundZ = targetOffset.z + releaseZOffsetMm;

    PalletPose descendOffset;
    descendOffset.z = releaseGroundZ
        - robotBaseHeightFromGroundMm
        + PALLET_GRIPPER_RELEASE_Z_OFFSET_MM
        - palletBaseTcpZMm;

    PalletPose liftOffset;
    liftOffset.z = -descendOffset.z;

    return {
        {PalletPlaceStepKind::ClampAtSafety, {}, QStringLiteral("安全位夹紧，模拟空箱在夹爪上")},
        {PalletPlaceStepKind::RunPalletBaseFunction, {}, QStringLiteral("运动到码垛区上方基准点")},
        {PalletPlaceStepKind::MoveXYAboveTarget, xyOffset, QStringLiteral("先执行 XY 到目标列/行上方")},
        {PalletPlaceStepKind::DescendToReleaseHeight, descendOffset, QStringLiteral("Z 下降到目标层上方释放高度")},
        {PalletPlaceStepKind::ReleaseGripper, {}, QStringLiteral("松爪释放空箱")},
        {PalletPlaceStepKind::LiftAfterRelease, liftOffset, QStringLiteral("松爪后先 Z 抬升，避免横移擦碰")},
        {PalletPlaceStepKind::RunStowFunction, {}, QStringLiteral("回运行安全位 Func_yun_xing_zhong")},
    };
}

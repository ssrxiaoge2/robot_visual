#include "palletplacesequence.h"

QList<PalletPlaceStep> buildPalletPlaceSequence(const PalletPose &targetOffset,
                                                double releaseZOffsetMm,
                                                double robotBaseHeightFromGroundMm,
                                                double palletBaseTcpZMm,
                                                QString *error)
{
    if (releaseZOffsetMm < 0.0 || robotBaseHeightFromGroundMm <= 0.0) {
        if (error)
            *error = QStringLiteral("释放高度或机器人基座离地高度无效");
        return {};
    }

    PalletPose xyOffset;
    xyOffset.x = targetOffset.x;
    xyOffset.y = targetOffset.y;
    xyOffset.rz = targetOffset.rz;

    const double targetTcpZ = PalletScheduler::releaseTcpZ(
        targetOffset, releaseZOffsetMm, robotBaseHeightFromGroundMm);
    if (targetTcpZ > palletBaseTcpZMm) {
        if (error) {
            *error = QStringLiteral("目标 TCP Z=%1 高于码垛初始点位 TCP Z=%2，已拒绝执行；请降低最大层数、释放高度或修正现场高度参数")
                .arg(targetTcpZ, 0, 'f', 1)
                .arg(palletBaseTcpZMm, 0, 'f', 1);
        }
        return {};
    }

    PalletPose descendOffset;
    descendOffset.z = targetTcpZ - palletBaseTcpZMm;

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

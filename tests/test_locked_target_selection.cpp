#include "visionclient.h"

#include <QJsonArray>
#include <QJsonObject>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void requireTrue(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}

QJsonObject target(double x, double y, double depth, double angle = 0.0, double confidence = 0.95)
{
    return {
        {QStringLiteral("offset_mm"), QJsonObject{
             {QStringLiteral("x"), x},
             {QStringLiteral("y"), y}}},
        {QStringLiteral("depth_compensated"), depth},
        {QStringLiteral("angle"), angle},
        {QStringLiteral("confidence"), confidence}
    };
}

const VisionHttpClient::TargetCandidate &selected(const VisionHttpClient::TargetSelection &selection)
{
    requireTrue(selection.hasTarget(), "选择结果必须包含目标");
    return selection.candidates.at(selection.selectedCandidateIndex);
}

VisionHttpClient::TargetSelectionContext lockedContext()
{
    VisionHttpClient::TargetSelectionContext ctx;
    ctx.anchorEnabled = true;
    ctx.lockEnabled = true;
    return ctx;
}

VisionHttpClient::TargetSelectionContext trackingContext(double lockX,
                                                         double lockY,
                                                         int missingFrames = 0)
{
    VisionHttpClient::TargetSelectionContext ctx = lockedContext();
    ctx.hasPreviousAnchorTarget = true;
    ctx.previousAnchorX = lockX;
    ctx.previousAnchorY = lockY;
    ctx.lockMissingFrames = missingFrames;
    return ctx;
}

const float kIdentityHandEye[4][4] = {
    {1.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 1.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 1.0f}
};

} // namespace

int main()
{
    using Reason = VisionHttpClient::TargetSelectionReason;

    VisionHttpClient::TargetSelectionContext narrowRoi = lockedContext();
    narrowRoi.stationRoiHalfX = 20.0;
    narrowRoi.stationRoiHalfY = 20.0;
    const auto customRoiRejectsTarget = VisionHttpClient::selectTarget(QJsonArray{
        target(30.0, 0.0, 1100.0)
    }, narrowRoi, kIdentityHandEye);
    requireTrue(!customRoiRejectsTarget.hasTarget(),
                "自定义运行时 ROI 必须实际参与候选过滤");

    const auto initialFiltersOtherStationHighest = VisionHttpClient::selectTarget(QJsonArray{
        target(30.0, 0.0, 1100.0),
        target(700.0, 0.0, 800.0)
    }, lockedContext(), kIdentityHandEye);
    requireTrue(initialFiltersOtherStationHighest.hasTarget(),
                "初始锁定必须能在可信目标中选出目标");
    requireTrue(selected(initialFiltersOtherStationHighest).sourceIndex == 0,
                "旁边工位目标即使 Z 更高，只要锚点距离不可信，也不能参与初始锁定");
    requireTrue(initialFiltersOtherStationHighest.reason == Reason::LockInitialHighestLayer,
                "初始锁定单个最高层可信目标必须记录 LockInitialHighestLayer");

    const auto initialRejectsDiagonalCircleLeak = VisionHttpClient::selectTarget(QJsonArray{
        target(30.0, 0.0, 1100.0),
        target(320.0, -120.0, 800.0)
    }, lockedContext(), kIdentityHandEye);
    requireTrue(initialRejectsDiagonalCircleLeak.hasTarget(),
                "圆形范围泄漏回归测试必须能在本工位候选中选出目标");
    requireTrue(selected(initialRejectsDiagonalCircleLeak).sourceIndex == 0,
                "斜向偏出但圆形距离仍小于旧阈值的旁站高箱，必须被 X/Y 矩形锚点可信范围过滤");
    requireTrue(!initialRejectsDiagonalCircleLeak.candidates.at(1).trusted,
                "超出矩形锚点可信范围的候选必须标记为不可信，便于现场日志追溯");

    const auto initialFixedSide = VisionHttpClient::selectTarget(QJsonArray{
        target(20.0, 120.0, 900.0),
        target(25.0, -160.0, 940.0)
    }, lockedContext(), kIdentityHandEye);
    requireTrue(selected(initialFixedSide).sourceIndex == 1,
                "最高层同层两个箱子必须按固定侧规则选择，默认选择 raw Y 最小目标");
    requireTrue(initialFixedSide.reason == Reason::LockInitialFixedSide,
                "初始同层固定侧选择必须记录 LockInitialFixedSide");

    const auto trackingKeepsLockedTarget = VisionHttpClient::selectTarget(QJsonArray{
        target(36.0, -54.0, 1100.0),
        target(600.0, 0.0, 800.0)
    }, trackingContext(30.0, 50.0), kIdentityHandEye);
    requireTrue(selected(trackingKeepsLockedTarget).sourceIndex == 0,
                "闭环跟踪必须优先延续锁定目标，不能被旁边工位更高目标抢走");
    requireTrue(trackingKeepsLockedTarget.reason == Reason::LockTrackingTarget,
                "闭环延续锁定目标必须记录 LockTrackingTarget");

    const auto trackingStillPrefersHighestLayer = VisionHttpClient::selectTarget(QJsonArray{
        target(5.0, 0.0, 930.0),
        target(200.0, 0.0, 820.0)
    }, trackingContext(0.0, 0.0), kIdentityHandEye);
    requireTrue(trackingStillPrefersHighestLayer.hasTarget(),
                "闭环帧必须能在锁定范围内选出目标");
    requireTrue(selected(trackingStillPrefersHighestLayer).sourceIndex == 1,
                "闭环帧仍必须最高层优先，不能因为较低层目标 lockdist 更近就抓低层箱子");
    requireTrue(trackingStillPrefersHighestLayer.reason == Reason::LockTrackingTarget,
                "闭环最高层优先后仍应记录 LockTrackingTarget，保持现场日志语义稳定");

    const auto trackingPrefersNearestLockDistanceOverFixedSide = VisionHttpClient::selectTarget(QJsonArray{
        target(52.7, 2.3, 1363.0),
        target(9.1, 260.8, 1284.0),
        target(-211.3, 20.4, 1472.0)
    }, trackingContext(13.7, -232.7), kIdentityHandEye);
    requireTrue(trackingPrefersNearestLockDistanceOverFixedSide.hasTarget(),
                "闭环锁定帧必须在锁定范围内选出连续目标");
    requireTrue(selected(trackingPrefersNearestLockDistanceOverFixedSide).sourceIndex == 1,
                "闭环锁定后必须选择 lockdist 最小的候选，不能再被固定侧 Y 抢到另一个同层目标");
    requireTrue(trackingPrefersNearestLockDistanceOverFixedSide.reason == Reason::LockTrackingTarget,
                "闭环锁定后即使同层多目标，也必须记录 LockTrackingTarget");

    const auto trackingMissing = VisionHttpClient::selectTarget(QJsonArray{
        target(600.0, 0.0, 800.0)
    }, trackingContext(30.0, 50.0), kIdentityHandEye);
    requireTrue(!trackingMissing.hasTarget(),
                "闭环帧只识别到旁边工位目标时，不允许自动切换目标");
    requireTrue(trackingMissing.reason == Reason::LockTargetMissing,
                "锁定目标未连续丢失到上限前必须记录 LockTargetMissing");
    requireTrue(trackingMissing.lockMissingFrames == 1,
                "锁定目标本帧丢失后，选择结果必须给出最新连续丢失帧数");

    const auto trackingLost = VisionHttpClient::selectTarget(QJsonArray{
        target(600.0, 0.0, 800.0)
    }, trackingContext(30.0, 50.0, VISION_LOCK_MAX_MISSING_FRAMES - 1), kIdentityHandEye);
    requireTrue(!trackingLost.hasTarget(),
                "锁定目标连续丢失达到上限后必须失败关闭");
    requireTrue(trackingLost.reason == Reason::LockTargetLost,
                "锁定目标连续丢失达到上限必须记录 LockTargetLost");
    requireTrue(trackingLost.lockMissingFrames == VISION_LOCK_MAX_MISSING_FRAMES,
                "锁定目标丢失达到上限时必须返回上限帧数");

    const QString missingSummary = VisionHttpClient::formatTargetSelectionLog(trackingMissing);
    requireTrue(missingSummary.contains(QStringLiteral("锁定=("))
                    && missingSummary.contains(QStringLiteral("丢失=1/3"))
                    && missingSummary.contains(QStringLiteral("拒绝切换旁站目标")),
                "锁定目标丢失日志必须包含锁定坐标、丢失帧数和拒绝切换原因");
    requireTrue(!missingSummary.contains(QLatin1Char('\n')),
                "锁定目标日志必须保持单行");

    return 0;
}

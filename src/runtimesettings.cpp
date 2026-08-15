#include "runtimesettings.h"

#include <QtGlobal>

#include <cmath>

namespace {

bool finitePositive(double value)
{
    return std::isfinite(value) && value > 0.0;
}

bool finiteNonNegative(double value)
{
    return std::isfinite(value) && value >= 0.0;
}

void require(bool condition, const QString &message, QStringList *errors)
{
    if (!condition)
        errors->append(message);
}

} // namespace

RuntimeSettings RuntimeSettings::defaults()
{
    return RuntimeSettings{};
}

SettingsValidation validateRuntimeSettings(const RuntimeSettings &settings)
{
    QStringList errors;

    require(std::isfinite(settings.pickup.largeBasketGrabZClearanceMm),
            QStringLiteral("大篮筐抓取 Z 余量必须为有限数值"), &errors);
    require(std::isfinite(settings.pickup.purpleBasketGrabZClearanceMm),
            QStringLiteral("紫筐抓取 Z 余量必须为有限数值"), &errors);
    require(std::isfinite(settings.pickup.grabXCompensationMm),
            QStringLiteral("抓取 X 补偿必须为有限数值"), &errors);
    require(std::isfinite(settings.pickup.grabYCompensationMm),
            QStringLiteral("抓取 Y 补偿必须为有限数值"), &errors);

    require(finitePositive(settings.vision.xyToleranceMm),
            QStringLiteral("XY 收敛阈值必须为正数"), &errors);
    require(finitePositive(settings.vision.rzToleranceDeg),
            QStringLiteral("Rz 收敛阈值必须为正数"), &errors);
    require(settings.vision.maxFineCorrectionCount >= 0
                && settings.vision.maxFineCorrectionCount <= 2,
            QStringLiteral("联合精修正次数必须位于 0 到 2"), &errors);
    require(settings.vision.settleMs > 0,
            QStringLiteral("视觉稳定等待时间必须为正数"), &errors);
    require(finitePositive(settings.vision.largeRzJumpThresholdDeg),
            QStringLiteral("Rz 大角度阈值必须为正数"), &errors);
    require(finiteNonNegative(settings.vision.largeRzDeltaToleranceDeg),
            QStringLiteral("Rz 连续帧容差不能为负数"), &errors);
    require(settings.vision.maxLargeRzExecutions >= 0,
            QStringLiteral("Rz 大角度执行次数不能为负数"), &errors);
    require(finitePositive(settings.vision.stationRoiHalfXmm)
                && finitePositive(settings.vision.stationRoiHalfYmm),
            QStringLiteral("工位 ROI 范围必须为正数"), &errors);
    require(finitePositive(settings.vision.anchorMaxTrustXmm)
                && finitePositive(settings.vision.anchorMaxTrustYmm),
            QStringLiteral("锚点可信范围必须为正数"), &errors);
    require(finiteNonNegative(settings.vision.anchorSameLayerToleranceMm),
            QStringLiteral("锚点同层容差不能为负数"), &errors);
    require(finitePositive(settings.vision.anchorSwitchMaxXyMm),
            QStringLiteral("锚点切换距离必须为正数"), &errors);
    require(settings.vision.lockMaxMissingFrames >= 0,
            QStringLiteral("目标锁定丢帧数不能为负数"), &errors);
    require(finitePositive(settings.vision.lockTrackRadiusMm),
            QStringLiteral("目标锁定半径必须为正数"), &errors);
    require(finiteNonNegative(settings.vision.lockSameLayerToleranceMm),
            QStringLiteral("目标锁定同层容差不能为负数"), &errors);

    require(finitePositive(settings.depthDescent.triggerDepthMm),
            QStringLiteral("深度触发阈值必须为正数"), &errors);
    require(finitePositive(settings.depthDescent.stepMm),
            QStringLiteral("深度单次下探距离必须为正数"), &errors);
    require(finitePositive(settings.depthDescent.maxAccumulatedMm),
            QStringLiteral("深度最大累计下探距离必须为正数"), &errors);
    require(settings.depthDescent.stepMm <= settings.depthDescent.maxAccumulatedMm,
            QStringLiteral("深度单次下探距离不能大于最大累计下探距离"), &errors);

    require(finitePositive(settings.search.descendStepMm),
            QStringLiteral("搜索单次下移距离必须为正数"), &errors);
    require(finitePositive(settings.search.maxAccumulatedMm),
            QStringLiteral("搜索最大累计下移距离必须为正数"), &errors);
    require(settings.search.descendStepMm <= settings.search.maxAccumulatedMm,
            QStringLiteral("搜索单次下移距离不能大于最大累计下移距离"), &errors);
    require(settings.search.settleMs > 0,
            QStringLiteral("搜索视觉等待时间必须为正数"), &errors);

    require(settings.motion.speedPercent >= 1 && settings.motion.speedPercent <= 100,
            QStringLiteral("机械臂速度倍率必须在 1% 到 100% 之间"), &errors);
    require(finitePositive(settings.motion.velocity),
            QStringLiteral("相对移动速度必须为正数"), &errors);
    require(finitePositive(settings.motion.acceleration),
            QStringLiteral("相对移动加速度必须为正数"), &errors);
    require(finiteNonNegative(settings.motion.radius),
            QStringLiteral("运动过渡半径不能为负数"), &errors);
    require(std::isfinite(settings.motion.scanRecoveryRotationDeg),
            QStringLiteral("扫码补救旋转角必须为有限数值"), &errors);

    require(finitePositive(settings.safety.maxSingleXyAdjustMm),
            QStringLiteral("单次 XY 最大调整距离必须为正数"), &errors);
    require(finitePositive(settings.safety.maxZDescendMm),
            QStringLiteral("最大 Z 下探距离必须为正数"), &errors);
    require(settings.safety.maxZDescendMm >= settings.depthDescent.maxAccumulatedMm,
            QStringLiteral("最大 Z 下探距离不能小于深度自动下探累计上限"), &errors);
    require(settings.safety.maxZDescendMm >= settings.search.maxAccumulatedMm,
            QStringLiteral("最大 Z 下探距离不能小于搜索累计上限"), &errors);
    require(settings.safety.normalMotionTimeoutMs > 0,
            QStringLiteral("普通运动超时必须为正数"), &errors);
    require(settings.safety.longZMotionTimeoutMs > 0,
            QStringLiteral("长距离 Z 运动超时必须为正数"), &errors);
    require(settings.safety.commandReadyTimeoutMs > 0,
            QStringLiteral("命令就绪超时必须为正数"), &errors);
    require(settings.safety.resetSettleMs > 0,
            QStringLiteral("复位稳定等待时间必须为正数"), &errors);
    require(settings.safety.pollIntervalMs > 0,
            QStringLiteral("轮询周期必须为正数"), &errors);
    require(settings.safety.shortMotionFallbackMs > 0,
            QStringLiteral("短动作判定时间必须为正数"), &errors);

    return {errors.isEmpty(), errors};
}

RuntimeSettings restoreCategoryDefaults(const RuntimeSettings &current,
                                        SettingsCategory category)
{
    RuntimeSettings restored = current;
    const RuntimeSettings defaults = RuntimeSettings::defaults();

    switch (category) {
    case SettingsCategory::Pickup:
        restored.pickup = defaults.pickup;
        break;
    case SettingsCategory::VisionClosedLoop:
        restored.vision = defaults.vision;
        break;
    case SettingsCategory::DepthDescent:
        restored.depthDescent = defaults.depthDescent;
        break;
    case SettingsCategory::Search:
        restored.search = defaults.search;
        break;
    case SettingsCategory::Motion:
        restored.motion = defaults.motion;
        break;
    case SettingsCategory::SafetyAndTimeouts:
        restored.safety = defaults.safety;
        break;
    }

    return restored;
}

DepthDescentDecision decideDepthDescent(
    double depthMm,
    double accumulatedMm,
    const RuntimeSettings::DepthDescent &settings)
{
    if (!settings.enabled || depthMm <= settings.triggerDepthMm)
        return {DepthDescentDecision::Action::ContinuePickup, 0.0};

    const double remaining = settings.maxAccumulatedMm - accumulatedMm;
    if (remaining <= 0.0)
        return {DepthDescentDecision::Action::FailLimitReached, 0.0};

    return {DepthDescentDecision::Action::MoveDown,
            qMin(settings.stepMm, remaining)};
}

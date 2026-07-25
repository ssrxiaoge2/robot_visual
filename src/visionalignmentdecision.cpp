#include "visionalignmentdecision.h"

namespace {

/**
 * @brief 在调试构建中拒绝不符合窗口协议的策略。
 *
 * 8 秒是联合对准观察的硬上限；调用方可缩短窗口，但不能延长到该上限之外。
 */
void assertValidPolicy(const VisionAlignment::WindowPolicy &policy)
{
    Q_ASSERT(policy.xyToleranceMm > 0.0);
    Q_ASSERT(policy.rzToleranceDeg > 0.0);
    Q_ASSERT(policy.maxFineCorrectionCount >= 0
             && policy.maxFineCorrectionCount <= 2);
    Q_ASSERT(policy.minElapsedMs >= 0);
    Q_ASSERT(policy.maxElapsedMs > policy.minElapsedMs);
    Q_ASSERT(policy.maxElapsedMs <= 8000);
}

} // namespace

bool VisionAlignment::isPlanarAligned(const Sample &sample,
                                      const WindowPolicy &policy)
{
    assertValidPolicy(policy);

    return sample.targetValid
        && qAbs(sample.xMm) <= policy.xyToleranceMm
        && qAbs(sample.yMm) <= policy.xyToleranceMm
        && qAbs(sample.rzDeg) <= policy.rzToleranceDeg;
}

VisionAlignment::ToolCorrection
VisionAlignment::toToolCorrection(const Sample &sample)
{
    // 工具系与视觉系的约定：X 同向，Y 与绕 Z 轴旋转方向相反。
    return {sample.xMm, -sample.yMm, -sample.rzDeg};
}

VisionAlignment::WindowDecision
VisionAlignment::decideWindow(const WindowInput &input,
                              const WindowPolicy &policy)
{
    assertValidPolicy(policy);

    // 失去锁定目标时必须 fail-closed，不能继续等待或下发历史偏差的精修命令。
    if (!input.latest.targetValid) {
        return {WindowAction::Stop, {}, QStringLiteral("锁定目标无效或已丢失")};
    }

    const bool aligned = isPlanarAligned(input.latest, policy);

    // 最小观察时间到达前只积累稳定性证据，避免过早下探或精修。
    if (input.elapsedMs < policy.minElapsedMs)
        return {WindowAction::ContinueObserving, {}, QString()};

    if (aligned && input.zStable)
        return {WindowAction::Descend, {}, QString()};

    // 到达硬窗口终点时，只有已同时满足平面对准和 Z 稳定的情况才能越过此前分支下探。
    if (input.elapsedMs >= policy.maxElapsedMs) {
        return {WindowAction::Stop, {},
                QStringLiteral("观察窗口达到8秒仍未同时满足对准与Z稳定")};
    }

    if (!aligned
        && input.completedFineCorrectionCount < policy.maxFineCorrectionCount) {
        return {WindowAction::FineCorrect, toToolCorrection(input.latest), QString()};
    }

    if (!aligned) {
        return {WindowAction::Stop, {}, QStringLiteral("联合精修剩余次数已耗尽")};
    }

    return {WindowAction::ContinueObserving, {}, QString()};
}

#include "visionalignmentdecision.h"

#include <algorithm>
#include <cmath>

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

VisionAlignment::StableDepthEvaluation
VisionAlignment::evaluateStableDepth(const QList<double> &samples,
                                     int requiredSamples,
                                     double maxCoreRangeMm)
{
    Q_ASSERT(requiredSamples >= 3);
    Q_ASSERT(requiredSamples % 2 == 1);
    Q_ASSERT(maxCoreRangeMm > 0.0);

    StableDepthEvaluation result;
    if (samples.size() != requiredSamples)
        return result;

    QList<double> sortedSamples = samples;
    for (double sample : sortedSamples) {
        // 视觉客户端正常路径已经拒绝非有限数值；这里仍保持纯判定函数自身完备，
        // 防止未来其他调用方把 NaN/Inf 排序后误判为稳定深度。
        if (!std::isfinite(sample))
            return result;
    }
    std::sort(sortedSamples.begin(), sortedSamples.end());

    const qsizetype medianIndex = sortedSamples.size() / 2;
    result.filteredZMm = sortedSamples.at(medianIndex);
    // 窗口固定为奇数且至少 3 帧：去掉排序后的一个最小值和一个最大值，
    // 中间核心样本仍必须全部落入原有稳定极差，不能用平均值掩盖双峰跳变。
    result.coreRangeMm =
        sortedSamples.at(sortedSamples.size() - 2) - sortedSamples.at(1);
    result.stable = result.coreRangeMm <= maxCoreRangeMm;
    return result;
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

    // 8 秒是不可越过的硬截止点。边界时刻即使本帧刚好同时满足对准和 Z 稳定，
    // 也必须停止，避免视觉回调与超时定时器的事件先后顺序改变机械臂最终动作。
    if (input.elapsedMs >= policy.maxElapsedMs) {
        return {WindowAction::Stop, {},
                QStringLiteral("观察窗口达到8秒硬上限")};
    }

    const bool aligned = isPlanarAligned(input.latest, policy);

    // 最小观察时间到达前只积累稳定性证据，避免过早下探或精修。
    if (input.elapsedMs < policy.minElapsedMs)
        return {WindowAction::ContinueObserving, {}, QString()};

    if (aligned && input.zStable)
        return {WindowAction::Descend, {}, QString()};

    if (!aligned
        && input.completedFineCorrectionCount < policy.maxFineCorrectionCount) {
        return {WindowAction::FineCorrect, toToolCorrection(input.latest), QString()};
    }

    if (!aligned) {
        return {WindowAction::Stop, {}, QStringLiteral("联合精修剩余次数已耗尽")};
    }

    return {WindowAction::ContinueObserving, {}, QString()};
}

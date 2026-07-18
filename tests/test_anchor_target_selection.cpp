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

void requireNear(double actual, double expected, double tolerance, const char *message)
{
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << message << " actual=" << actual << " expected=" << expected << std::endl;
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

VisionHttpClient::TargetSelectionContext context(double accumulatedX = 0.0,
                                                 double accumulatedY = 0.0)
{
    VisionHttpClient::TargetSelectionContext ctx;
    ctx.anchorEnabled = true;
    ctx.accumulatedToolX = accumulatedX;
    ctx.accumulatedToolY = accumulatedY;
    return ctx;
}

const float kIdentityHandEye[4][4] = {
    {1.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 1.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 1.0f}
};

const float kRotatedTranslatedHandEye[4][4] = {
    {0.0f, -1.0f, 0.0f, 10.0f},
    {1.0f, 0.0f, 0.0f, -20.0f},
    {0.0f, 0.0f, 1.0f, 30.0f},
    {0.0f, 0.0f, 0.0f, 1.0f}
};

} // namespace

int main()
{
    using Reason = VisionHttpClient::TargetSelectionReason;

    const auto highestInsideAnchor = VisionHttpClient::selectTarget(QJsonArray{
        target(20.0, -30.0, 1000.0),
        target(50.0, -40.0, 900.0)
    }, context(), kIdentityHandEye);
    requireTrue(selected(highestInsideAnchor).sourceIndex == 1,
                "锚点可信范围内必须优先选择最高层目标");
    requireTrue(highestInsideAnchor.reason == Reason::AnchorHighestLayer,
                "单个可信最高层必须记录 AnchorHighestLayer");

    const auto sameLayerNearestAnchor = VisionHttpClient::selectTarget(QJsonArray{
        target(250.0, 0.0, 900.0),
        target(80.0, 0.0, 910.0)
    }, context(), kIdentityHandEye);
    requireTrue(selected(sameLayerNearestAnchor).sourceIndex == 1,
                "最高层同层范围内必须选择离初始锚点 XY 最近的目标");
    requireTrue(sameLayerNearestAnchor.reason == Reason::AnchorSameLayerNearest,
                "同层锚点择近必须记录 AnchorSameLayerNearest");

    const auto accumulatedAnchor = VisionHttpClient::selectTarget(QJsonArray{
        target(10.0, 20.0, 900.0)
    }, context(100.0, -50.0), kIdentityHandEye);
    requireNear(selected(accumulatedAnchor).anchorX, 110.0, 0.001,
                "anchorX 必须等于已完成 X 位移 + 对准 X");
    requireNear(selected(accumulatedAnchor).anchorY, -70.0, 0.001,
                "anchorY 必须等于已完成 Y 位移 + 对准 Y，Y 使用现有阶段一取反方向");

    const auto transformedAnchor = VisionHttpClient::selectTarget(QJsonArray{
        target(7.0, 3.0, 100.0, 135.0)
    }, context(100.0, -50.0), kRotatedTranslatedHandEye);
    requireNear(selected(transformedAnchor).toolX, 7.0, 0.001,
                "非 identity 手眼矩阵必须按行主序计算 toolX，并使用第 4 列平移");
    requireNear(selected(transformedAnchor).toolY, -13.0, 0.001,
                "非 identity 手眼矩阵必须按行主序计算 toolY，并保持轴/符号");
    requireNear(selected(transformedAnchor).toolZ, 130.0, 0.001,
                "非 identity 手眼矩阵必须按行主序计算 toolZ，并使用第 4 列平移");
    requireNear(selected(transformedAnchor).toolRz, -45.0, 0.001,
                "toolRz 必须沿用 [-90,90] 角度规范化");
    requireNear(selected(transformedAnchor).alignmentY, 13.0, 0.001,
                "alignmentY 必须等于 -toolY");
    requireNear(selected(transformedAnchor).anchorX, 107.0, 0.001,
                "anchorX 必须等于累计 X + toolX");
    requireNear(selected(transformedAnchor).anchorY, -37.0, 0.001,
                "anchorY 必须等于累计 Y + alignmentY");

    VisionHttpClient::TargetSelectionContext farCtx = context();
    farCtx.maxTrustDistance = 200.0;
    const auto farHighest = VisionHttpClient::selectTarget(QJsonArray{
        target(350.0, 0.0, 800.0),
        target(20.0, 0.0, 1000.0)
    }, farCtx, kIdentityHandEye);
    requireTrue(!farHighest.hasTarget(),
                "最高层目标离初始锚点过远时必须 fail-closed，不允许改抓低层近目标");
    requireTrue(farHighest.reason == Reason::AnchorDistanceTooFar,
                "最高目标过远必须记录 AnchorDistanceTooFar");

    const auto defaultFarHighest = VisionHttpClient::selectTarget(QJsonArray{
        target(451.0, 0.0, 800.0),
        target(20.0, 0.0, 1000.0)
    }, context(), kIdentityHandEye);
    requireTrue(!defaultFarHighest.hasTarget(),
                "默认可信阈值下，旁边工位/全局最高目标过远时必须 fail-closed");
    requireTrue(defaultFarHighest.reason == Reason::AnchorDistanceTooFar,
                "默认可信阈值下，最高目标过远必须记录 AnchorDistanceTooFar");

    VisionHttpClient::TargetSelectionContext jumpCtx = context();
    jumpCtx.hasPreviousAnchorTarget = true;
    jumpCtx.previousAnchorX = 0.0;
    jumpCtx.previousAnchorY = 0.0;
    jumpCtx.maxSwitchDistance = 100.0;
    const auto jump = VisionHttpClient::selectTarget(QJsonArray{
        target(180.0, 0.0, 800.0)
    }, jumpCtx, kIdentityHandEye);
    requireTrue(!jump.hasTarget(),
                "闭环目标相对上一帧锚点位置跳变过大时必须 fail-closed");
    requireTrue(jump.reason == Reason::AnchorTargetJumpTooFar,
                "目标跳变过大必须记录 AnchorTargetJumpTooFar");

    const QString summary = VisionHttpClient::formatTargetSelectionLog(sameLayerNearestAnchor);
    requireTrue(summary.contains(QStringLiteral("anchor=("))
                    && summary.contains(QStringLiteral("dist="))
                    && summary.contains(QStringLiteral("可信")),
                "锚点选择日志必须包含 anchor 坐标、距离和可信状态");
    requireTrue(!summary.contains(QLatin1Char('\n')),
                "视觉选择摘要必须保持单行，避免现场日志刷屏");

    VisionHttpClient::TargetSelectionContext logCtx = context();
    logCtx.maxTrustDistance = 100.0;
    const auto perCandidateTrustLog = VisionHttpClient::selectTarget(QJsonArray{
        target(10.0, 0.0, 900.0),
        target(150.0, 0.0, 1000.0)
    }, logCtx, kIdentityHandEye);
    requireTrue(perCandidateTrustLog.hasTarget(),
                "日志可信状态测试需要选中近处最高层候选");
    requireTrue(perCandidateTrustLog.candidates.at(0).trusted,
                "近处候选必须标记为可信");
    requireTrue(!perCandidateTrustLog.candidates.at(1).trusted,
                "远处候选即使不是最终 best，也必须按候选自身锚点距离标记为不可信");
    const QString perCandidateSummary = VisionHttpClient::formatTargetSelectionLog(perCandidateTrustLog);
    requireTrue(perCandidateSummary.contains(QStringLiteral("#1 x=150.0 y=0.0 z=1000.0 范围内 tool=(150.0,0.0,1000.0) anchor=(150.0,0.0) dist=150.0 不可信")),
                "日志必须按每个候选自身距离输出可信/不可信，不能只反映最终 best");
    requireTrue(!perCandidateSummary.contains(QLatin1Char('\n')),
                "逐候选可信状态日志必须保持单行");

    return 0;
}

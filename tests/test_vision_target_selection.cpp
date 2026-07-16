#include "visionclient.h"

#include <QJsonArray>
#include <QJsonObject>

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

QJsonObject target(double x,
                   double y,
                   double depth,
                   double angle = 0.0,
                   double confidence = 0.95)
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

const VisionHttpClient::TargetCandidate &selected(
    const VisionHttpClient::TargetSelection &selection)
{
    requireTrue(selection.hasTarget(), "选择结果必须包含目标");
    return selection.candidates.at(selection.selectedCandidateIndex);
}

} // namespace

int main()
{
    using Reason = VisionHttpClient::TargetSelectionReason;

    const auto currentStationWins = VisionHttpClient::selectTarget(QJsonArray{
        target(501.0, 0.0, 100.0),
        target(120.0, 80.0, 300.0)
    });
    requireTrue(selected(currentStationWins).sourceIndex == 1,
                "矩形外目标即使更高也必须被排除");

    const auto highestLayerWins = VisionHttpClient::selectTarget(QJsonArray{
        target(5.0, 5.0, 400.0),
        target(300.0, 300.0, 300.0)
    });
    requireTrue(selected(highestLayerWins).sourceIndex == 1,
                "当前工位内必须先选择最高层，而不是最近中心");
    requireTrue(highestLayerWins.reason == Reason::HighestLayer,
                "单个最高层目标必须记录 HighestLayer 原因");

    const auto sameLayerNearest = VisionHttpClient::selectTarget(QJsonArray{
        target(250.0, 250.0, 300.0),
        target(40.0, 30.0, 315.0)
    });
    requireTrue(selected(sameLayerNearest).sourceIndex == 1,
                "20mm 同层范围内必须选择 XY 最近中心目标");
    requireTrue(sameLayerNearest.reason == Reason::SameLayerNearestCenter,
                "同层择近必须记录 SameLayerNearestCenter 原因");

    const auto boundary = VisionHttpClient::selectTarget(QJsonArray{
        target(-500.0, 500.0, 200.0)
    });
    requireTrue(boundary.hasTarget(), "±500mm 边界必须有效");
    requireTrue(selected(boundary).insideStationRoi, "边界目标必须标记为 ROI 内");

    const auto orderA = VisionHttpClient::selectTarget(QJsonArray{
        target(200.0, 0.0, 350.0),
        target(20.0, 0.0, 200.0),
        target(100.0, 0.0, 210.0)
    });
    const auto orderB = VisionHttpClient::selectTarget(QJsonArray{
        target(100.0, 0.0, 210.0),
        target(200.0, 0.0, 350.0),
        target(20.0, 0.0, 200.0)
    });
    requireTrue(selected(orderA).x == 20.0 && selected(orderB).x == 20.0,
                "调换 JSON 顺序后必须选择同一物理目标");

    QJsonObject missingDepth = target(0.0, 0.0, 100.0);
    missingDepth.remove(QStringLiteral("depth_compensated"));
    const auto noValidTarget = VisionHttpClient::selectTarget(QJsonArray{
        target(600.0, 0.0, 100.0),
        missingDepth,
        QJsonObject{{QStringLiteral("offset_mm"), QStringLiteral("非法")}}
    });
    requireTrue(!noValidTarget.hasTarget(),
                "矩形外或关键字段非法时必须返回无目标");
    requireTrue(noValidTarget.reason == Reason::None,
                "无目标必须记录 None 原因");

    const auto stableTie = VisionHttpClient::selectTarget(QJsonArray{
        target(10.0, 0.0, 200.0),
        target(-10.0, 0.0, 200.0)
    });
    requireTrue(selected(stableTie).sourceIndex == 0,
                "深度和中心距离完全相同时必须按原始下标稳定兜底");
    requireTrue(stableTie.reason == Reason::StableSourceIndex,
                "稳定兜底必须记录 StableSourceIndex 原因");

    const QString summary = VisionHttpClient::formatTargetSelectionLog(currentStationWins);
    requireTrue(summary.contains(QStringLiteral("#0"))
                    && summary.contains(QStringLiteral("范围外"))
                    && summary.contains(QStringLiteral("选中=#1")),
                "候选摘要必须包含下标、ROI 判定和最终选择");
    requireTrue(!summary.contains(QLatin1Char('\n')),
                "每次推理候选摘要必须保持单行");

    return 0;
}

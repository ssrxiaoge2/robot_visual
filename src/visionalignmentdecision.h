#ifndef VISIONALIGNMENTDECISION_H
#define VISIONALIGNMENTDECISION_H

#include <QList>
#include <QString>
#include <QtGlobal>

namespace VisionAlignment {

/**
 * @brief 单次视觉观测结果。
 *
 * 平移量使用毫米，Rz 使用角度；targetValid 表示该结果仍对应被锁定的目标。
 */
struct Sample {
    double xMm = 0.0;
    double yMm = 0.0;
    double zMm = 0.0;
    double rzDeg = 0.0;
    bool targetValid = false;
};

/**
 * @brief 需要下发给机器人工具坐标系的精修量。
 *
 * 视觉坐标中的 X 与工具坐标同向，Y 和 Rz 与工具坐标反向。
 */
struct ToolCorrection {
    double xMm = 0.0;
    double yMm = 0.0;
    double rzDeg = 0.0;
};

/**
 * @brief 联合对准观察窗口的固定策略参数。
 *
 * 最大观察时间被限制为 8 秒以内；未来允许随硬件替换缩短窗口，不能突破该硬上限。
 */
struct WindowPolicy {
    double xyToleranceMm = 2.0;
    double rzToleranceDeg = 1.0;
    int maxFineCorrectionCount = 1;
    qint64 minElapsedMs = 4000;
    qint64 maxElapsedMs = 8000;
};

/// 观察窗口判定后，调度状态机应执行的下一项动作。
enum class WindowAction {
    ContinueObserving,
    Descend,
    FineCorrect,
    Stop
};

/**
 * @brief 提供窗口判定所需的当前采样及已执行状态。
 *
 * completedFineCorrectionCount 只记录已完成的精修次数，避免将尚未完成的命令重复计数。
 */
struct WindowInput {
    Sample latest;
    bool zStable = false;
    qint64 elapsedMs = 0;
    int completedFineCorrectionCount = 0;
};

/// 纯判定输出；Stop 时 reason 用于向上层说明停机原因。
struct WindowDecision {
    WindowAction action = WindowAction::ContinueObserving;
    ToolCorrection correction;
    QString reason;
};

/**
 * @brief 固定机械臂观察期间的稳健深度判定结果。
 *
 * filteredZMm 是排序后的中值，用作最终下探深度；coreRangeMm 是去掉一个最大值和
 * 一个最小值后，中间核心样本的极差。这样可以隔离深度算法偶发的单帧高、低异常，
 * 同时仍要求核心样本满足原有 5mm 稳定边界。
 */
struct StableDepthEvaluation {
    bool stable = false;
    double filteredZMm = 0.0;
    double coreRangeMm = 0.0;
};

/**
 * @brief 判断目标是否在平面 XY/Rz 联合容差内。
 * @param sample 当前锁定目标的视觉偏差。
 * @param policy 提供 XY 与 Rz 的允许误差。
 * @return 仅当目标有效且三个偏差均在含边界的容差内时返回 true。
 */
bool isPlanarAligned(const Sample &sample, const WindowPolicy &policy);

/**
 * @brief 将视觉偏差变换为工具坐标系的相对精修量。
 * @param sample 当前视觉偏差。
 * @return X 同向、Y 与 Rz 反向后的工具修正量。
 */
ToolCorrection toToolCorrection(const Sample &sample);

/**
 * @brief 使用奇数个真实新帧计算稳健深度。
 * @param samples 当前运动完成后按时间顺序保存的真实新帧深度。
 * @param requiredSamples 完整窗口帧数，必须是大于等于 3 的奇数。
 * @param maxCoreRangeMm 去掉一高一低后，核心样本允许的最大极差。
 * @return 窗口不完整或包含非法值时 stable=false；完整时同时返回中值和核心极差。
 */
StableDepthEvaluation evaluateStableDepth(const QList<double> &samples,
                                          int requiredSamples,
                                          double maxCoreRangeMm);

/**
 * @brief 按固定优先级判定联合对准观察窗口的下一步。
 *
 * 优先级依次为目标有效性、最小观察时间、对准并且 Z 稳定、8 秒超时、可用精修、
 * 精修耗尽，最后保留继续观察。
 */
WindowDecision decideWindow(const WindowInput &input,
                            const WindowPolicy &policy);

} // namespace VisionAlignment

#endif // VISIONALIGNMENTDECISION_H

#ifndef VISIONALIGNMENTDECISION_H
#define VISIONALIGNMENTDECISION_H

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
 * @brief 按固定优先级判定联合对准观察窗口的下一步。
 *
 * 优先级依次为目标有效性、最小观察时间、对准并且 Z 稳定、8 秒超时、可用精修、
 * 精修耗尽，最后保留继续观察。
 */
WindowDecision decideWindow(const WindowInput &input,
                            const WindowPolicy &policy);

} // namespace VisionAlignment

#endif // VISIONALIGNMENTDECISION_H

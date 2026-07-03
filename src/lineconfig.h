#ifndef LINECONFIG_H
#define LINECONFIG_H

#include <QDateTime>
#include <QChar>
#include <QString>

#include "palletscheduler.h"

/**
 * 整条补料线的生命周期状态，由 LineManager 维护：
 * Idle=未启动；Running=执行任务或等待缺料；ReturningHome=返回 LM1；Error=需人工复位。
 */
enum class LineSystemState { Idle, Running, ReturningHome, Error };

/**
 * 单任务状态：Pending=排队；Running=执行中；Succeeded=完整成功；
 * Failed=任务失败但整线不一定报警；Canceled=人工 Stop/系统清队列取消。
 */
enum class TaskState { Pending, Running, Succeeded, Failed, Canceled };

/// 缺料任务的来源；v1 仅使用 UiMock，CustomerSystem 为后续客户接口预留。
enum class TaskSource { UiMock, CustomerSystem };

/// 面向 UI 和日志的业务步骤，比 TaskExecutor 内部 ExecState 更稳定、更粗粒度。
enum class TaskStep {
    Waiting,             ///< 等待调度或正在装载配置。
    AgvToPickup,         ///< AGV 前往当前工位对应的取料 LM。
    ArmPickup,           ///< 机械臂执行视觉定位和取料。
    PreGripScan,         ///< 夹紧前扫码；机械臂在安全暂停点等待结果。
    GripAndLift,         ///< 扫码通过后夹紧料箱并抬升。
    StowAfterPickup,     ///< 取料完成后收回运行中安全姿态。
    AgvToUnload,         ///< AGV 前往该工位的倒料 LM。
    ArmUnload,           ///< 机械臂移动到倒料点并执行倒料函数。
    StowAfterUnload,     ///< 倒料后携带空箱收回安全姿态。
    AgvToPallet,         ///< AGV 前往共享/独立码垛区 LM。
    PreparePalletPoint,  ///< 校验码垛配置并计算下一相对偏移。
    ArmPalletPlace,      ///< 机械臂移动到码垛点并松爪。
    CommitPallet,        ///< 放置成功后更新 PalletScheduler 缓存。
    StowAfterPallet,     ///< 空箱放置后机械臂收回安全姿态。
    ReturningHome,       ///< 无后续任务时 AGV 返回 LM1。
    Done                 ///< 任务已进入终态，不再继续推进。
};

/// 可复制的任务快照；由 TaskQueue 创建、TaskExecutor 更新、MainWindow 只读展示。
struct Task {
    quint64 taskId = 0;                         ///< 进程内递增任务号，用于日志追踪。
    int stationId = 0;                         ///< 客户业务工位号，合法范围 1-12。
    TaskSource source = TaskSource::UiMock;     ///< 任务来源。
    TaskState state = TaskState::Pending;       ///< 当前任务状态。
    TaskStep step = TaskStep::Waiting;          ///< 当前业务步骤。
    int stepIndex = 0;                          ///< UI 排序/展示使用的稳定步骤序号。
    QDateTime createdAt;                        ///< 入队 UTC 时间。
    QString statusText;                         ///< 面向现场人员的当前状态说明。
    QString lastError;                          ///< 最近一次失败原因；成功任务为空。
};

/// 一个工位从取料到倒料所需的固定现场配置。
struct StationTaskConfig {
    int stationId = 0;                          ///< 业务工位号（1-12）。
    int pickupLm = 0;                           ///< 仙工地图取料站数字 LM。
    int unloadLm = 0;                           ///< 仙工地图倒料站数字 LM。
    PalletArea palletArea = PalletArea::LargeBox; ///< 空箱最终使用的码垛区。
    QString captureFunc;                        ///< 示教器拍照/抬升安全位函数名。
    QString unloadPointFunc;                    ///< 示教器倒料准备点函数名。
    QString unloadFunc;                         ///< 示教器翻转倒料函数名。
};

/// 一个码垛区的 AGV 站点和机械臂函数配置。
struct PalletAreaTaskConfig {
    PalletArea area = PalletArea::LargeBox; ///< PalletScheduler 使用的区域键。
    int palletLm = 0;                       ///< 仙工地图码垛区数字 LM。
    QString palletBaseFunc;                 ///< 机械臂到达码垛基准位的示教函数。
    QString releaseFunc;                    ///< 放置空箱后松爪的示教函数。
};

namespace lineconfig_detail {

// 集中配置表是现场点位/示教函数的唯一来源；修改前必须与 AGV 地图和示教器核对。
inline const StationTaskConfig kStationTaskConfigs[] = {
    {1, 3, 3, PalletArea::LargeBox, QStringLiteral("Func_capture1"), QStringLiteral("Func_daoliao1"), QStringLiteral("Func_fanzhuan")},
    {2, 4, 4, PalletArea::LargeBox, QStringLiteral("Func_capture2"), QStringLiteral("Func_daoliao2"), QStringLiteral("Func_fanzhuan")},
    {3, 5, 9, PalletArea::LargeBox, QStringLiteral("Func_capture3"), QStringLiteral("Func_daoliao3"), QStringLiteral("Func_fanzhuan")},
    {4, 5, 9, PalletArea::LargeBox, QStringLiteral("Func_capture4"), QStringLiteral("Func_daoliao4"), QStringLiteral("Func_fanzhuan")},
    {5, 5, 10, PalletArea::LargeBox, QStringLiteral("Func_capture5"), QStringLiteral("Func_daoliao5"), QStringLiteral("Func_fanzhuan")},
    {6, 6, 10, PalletArea::LargeBox, QStringLiteral("Func_capture6"), QStringLiteral("Func_daoliao6"), QStringLiteral("Func_fanzhuan")},
    {7, 6, 11, PalletArea::LargeBox, QStringLiteral("Func_capture7"), QStringLiteral("Func_daoliao7"), QStringLiteral("Func_fanzhuan")},
    {8, 6, 11, PalletArea::LargeBox, QStringLiteral("Func_capture8"), QStringLiteral("Func_daoliao8"), QStringLiteral("Func_fanzhuan")},
    {9, 7, 12, PalletArea::LargeBox, QStringLiteral("Func_capture9"), QStringLiteral("Func_daoliao9"), QStringLiteral("Func_fanzhuan")},
    {10, 7, 12, PalletArea::LargeBox, QStringLiteral("Func_capture10"), QStringLiteral("Func_daoliao10"), QStringLiteral("Func_fanzhuan")},
    {11, 7, 7, PalletArea::LargeBox, QStringLiteral("Func_capture11"), QStringLiteral("Func_daoliao11"), QStringLiteral("Func_fanzhuan")},
    {12, 15, 15, PalletArea::SmallBox, QStringLiteral("Func_capture12"), QStringLiteral("Func_daoliao12"), QStringLiteral("Func_fanzhuan")},
};

// 码垛 LM16/17 目前是设计阶段占位值，投产前必须替换为现场真实站点。
inline const PalletAreaTaskConfig kPalletAreaTaskConfigs[] = {
    {
        // 当前复用现有码垛枚举，业务含义分别是共享码垛区和工位12独立码垛区。
        PalletArea::LargeBox,
        16, // 这是现场待替换的模拟值。
        QStringLiteral("Func_pallet_shared_base"),
        QStringLiteral("Func_songzhua"),
    },
    {
        // 当前复用现有码垛枚举，业务含义分别是共享码垛区和工位12独立码垛区。
        PalletArea::SmallBox,
        17, // 这是现场待替换的模拟值。
        QStringLiteral("Func_pallet_s12_base"),
        QStringLiteral("Func_songzhua"),
    },
};

} // namespace lineconfig_detail

/// 按业务工位号查找只读配置；返回指针指向静态表，调用方不得释放或修改。
inline const StationTaskConfig *stationConfig(int stationId)
{
    for (const StationTaskConfig &config : lineconfig_detail::kStationTaskConfigs) {
        if (config.stationId == stationId) {
            return &config;
        }
    }
    return nullptr;
}

/// 按码垛区域查找只读配置；未配置时返回 nullptr，由调度升级为系统级错误。
inline const PalletAreaTaskConfig *palletAreaConfig(PalletArea area)
{
    for (const PalletAreaTaskConfig &config : lineconfig_detail::kPalletAreaTaskConfigs) {
        if (config.area == area) {
            return &config;
        }
    }
    return nullptr;
}

/// 将内部码垛枚举转换为客户可读名称，仅用于 UI 和日志。
inline QString palletAreaDisplayName(PalletArea area)
{
    switch (area) {
    case PalletArea::LargeBox:
        return QStringLiteral("共享码垛区");
    case PalletArea::SmallBox:
        return QStringLiteral("工位12独立码垛区");
    }
    return QStringLiteral("未知码垛区");
}

/// 将任务状态转换为客户可读文案。
inline QString taskStateText(TaskState state)
{
    switch (state) {
    case TaskState::Pending:
        return QStringLiteral("待处理");
    case TaskState::Running:
        return QStringLiteral("进行中");
    case TaskState::Succeeded:
        return QStringLiteral("已完成");
    case TaskState::Failed:
        return QStringLiteral("失败");
    case TaskState::Canceled:
        return QStringLiteral("已取消");
    }
    return QStringLiteral("未知状态");
}

/// 生成统一日志前缀，例如 [T#18 S05][AGV]，便于跨设备追踪同一任务。
inline QString taskSourceText(TaskSource source)
{
    switch (source) {
    case TaskSource::UiMock:
        return QStringLiteral("模拟");
    case TaskSource::CustomerSystem:
        return QStringLiteral("现场");
    }
    return QStringLiteral("未知来源");
}

inline QString formatTaskPrefix(const Task &task, const QString &stage)
{
    return QStringLiteral("[T#%1 S%2][%3]")
        .arg(task.taskId)
        .arg(task.stationId, 2, 10, QChar(u'0'))
        .arg(stage);
}

#endif // LINECONFIG_H

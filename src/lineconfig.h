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

/// 任务来源决定是否允许修改正式账本；模拟来源永远不能入账。
enum class TaskSource {
    UiMock,        ///< 现有 UI 模拟缺料。
    LiveAutomatic, ///< 正式账本自动计划的一箱任务。
    LiveManual     ///< 人工二次确认的一箱正式任务。
};

/// 统一 UI/FIFO/日志文案，避免各界面自行翻译产生不一致来源名称。
inline QString taskSourceText(TaskSource source)
{
    switch (source) {
    case TaskSource::UiMock:
        return QStringLiteral("模拟");
    case TaskSource::LiveAutomatic:
        return QStringLiteral("真实自动");
    case TaskSource::LiveManual:
        return QStringLiteral("人工补料");
    }
    return QStringLiteral("未知来源");
}

/// 真实缺料任务追加结果；拒收时 taskId 固定为 0，reason 可直接展示。
struct TaskEnqueueResult {
    bool accepted = false; ///< true 表示任务已经追加到 FIFO。
    quint64 taskId = 0;    ///< accepted=false 时必须为 0。
    QString reason;        ///< 拒收时可直接展示的中文原因。
};

/// 夹紧后的机械臂离开策略。
///
/// None：夹紧后不额外回拍照/安全位，阶段一直接完成，适用于 1/2/11/12 等
///       取料点与后续路径已经安全衔接的工位。
/// CaptureFunc：兼容旧逻辑，夹紧后复用 captureFunc 回安全高度；仅适用于
///              captureFunc 本身就是单点或安全回位路径的工位。
/// CustomFunc：夹紧后调用 afterGripFunc；用于 captureFunc 带过渡点、不能
///             作为夹后回位路径复用的工位。
enum class AfterGripMode {
    None,
    CaptureFunc,
    CustomFunc
};

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
    ArmPalletPlace,      ///< 等待机械臂完成标准码垛动作，动作内已包含松爪后回运行安全位。
    CommitPallet,        ///< 标准码垛动作完整成功后更新 PalletScheduler 缓存。
    StowAfterPallet,     ///< 兼容保留的显式收姿态步骤；标准码垛主流程不再进入。
    ReturningHome,       ///< 无后续任务时 AGV 返回 LM1。
    Done                 ///< 任务已进入终态，不再继续推进。
};

/// 可复制的任务快照；由 TaskQueue 创建、TaskExecutor 更新、MainWindow 只读展示。
struct Task {
    quint64 taskId = 0;                         ///< 进程内递增任务号，用于日志追踪。
    int stationId = 0;                         ///< 客户业务工位号，合法范围 1-12。
    TaskSource source = TaskSource::UiMock;     ///< 任务来源。
    quint64 replenishmentOrderNo = 0;           ///< 真实补料单号；模拟任务保持 0，便于默认兼容。
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
    AfterGripMode afterGripMode = AfterGripMode::CaptureFunc; ///< 夹紧后的离开策略，不能再隐式等同 captureFunc。
    QString afterGripFunc;                                    ///< afterGripMode=CustomFunc 时调用的夹后安全离开函数。
    QString unloadPointFunc;                    ///< 示教器倒料准备点函数名。
    QString unloadFunc;                         ///< 示教器翻转倒料函数名。
    QString stowAfterUnloadFunc;                ///< 这是“倒料后”从当前倒料点回运行安全位的函数，不是夹紧后回安全高度函数。
    double grabZClearance = 425.0;              ///< Z 下探余量(mm)：下探量=视觉Z-grabZClearance。
};

/// 一个码垛区的 AGV 站点和机械臂函数配置。
struct PalletAreaTaskConfig {
    PalletArea area = PalletArea::LargeBox; ///< PalletScheduler 使用的区域键。
    int palletLm = 0;                       ///< 仙工地图码垛区数字 LM。
    QString palletBaseFunc;                 ///< 机械臂到达码垛基准位的示教函数。
    QString releaseFunc;                    ///< 放置空箱后松爪的示教函数。
};

namespace lineconfig_detail {

// Z 下探余量按现场箱型显式写入每个工位：1-11 为篮筐，12 为紫框。
// 公式保持不变：descend = visionZ - grabZClearance。
// 1-11 现场现象是下降偏多，因此相对旧值 425.0 应调大；12 下降偏少，因此应调小。
static constexpr double kLargeBasketGrabZClearance = 417.0;
static constexpr double kPurpleBasketGrabZClearance = 380.0;

// 集中配置表是现场点位/示教函数的唯一来源；修改前必须与 AGV 地图和示教器核对。
inline const StationTaskConfig kStationTaskConfigs[] = {
    {1, 3, 3, PalletArea::SmallBox, QStringLiteral("Func_capture1"), AfterGripMode::None, QString(), QStringLiteral("Func_daoliao1"), QStringLiteral("Func_fanzhuan"), QStringLiteral("Func_yun_xing_zhong"), kLargeBasketGrabZClearance},
    {2, 4, 4, PalletArea::SmallBox, QStringLiteral("Func_capture2"), AfterGripMode::None, QString(), QStringLiteral("Func_daoliao2"), QStringLiteral("Func_fanzhuan"), QStringLiteral("Func_yun_xing_zhong"), kLargeBasketGrabZClearance},
    // 工位 3 的示教器函数内部包含“倒料点 -> 过渡点 -> 安全点”的路径。
    {3, 24, 9, PalletArea::SmallBox, QStringLiteral("Func_capture3"), AfterGripMode::CaptureFunc, QString(), QStringLiteral("Func_daoliao3"), QStringLiteral("Func_fanzhuan"), QStringLiteral("Func_daoliao3_huianquanwei"), kLargeBasketGrabZClearance},
    {4, 24, 9, PalletArea::SmallBox, QStringLiteral("Func_capture4"), AfterGripMode::CaptureFunc, QString(), QStringLiteral("Func_daoliao4"), QStringLiteral("Func_fanzhuan"), QStringLiteral("Func_yun_xing_zhong"), kLargeBasketGrabZClearance},
    {5, 23, 10, PalletArea::SmallBox, QStringLiteral("Func_capture5"), AfterGripMode::CaptureFunc, QString(), QStringLiteral("Func_daoliao5"), QStringLiteral("Func_fanzhuan"), QStringLiteral("Func_yun_xing_zhong"), kLargeBasketGrabZClearance},
    {6, 22, 10, PalletArea::SmallBox, QStringLiteral("Func_capture6"), AfterGripMode::CaptureFunc, QString(), QStringLiteral("Func_daoliao6"), QStringLiteral("Func_fanzhuan"), QStringLiteral("Func_yun_xing_zhong"), kLargeBasketGrabZClearance},
    {7, 21, 11, PalletArea::SmallBox, QStringLiteral("Func_capture7"), AfterGripMode::CaptureFunc, QString(), QStringLiteral("Func_daoliao7"), QStringLiteral("Func_fanzhuan"), QStringLiteral("Func_yun_xing_zhong"), kLargeBasketGrabZClearance},
    {8, 19, 11, PalletArea::SmallBox, QStringLiteral("Func_capture8"), AfterGripMode::CaptureFunc, QString(), QStringLiteral("Func_daoliao8"), QStringLiteral("Func_fanzhuan"), QStringLiteral("Func_yun_xing_zhong"), kLargeBasketGrabZClearance},
    {9, 20, 12, PalletArea::SmallBox, QStringLiteral("Func_capture9"), AfterGripMode::CaptureFunc, QString(), QStringLiteral("Func_daoliao9"), QStringLiteral("Func_fanzhuan"), QStringLiteral("Func_yun_xing_zhong"), kLargeBasketGrabZClearance},
    {10, 7, 12, PalletArea::SmallBox, QStringLiteral("Func_capture10"), AfterGripMode::CaptureFunc, QString(), QStringLiteral("Func_daoliao10"), QStringLiteral("Func_fanzhuan"), QStringLiteral("Func_yun_xing_zhong"), kLargeBasketGrabZClearance},
    {11, 7, 7, PalletArea::SmallBox, QStringLiteral("Func_capture11"), AfterGripMode::None, QString(), QStringLiteral("Func_daoliao11"), QStringLiteral("Func_fanzhuan"), QStringLiteral("Func_yun_xing_zhong"), kLargeBasketGrabZClearance},
    {12, 15, 15, PalletArea::LargeBox, QStringLiteral("Func_capture12"), AfterGripMode::CaptureFunc, QString(), QStringLiteral("Func_daoliao12"), QStringLiteral("Func_fanzhuan"), QStringLiteral("Func_yun_xing_zhong"), kPurpleBasketGrabZClearance},
};

// 码垛 LM16/17 目前是设计阶段占位值，投产前必须替换为现场真实站点。
inline const PalletAreaTaskConfig kPalletAreaTaskConfigs[] = {
    {
        // 当前复用现有码垛枚举，业务含义分别是共享码垛区和工位12独立码垛区。
        PalletArea::LargeBox,
        16, // 这是现场待替换的模拟值。
        QStringLiteral("Func_pallet_s12_base"),
        QStringLiteral("Func_songzhua"),
    },
    {
        // 当前复用现有码垛枚举，业务含义分别是共享码垛区和工位12独立码垛区。
        PalletArea::SmallBox,
        17, // 这是现场待替换的模拟值。
        QStringLiteral("Func_pallet_shared_base"),
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
inline QString formatTaskPrefix(const Task &task, const QString &stage)
{
    return QStringLiteral("[T#%1 S%2][%3]")
        .arg(task.taskId)
        .arg(task.stationId, 2, 10, QChar(u'0'))
        .arg(stage);
}

#endif // LINECONFIG_H

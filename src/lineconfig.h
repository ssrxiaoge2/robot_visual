#ifndef LINECONFIG_H
#define LINECONFIG_H

#include <QDateTime>
#include <QChar>
#include <QString>

#include "palletscheduler.h"

enum class LineSystemState { Idle, Running, ReturningHome, Error };
enum class TaskState { Pending, Running, Succeeded, Failed, Canceled };
enum class TaskSource { UiMock, CustomerSystem };
enum class TaskStep {
    Waiting,
    AgvToPickup,
    ArmPickup,
    PreGripScan,
    GripAndLift,
    StowAfterPickup,
    AgvToUnload,
    ArmUnload,
    StowAfterUnload,
    AgvToPallet,
    PreparePalletPoint,
    ArmPalletPlace,
    CommitPallet,
    StowAfterPallet,
    ReturningHome,
    Done
};

struct Task {
    quint64 taskId = 0;
    int stationId = 0;
    TaskSource source = TaskSource::UiMock;
    TaskState state = TaskState::Pending;
    TaskStep step = TaskStep::Waiting;
    int stepIndex = 0;
    QDateTime createdAt;
    QString statusText;
    QString lastError;
};

struct StationTaskConfig {
    int stationId = 0;
    int pickupLm = 0;
    int unloadLm = 0;
    PalletArea palletArea = PalletArea::LargeBox;
    QString captureFunc;
    QString unloadPointFunc;
    QString unloadFunc;
};

struct PalletAreaTaskConfig {
    PalletArea area = PalletArea::LargeBox;
    int palletLm = 0;
    QString palletBaseFunc;
    QString releaseFunc;
};

namespace lineconfig_detail {

inline const StationTaskConfig kStationTaskConfigs[] = {
    {1, 3, 3, PalletArea::LargeBox, QStringLiteral("Func_capture1"), QStringLiteral("Func_daoliao1"), QStringLiteral("Func_daoliao_S01")},
    {2, 4, 4, PalletArea::LargeBox, QStringLiteral("Func_capture2"), QStringLiteral("Func_daoliao2"), QStringLiteral("Func_daoliao_S02")},
    {3, 5, 9, PalletArea::LargeBox, QStringLiteral("Func_capture3"), QStringLiteral("Func_daoliao3"), QStringLiteral("Func_daoliao_S03")},
    {4, 5, 9, PalletArea::LargeBox, QStringLiteral("Func_capture4"), QStringLiteral("Func_daoliao4"), QStringLiteral("Func_daoliao_S04")},
    {5, 5, 10, PalletArea::LargeBox, QStringLiteral("Func_capture5"), QStringLiteral("Func_daoliao5"), QStringLiteral("Func_daoliao_S05")},
    {6, 6, 10, PalletArea::LargeBox, QStringLiteral("Func_capture6"), QStringLiteral("Func_daoliao6"), QStringLiteral("Func_daoliao_S06")},
    {7, 6, 11, PalletArea::LargeBox, QStringLiteral("Func_capture7"), QStringLiteral("Func_daoliao7"), QStringLiteral("Func_daoliao_S07")},
    {8, 6, 11, PalletArea::LargeBox, QStringLiteral("Func_capture8"), QStringLiteral("Func_daoliao8"), QStringLiteral("Func_daoliao_S08")},
    {9, 7, 12, PalletArea::LargeBox, QStringLiteral("Func_capture9"), QStringLiteral("Func_daoliao9"), QStringLiteral("Func_daoliao_S09")},
    {10, 7, 12, PalletArea::LargeBox, QStringLiteral("Func_capture10"), QStringLiteral("Func_daoliao10"), QStringLiteral("Func_daoliao_S10")},
    {11, 7, 7, PalletArea::LargeBox, QStringLiteral("Func_capture11"), QStringLiteral("Func_daoliao11"), QStringLiteral("Func_daoliao_S11")},
    {12, 15, 15, PalletArea::SmallBox, QStringLiteral("Func_capture12"), QStringLiteral("Func_daoliao12"), QStringLiteral("Func_daoliao_S12")},
};

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

inline const StationTaskConfig *stationConfig(int stationId)
{
    for (const StationTaskConfig &config : lineconfig_detail::kStationTaskConfigs) {
        if (config.stationId == stationId) {
            return &config;
        }
    }
    return nullptr;
}

inline const PalletAreaTaskConfig *palletAreaConfig(PalletArea area)
{
    for (const PalletAreaTaskConfig &config : lineconfig_detail::kPalletAreaTaskConfigs) {
        if (config.area == area) {
            return &config;
        }
    }
    return nullptr;
}

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

inline QString formatTaskPrefix(const Task &task, const QString &stage)
{
    return QStringLiteral("[T#%1 S%2][%3]")
        .arg(task.taskId)
        .arg(task.stationId, 2, 10, QChar(u'0'))
        .arg(stage);
}

#endif // LINECONFIG_H

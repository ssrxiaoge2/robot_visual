#ifndef PALLETSCHEDULER_H
#define PALLETSCHEDULER_H

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QStringList>

/**
 * @brief 码垛区域：现场固定两套配置，大箱区/小箱区互不影响。
 */
enum class PalletArea {
    LargeBox,
    SmallBox
};

/**
 * @brief 机械臂点位或偏移量。
 *
 * 在主流程里优先把该结构作为“相对机械臂初始点位的偏移”使用；
 * 在 UI 绝对预览里，pose = originPose + offset。
 */
struct PalletPose {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double rx = 0.0;
    double ry = 0.0;
    double rz = 0.0;
};

/**
 * @brief 码垛区域或空箱尺寸，单位 mm。
 */
struct PalletSize {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

/**
 * @brief 单个码垛区域的可配置参数。
 *
 * palletSize 表示固定码垛区域/托盘尺寸，boxSize 表示当前箱型尺寸；
 * palletSize.z 会计入相对 Z 偏移和总高度校验；gap/margin 控制每个箱之间和边缘预留空间；
 * originPose 只用于绝对预览，
 * 主流程给机械臂的值应优先使用 nextRelativeOffset() 返回的 offset。
 */
struct PalletConfig {
    PalletSize palletSize;
    PalletSize boxSize;
    double gapX = 20.0;
    double gapY = 20.0;
    double marginX = 20.0;
    double marginY = 20.0;
    int maxLayers = 8;
    // 现场记录/推荐字段，不自动叠加到相对 Z 偏移；相对 Z 已包含 palletSize.z。
    double releaseZOffset = 0.0;
    double maxRobotZ = 0.0; // 0 表示不校验最高安全 Z
    bool invertX = false;
    bool invertY = false;
    // 机械臂/现场示教的码垛初始点位，仅用于绝对点位预览。
    PalletPose originPose;
};

/**
 * @brief 仿真或下一点计算的单箱结果。
 *
 * index/layer/row/column 从 1 开始显示；offset 是主流程输出的相对偏移；
 * pose 是 UI 调试用的绝对预览点位；offset.z 已包含托盘高度。
 */
struct PalletSimulationItem {
    int index = 0;
    int layer = 0;
    int row = 0;
    int column = 0;
    PalletPose offset;
    PalletPose pose;
};

/**
 * @brief 空箱码垛规划与状态缓存模块。
 *
 * 该类只负责参数、容量、点位、仿真和已放数量缓存；不控制机械臂，
 * 不发 HTTP，也不判断箱型来源。真实流程必须在机械臂确认“到点 + 松爪完成”后
 * 才调用 commitPlaced() 推进缓存。
 */
class PalletScheduler : public QObject
{
    Q_OBJECT

public:
    /** @brief 构造时加载大箱/小箱两套配置和已放数量缓存。 */
    explicit PalletScheduler(QObject *parent = nullptr);

    /** @brief 返回区域中文名称，用于 UI 提示。 */
    static QString areaName(PalletArea area);
    /** @brief 大箱现场默认值：托盘 1100x1100，小箱/大箱参数中的大箱 580x380x288。 */
    static PalletConfig defaultLargeBoxConfig();
    /** @brief 小箱现场默认值：托盘 1100x1100，小箱 450x270x108。 */
    static PalletConfig defaultSmallBoxConfig();

    /** @brief 保存指定区域配置到内存和 QSettings，并发出 stateChanged。 */
    void setConfig(PalletArea area, const PalletConfig &config);
    /** @brief 读取指定区域当前配置。 */
    PalletConfig config(PalletArea area) const;

    /**
     * @brief 校验配置是否能码垛。
     * @param errors 输出阻断性错误，例如尺寸非法、容量为 0、超过安全 Z。
     * @param suggestions 输出非阻断建议，例如释放高度参考值偏离推荐范围。
     */
    bool validateConfig(PalletArea area,
                        QStringList *errors = nullptr,
                        QStringList *suggestions = nullptr) const;

    /** @brief 当前配置可放置的列数。 */
    int columns(PalletArea area) const;
    /** @brief 当前配置可放置的行数。 */
    int rows(PalletArea area) const;
    /** @brief 当前配置每层可放箱数。 */
    int perLayerCapacity(PalletArea area) const;
    /** @brief 当前配置总容量，等于每层容量 * 最大层数。 */
    int totalCapacity(PalletArea area) const;

    /** @brief 预览当前缓存下的下一箱绝对点位；不会修改 placedCount。 */
    bool nextPose(PalletArea area, PalletPose *pose, QString *error = nullptr) const;
    /** @brief 预览当前缓存下的下一箱相对偏移；这是主流程优先传给机械臂的值。 */
    bool nextRelativeOffset(PalletArea area, PalletPose *offset, QString *error = nullptr) const;

    /** @brief 放置完成后推进缓存；只能在机械臂松爪完成或 UI 仿真确认后调用。 */
    bool commitPlaced(PalletArea area, QString *error = nullptr);
    /** @brief 清空指定区域已放数量和视觉连续空计数。 */
    void reset(PalletArea area);
    /** @brief 人工修正已放数量，用于叉车搬走或现场状态不一致时纠偏。 */
    bool setPlacedCount(PalletArea area, int count, QString *error = nullptr);
    /** @brief 返回指定区域当前缓存的已放数量。 */
    int placedCount(PalletArea area) const;

    /** @brief 从空托盘开始生成仿真点位，不修改真实 placedCount。 */
    QList<PalletSimulationItem> simulate(PalletArea area,
                                          int maxCount = 0,
                                          QString *error = nullptr) const;

    /** @brief 标记机械臂码垛动作进行中；进行中时视觉空区结果不会自动清零缓存。 */
    void setStackingActive(PalletArea area, bool active);
    /** @brief 记录一次视觉识别为空；连续达到阈值且未码垛中时自动清零。 */
    void markAreaObservedEmpty(PalletArea area);
    /** @brief 记录一次视觉识别为有目标；清空连续空计数。 */
    void markAreaObservedOccupied(PalletArea area);
    /** @brief 返回连续视觉识别为空的次数。 */
    int emptyObserveCount(PalletArea area) const;
    /** @brief 返回最近一次视觉自动清零时间，无记录时为无效时间。 */
    QDateTime lastAutoResetTime(PalletArea area) const;

signals:
    /** @brief 指定区域已达到总容量，需要人工搬运。 */
    void areaFull(PalletArea area);
    /** @brief 视觉连续识别为空后自动清零缓存。 */
    void areaAutoReset(PalletArea area, const QString &reason);
    /** @brief 配置、状态或视觉计数变化，UI 需要刷新。 */
    void stateChanged(PalletArea area);

private:
    struct AreaState {
        PalletConfig config;
        int placedCount = 0;
        int emptyObserveCount = 0;
        bool stackingActive = false;
        QDateTime lastAutoResetTime;
    };

    AreaState &state(PalletArea area);
    const AreaState &state(PalletArea area) const;

    int boundedMaxLayers(PalletArea area) const;
    bool computeItem(PalletArea area,
                     int placedIndex,
                     PalletSimulationItem *item,
                     QString *error = nullptr) const;

    void load();
    void saveArea(PalletArea area) const;
    static QString settingsPrefix(PalletArea area);
    static void setError(QString *error, const QString &message);

    AreaState m_large;
    AreaState m_small;
};

Q_DECLARE_METATYPE(PalletArea)

#endif // PALLETSCHEDULER_H

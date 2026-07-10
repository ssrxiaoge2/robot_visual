#include "palletscheduler.h"

#include "palletplacesequence.h"

#include <QSettings>
#include <QtMath>

namespace {
static const char *kSettingsOrg = "wh-robot";
static const char *kSettingsApp = "robot-visual";
static constexpr int kMaxLayers = 8;

int floorCapacity(double usable, double box, double gap)
{
    if (usable <= 0.0 || box <= 0.0 || gap < 0.0)
        return 0;
    return qMax(0, static_cast<int>(std::floor((usable + gap) / (box + gap))));
}

}

PalletScheduler::PalletScheduler(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<PalletArea>("PalletArea");
    m_large.config = defaultLargeBoxConfig();
    m_small.config = defaultSmallBoxConfig();
    load();
}

QString PalletScheduler::areaName(PalletArea area)
{
    return area == PalletArea::LargeBox ? QStringLiteral("大箱码垛")
                                        : QStringLiteral("小箱码垛");
}

PalletConfig PalletScheduler::defaultLargeBoxConfig()
{
    PalletConfig cfg;
    cfg.palletSize = {1100.0, 1100.0, 0.0};
    cfg.boxSize = {580.0, 380.0, 288.0};
    cfg.gapX = 20.0;
    cfg.gapY = 20.0;
    cfg.marginX = 20.0;
    cfg.marginY = 20.0;
    cfg.maxLayers = 8;
    cfg.releaseZOffset = cfg.boxSize.z * 1.2;
    cfg.robotBaseHeightFromGround = 850.0;
    return cfg;
}

PalletConfig PalletScheduler::defaultSmallBoxConfig()
{
    PalletConfig cfg;
    cfg.palletSize = {1100.0, 1100.0, 0.0};
    cfg.boxSize = {450.0, 270.0, 108.0};
    cfg.gapX = 20.0;
    cfg.gapY = 20.0;
    cfg.marginX = 20.0;
    cfg.marginY = 20.0;
    cfg.maxLayers = 8;
    cfg.releaseZOffset = cfg.boxSize.z * 1.2;
    cfg.robotBaseHeightFromGround = 850.0;
    return cfg;
}

QString PalletScheduler::settingsFilePath()
{
    QSettings s(kSettingsOrg, kSettingsApp);
    return s.fileName();
}

double PalletScheduler::releaseGroundZ(const PalletPose &targetOffset,
                                       double releaseZOffsetMm)
{
    return targetOffset.z + releaseZOffsetMm;
}

double PalletScheduler::releaseTcpZ(const PalletPose &targetOffset,
                                    double releaseZOffsetMm,
                                    double robotBaseHeightFromGroundMm)
{
    return releaseGroundZ(targetOffset, releaseZOffsetMm)
        - robotBaseHeightFromGroundMm
        + PALLET_GRIPPER_RELEASE_Z_OFFSET_MM;
}

void PalletScheduler::setConfig(PalletArea area, const PalletConfig &config)
{
    state(area).config = config;
    saveArea(area);
    emit stateChanged(area);
}

PalletConfig PalletScheduler::config(PalletArea area) const
{
    return state(area).config;
}

bool PalletScheduler::validateConfig(PalletArea area,
                                     QStringList *errors,
                                     QStringList *suggestions) const
{
    QStringList localErrors;
    QStringList localSuggestions;
    const PalletConfig cfg = config(area);

    if (cfg.palletSize.x <= 0.0 || cfg.palletSize.y <= 0.0)
        localErrors << QStringLiteral("码垛区域长宽必须大于 0");
    if (cfg.palletSize.z < 0.0)
        localErrors << QStringLiteral("托盘面离地高度不能小于 0");
    if (cfg.robotBaseHeightFromGround <= 0.0)
        localErrors << QStringLiteral("机器人基座离地高度必须大于 0");
    if (cfg.boxSize.x <= 0.0 || cfg.boxSize.y <= 0.0 || cfg.boxSize.z <= 0.0)
        localErrors << QStringLiteral("空箱长宽高必须大于 0");
    if (cfg.gapX < 0.0 || cfg.gapY < 0.0)
        localErrors << QStringLiteral("箱间距不能小于 0");
    if (cfg.marginX < 0.0 || cfg.marginY < 0.0)
        localErrors << QStringLiteral("边缘预留不能小于 0");
    if (cfg.maxLayers < 1 || cfg.maxLayers > kMaxLayers)
        localErrors << QStringLiteral("最大层数必须在 1-8 之间");
    if (cfg.releaseZOffset < 0.0)
        localErrors << QStringLiteral("目标层上方释放高度不能小于 0");

    if (cfg.boxSize.z > 0.0) {
        const double minSuggested = cfg.boxSize.z;
        const double maxSuggested = cfg.boxSize.z * 1.5;
        if (cfg.releaseZOffset < minSuggested || cfg.releaseZOffset > maxSuggested) {
            localSuggestions << QStringLiteral(
                "目标层上方释放高度建议在 %1-%2 mm；真实释放地面高度 = 托盘面离地高度 + 层高 + 该高度")
                .arg(minSuggested, 0, 'f', 1)
                .arg(maxSuggested, 0, 'f', 1);
        }
    }

    const double usableX = cfg.palletSize.x - 2.0 * cfg.marginX;
    const double usableY = cfg.palletSize.y - 2.0 * cfg.marginY;
    if (usableX <= 0.0 || usableY <= 0.0)
        localErrors << QStringLiteral("边缘预留过大，可用码垛区域为空");

    const int c = columns(area);
    const int r = rows(area);
    if (localErrors.isEmpty() && (c <= 0 || r <= 0))
        localErrors << QStringLiteral("当前区域尺寸、边距和箱体尺寸无法放下任何空箱");

    if (placedCount(area) > totalCapacity(area))
        localErrors << QStringLiteral("当前已放数量超过总容量，请修正缓存状态");

    if (errors) *errors = localErrors;
    if (suggestions) *suggestions = localSuggestions;
    return localErrors.isEmpty();
}

int PalletScheduler::columns(PalletArea area) const
{
    const PalletConfig cfg = config(area);
    return floorCapacity(cfg.palletSize.x - 2.0 * cfg.marginX, cfg.boxSize.x, cfg.gapX);
}

int PalletScheduler::rows(PalletArea area) const
{
    const PalletConfig cfg = config(area);
    return floorCapacity(cfg.palletSize.y - 2.0 * cfg.marginY, cfg.boxSize.y, cfg.gapY);
}

int PalletScheduler::perLayerCapacity(PalletArea area) const
{
    return columns(area) * rows(area);
}

int PalletScheduler::totalCapacity(PalletArea area) const
{
    return perLayerCapacity(area) * boundedMaxLayers(area);
}

bool PalletScheduler::nextPose(PalletArea area, PalletPose *pose, QString *error) const
{
    PalletSimulationItem item;
    if (!computeItem(area, placedCount(area), &item, error))
        return false;
    if (pose) *pose = item.pose;
    return true;
}

bool PalletScheduler::nextRelativeOffset(PalletArea area, PalletPose *offset, QString *error) const
{
    PalletSimulationItem item;
    if (!computeItem(area, placedCount(area), &item, error))
        return false;
    if (offset) *offset = item.offset;
    return true;
}

bool PalletScheduler::commitPlaced(PalletArea area, QString *error)
{
    // 只在“放置动作已完成”时推进缓存；单纯计算下一点不能调用这里。
    const int cap = totalCapacity(area);
    if (cap <= 0) {
        setError(error, QStringLiteral("当前配置容量为 0，不能提交放置状态"));
        return false;
    }
    if (placedCount(area) >= cap) {
        setError(error, QStringLiteral("码垛区已满，请人工搬运并清零"));
        emit areaFull(area);
        return false;
    }

    AreaState &s = state(area);
    ++s.placedCount;
    saveArea(area);
    emit stateChanged(area);
    return true;
}

void PalletScheduler::reset(PalletArea area)
{
    AreaState &s = state(area);
    s.placedCount = 0;
    saveArea(area);
    emit stateChanged(area);
}

bool PalletScheduler::setPlacedCount(PalletArea area, int count, QString *error)
{
    const int cap = totalCapacity(area);
    if (count < 0 || count > cap) {
        setError(error, QStringLiteral("已放数量必须在 0-%1 之间").arg(cap));
        return false;
    }

    AreaState &s = state(area);
    s.placedCount = count;
    saveArea(area);
    emit stateChanged(area);
    return true;
}

int PalletScheduler::placedCount(PalletArea area) const
{
    return state(area).placedCount;
}

QList<PalletSimulationItem> PalletScheduler::simulate(PalletArea area,
                                                       int maxCount,
                                                       QString *error) const
{
    QStringList errors;
    QStringList suggestions;
    Q_UNUSED(suggestions)
    if (!validateConfig(area, &errors, nullptr)) {
        setError(error, errors.join(QStringLiteral("；")));
        return {};
    }

    const int cap = totalCapacity(area);
    const int count = maxCount <= 0 ? cap : qMin(maxCount, cap);
    QList<PalletSimulationItem> items;
    items.reserve(count);
    for (int i = 0; i < count; ++i) {
        PalletSimulationItem item;
        if (!computeItem(area, i, &item, error))
            break;
        items << item;
    }
    return items;
}

PalletScheduler::AreaState &PalletScheduler::state(PalletArea area)
{
    return area == PalletArea::LargeBox ? m_large : m_small;
}

const PalletScheduler::AreaState &PalletScheduler::state(PalletArea area) const
{
    return area == PalletArea::LargeBox ? m_large : m_small;
}

int PalletScheduler::boundedMaxLayers(PalletArea area) const
{
    return qBound(1, config(area).maxLayers, kMaxLayers);
}

bool PalletScheduler::computeItem(PalletArea area,
                                  int placedIndex,
                                  PalletSimulationItem *item,
                                  QString *error) const
{
    QStringList errors;
    if (!validateConfig(area, &errors, nullptr)) {
        setError(error, errors.join(QStringLiteral("；")));
        return false;
    }

    const int capPerLayer = perLayerCapacity(area);
    const int cap = totalCapacity(area);
    if (capPerLayer <= 0 || cap <= 0) {
        setError(error, QStringLiteral("当前配置容量为 0"));
        return false;
    }
    if (placedIndex < 0 || placedIndex >= cap) {
        setError(error, QStringLiteral("码垛区已满，请人工搬运并清零"));
        const_cast<PalletScheduler *>(this)->areaFull(area);
        return false;
    }

    const PalletConfig cfg = config(area);
    const int c = columns(area);
    const int r = rows(area);
    Q_UNUSED(r)
    const int indexInLayer = placedIndex % capPerLayer;
    const int layer = placedIndex / capPerLayer;
    const int column = indexInLayer % c;
    const int row = indexInLayer / c;

    const double totalUsedX = c * cfg.boxSize.x + (c - 1) * cfg.gapX;
    const double totalUsedY = rows(area) * cfg.boxSize.y + (rows(area) - 1) * cfg.gapY;
    const double startX = -totalUsedX / 2.0 + cfg.boxSize.x / 2.0;
    const double startY = -totalUsedY / 2.0 + cfg.boxSize.y / 2.0;

    PalletPose offset;
    offset.x = startX + column * (cfg.boxSize.x + cfg.gapX);
    offset.y = startY + row * (cfg.boxSize.y + cfg.gapY);
    offset.z = cfg.palletSize.z + layer * cfg.boxSize.z;
    if (cfg.invertX) offset.x = -offset.x;
    if (cfg.invertY) offset.y = -offset.y;

    // pose 仅用于 UI 表格预览：x/y/rz 展示相对偏移，z 展示目标 TCP Z 估算值。
    PalletPose pose;
    pose.x = offset.x;
    pose.y = offset.y;
    pose.z = releaseTcpZ(offset, cfg.releaseZOffset, cfg.robotBaseHeightFromGround);
    pose.rz = offset.rz;

    if (item) {
        item->index = placedIndex + 1;
        item->layer = layer + 1;
        item->row = row + 1;
        item->column = column + 1;
        item->offset = offset;
        item->pose = pose;
    }
    return true;
}

void PalletScheduler::load()
{
    QSettings s(kSettingsOrg, kSettingsApp);
    for (PalletArea area : {PalletArea::LargeBox, PalletArea::SmallBox}) {
        AreaState &st = state(area);
        const PalletConfig def = st.config;
        const QString prefix = settingsPrefix(area);
        PalletConfig cfg;
        cfg.palletSize.x = s.value(prefix + "/config/palletX", def.palletSize.x).toDouble();
        cfg.palletSize.y = s.value(prefix + "/config/palletY", def.palletSize.y).toDouble();
        cfg.palletSize.z = s.value(prefix + "/config/palletZ", def.palletSize.z).toDouble();
        cfg.boxSize.x = s.value(prefix + "/config/boxX", def.boxSize.x).toDouble();
        cfg.boxSize.y = s.value(prefix + "/config/boxY", def.boxSize.y).toDouble();
        cfg.boxSize.z = s.value(prefix + "/config/boxZ", def.boxSize.z).toDouble();
        cfg.gapX = s.value(prefix + "/config/gapX", def.gapX).toDouble();
        cfg.gapY = s.value(prefix + "/config/gapY", def.gapY).toDouble();
        cfg.marginX = s.value(prefix + "/config/marginX", def.marginX).toDouble();
        cfg.marginY = s.value(prefix + "/config/marginY", def.marginY).toDouble();
        cfg.maxLayers = s.value(prefix + "/config/maxLayers", def.maxLayers).toInt();
        cfg.releaseZOffset = s.value(prefix + "/config/releaseZOffset", def.releaseZOffset).toDouble();
        cfg.robotBaseHeightFromGround = s.value(prefix + "/config/robotBaseHeightFromGround",
                                                def.robotBaseHeightFromGround).toDouble();
        cfg.invertX = s.value(prefix + "/config/invertX", def.invertX).toBool();
        cfg.invertY = s.value(prefix + "/config/invertY", def.invertY).toBool();
        st.config = cfg;
        st.placedCount = s.value(prefix + "/placedCount", 0).toInt();
    }
}

void PalletScheduler::saveArea(PalletArea area) const
{
    QSettings s(kSettingsOrg, kSettingsApp);
    const AreaState &st = state(area);
    const PalletConfig cfg = st.config;
    const QString prefix = settingsPrefix(area);
    s.setValue(prefix + "/config/palletX", cfg.palletSize.x);
    s.setValue(prefix + "/config/palletY", cfg.palletSize.y);
    s.setValue(prefix + "/config/palletZ", cfg.palletSize.z);
    s.setValue(prefix + "/config/boxX", cfg.boxSize.x);
    s.setValue(prefix + "/config/boxY", cfg.boxSize.y);
    s.setValue(prefix + "/config/boxZ", cfg.boxSize.z);
    s.setValue(prefix + "/config/gapX", cfg.gapX);
    s.setValue(prefix + "/config/gapY", cfg.gapY);
    s.setValue(prefix + "/config/marginX", cfg.marginX);
    s.setValue(prefix + "/config/marginY", cfg.marginY);
    s.setValue(prefix + "/config/maxLayers", cfg.maxLayers);
    s.setValue(prefix + "/config/releaseZOffset", cfg.releaseZOffset);
    s.setValue(prefix + "/config/robotBaseHeightFromGround", cfg.robotBaseHeightFromGround);
    s.setValue(prefix + "/config/invertX", cfg.invertX);
    s.setValue(prefix + "/config/invertY", cfg.invertY);
    s.setValue(prefix + "/placedCount", st.placedCount);
    s.sync();
}

QString PalletScheduler::settingsPrefix(PalletArea area)
{
    return area == PalletArea::LargeBox ? QStringLiteral("pallet/large")
                                        : QStringLiteral("pallet/small");
}

void PalletScheduler::setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

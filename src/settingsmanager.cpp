#include "settingsmanager.h"

#include <QFile>
#include <QSaveFile>
#include <QSettings>
#include <QVariant>

#include <cmath>
#include <utility>

namespace {

void writeSettings(QSettings *ini, const RuntimeSettings &s)
{
    ini->setValue(QStringLiteral("pickup/largeBasketGrabZClearanceMm"),
                  s.pickup.largeBasketGrabZClearanceMm);
    ini->setValue(QStringLiteral("pickup/purpleBasketGrabZClearanceMm"),
                  s.pickup.purpleBasketGrabZClearanceMm);
    ini->setValue(QStringLiteral("pickup/grabXCompensationMm"),
                  s.pickup.grabXCompensationMm);
    ini->setValue(QStringLiteral("pickup/grabYCompensationMm"),
                  s.pickup.grabYCompensationMm);
    ini->setValue(QStringLiteral("pickup/zDescendInvert"), s.pickup.zDescendInvert);

    ini->setValue(QStringLiteral("vision/xyToleranceMm"), s.vision.xyToleranceMm);
    ini->setValue(QStringLiteral("vision/rzToleranceDeg"), s.vision.rzToleranceDeg);
    ini->setValue(QStringLiteral("vision/maxFineCorrectionCount"),
                  s.vision.maxFineCorrectionCount);
    // 保存时主动清理遗留键，避免旧 15 轮配置继续参与后续配置读取。
    ini->remove(QStringLiteral("vision/maxGrabIterations"));
    ini->setValue(QStringLiteral("vision/settleMs"), s.vision.settleMs);
    ini->setValue(QStringLiteral("vision/largeRzJumpThresholdDeg"),
                  s.vision.largeRzJumpThresholdDeg);
    ini->setValue(QStringLiteral("vision/largeRzDeltaToleranceDeg"),
                  s.vision.largeRzDeltaToleranceDeg);
    ini->setValue(QStringLiteral("vision/maxLargeRzExecutions"),
                  s.vision.maxLargeRzExecutions);
    ini->setValue(QStringLiteral("vision/stationRoiHalfXmm"),
                  s.vision.stationRoiHalfXmm);
    ini->setValue(QStringLiteral("vision/stationRoiHalfYmm"),
                  s.vision.stationRoiHalfYmm);
    ini->setValue(QStringLiteral("vision/anchorMaxTrustXmm"),
                  s.vision.anchorMaxTrustXmm);
    ini->setValue(QStringLiteral("vision/anchorMaxTrustYmm"),
                  s.vision.anchorMaxTrustYmm);
    ini->setValue(QStringLiteral("vision/anchorSameLayerToleranceMm"),
                  s.vision.anchorSameLayerToleranceMm);
    ini->setValue(QStringLiteral("vision/anchorSwitchMaxXyMm"),
                  s.vision.anchorSwitchMaxXyMm);
    ini->setValue(QStringLiteral("vision/lockMaxMissingFrames"),
                  s.vision.lockMaxMissingFrames);
    ini->setValue(QStringLiteral("vision/lockTrackRadiusMm"),
                  s.vision.lockTrackRadiusMm);
    ini->setValue(QStringLiteral("vision/lockSameLayerToleranceMm"),
                  s.vision.lockSameLayerToleranceMm);

    ini->setValue(QStringLiteral("depthDescent/enabled"), s.depthDescent.enabled);
    ini->setValue(QStringLiteral("depthDescent/triggerDepthMm"),
                  s.depthDescent.triggerDepthMm);
    ini->setValue(QStringLiteral("depthDescent/stepMm"), s.depthDescent.stepMm);
    ini->setValue(QStringLiteral("depthDescent/maxAccumulatedMm"),
                  s.depthDescent.maxAccumulatedMm);

    ini->setValue(QStringLiteral("search/descendStepMm"), s.search.descendStepMm);
    ini->setValue(QStringLiteral("search/maxAccumulatedMm"),
                  s.search.maxAccumulatedMm);
    ini->setValue(QStringLiteral("search/settleMs"), s.search.settleMs);

    ini->setValue(QStringLiteral("motion/speedPercent"), s.motion.speedPercent);
    ini->setValue(QStringLiteral("motion/velocity"), s.motion.velocity);
    ini->setValue(QStringLiteral("motion/acceleration"), s.motion.acceleration);
    ini->setValue(QStringLiteral("motion/radius"), s.motion.radius);
    ini->setValue(QStringLiteral("motion/scanRecoveryRotationDeg"),
                  s.motion.scanRecoveryRotationDeg);

    ini->setValue(QStringLiteral("safety/maxSingleXyAdjustMm"),
                  s.safety.maxSingleXyAdjustMm);
    ini->setValue(QStringLiteral("safety/maxZDescendMm"), s.safety.maxZDescendMm);
    ini->setValue(QStringLiteral("safety/normalMotionTimeoutMs"),
                  s.safety.normalMotionTimeoutMs);
    ini->setValue(QStringLiteral("safety/longZMotionTimeoutMs"),
                  s.safety.longZMotionTimeoutMs);
    ini->setValue(QStringLiteral("safety/commandReadyTimeoutMs"),
                  s.safety.commandReadyTimeoutMs);
    ini->setValue(QStringLiteral("safety/resetSettleMs"), s.safety.resetSettleMs);
    ini->setValue(QStringLiteral("safety/pollIntervalMs"), s.safety.pollIntervalMs);
    ini->setValue(QStringLiteral("safety/shortMotionFallbackMs"),
                  s.safety.shortMotionFallbackMs);
}

} // namespace

SettingsManager::SettingsManager(QString iniPath, QObject *parent)
    : QObject(parent)
    , m_iniPath(std::move(iniPath))
    , m_stagedPath(m_iniPath + QStringLiteral(".staged"))
{
}

SettingsLoadResult SettingsManager::load()
{
    RuntimeSettings loaded = RuntimeSettings::defaults();
    QStringList warnings;

    if (!QFile::exists(m_iniPath)) {
        m_current = loaded;
        return {loaded, warnings};
    }

    QSettings ini(m_iniPath, QSettings::IniFormat);
    auto readDouble = [&](const QString &key, double *target) {
        if (!ini.contains(key))
            return true;
        bool ok = false;
        const double value = ini.value(key).toDouble(&ok);
        if (!ok || !std::isfinite(value)) {
            warnings.append(QStringLiteral("%1 无效，已使用默认值").arg(key));
            return false;
        }
        *target = value;
        return true;
    };
    auto readInt = [&](const QString &key, int *target) {
        if (!ini.contains(key))
            return true;
        bool ok = false;
        const int value = ini.value(key).toInt(&ok);
        if (!ok) {
            warnings.append(QStringLiteral("%1 无效，已使用默认值").arg(key));
            return false;
        }
        *target = value;
        return true;
    };
    auto readBool = [&](const QString &key, bool *target) {
        if (ini.contains(key))
            *target = ini.value(key).toBool();
    };

    readDouble(QStringLiteral("pickup/largeBasketGrabZClearanceMm"),
               &loaded.pickup.largeBasketGrabZClearanceMm);
    readDouble(QStringLiteral("pickup/purpleBasketGrabZClearanceMm"),
               &loaded.pickup.purpleBasketGrabZClearanceMm);
    readDouble(QStringLiteral("pickup/grabXCompensationMm"),
               &loaded.pickup.grabXCompensationMm);
    readDouble(QStringLiteral("pickup/grabYCompensationMm"),
               &loaded.pickup.grabYCompensationMm);
    readBool(QStringLiteral("pickup/zDescendInvert"), &loaded.pickup.zDescendInvert);

    readDouble(QStringLiteral("vision/xyToleranceMm"), &loaded.vision.xyToleranceMm);
    readDouble(QStringLiteral("vision/rzToleranceDeg"), &loaded.vision.rzToleranceDeg);
    readInt(QStringLiteral("vision/maxFineCorrectionCount"),
            &loaded.vision.maxFineCorrectionCount);
    readInt(QStringLiteral("vision/settleMs"), &loaded.vision.settleMs);
    readDouble(QStringLiteral("vision/largeRzJumpThresholdDeg"),
               &loaded.vision.largeRzJumpThresholdDeg);
    readDouble(QStringLiteral("vision/largeRzDeltaToleranceDeg"),
               &loaded.vision.largeRzDeltaToleranceDeg);
    readInt(QStringLiteral("vision/maxLargeRzExecutions"),
            &loaded.vision.maxLargeRzExecutions);
    readDouble(QStringLiteral("vision/stationRoiHalfXmm"),
               &loaded.vision.stationRoiHalfXmm);
    readDouble(QStringLiteral("vision/stationRoiHalfYmm"),
               &loaded.vision.stationRoiHalfYmm);
    readDouble(QStringLiteral("vision/anchorMaxTrustXmm"),
               &loaded.vision.anchorMaxTrustXmm);
    readDouble(QStringLiteral("vision/anchorMaxTrustYmm"),
               &loaded.vision.anchorMaxTrustYmm);
    readDouble(QStringLiteral("vision/anchorSameLayerToleranceMm"),
               &loaded.vision.anchorSameLayerToleranceMm);
    readDouble(QStringLiteral("vision/anchorSwitchMaxXyMm"),
               &loaded.vision.anchorSwitchMaxXyMm);
    readInt(QStringLiteral("vision/lockMaxMissingFrames"),
            &loaded.vision.lockMaxMissingFrames);
    readDouble(QStringLiteral("vision/lockTrackRadiusMm"),
               &loaded.vision.lockTrackRadiusMm);
    readDouble(QStringLiteral("vision/lockSameLayerToleranceMm"),
               &loaded.vision.lockSameLayerToleranceMm);

    RuntimeSettings::DepthDescent depth = loaded.depthDescent;
    bool depthFieldsValid = true;
    readBool(QStringLiteral("depthDescent/enabled"), &depth.enabled);
    depthFieldsValid &= readDouble(QStringLiteral("depthDescent/triggerDepthMm"),
                                   &depth.triggerDepthMm);
    depthFieldsValid &= readDouble(QStringLiteral("depthDescent/stepMm"),
                                   &depth.stepMm);
    depthFieldsValid &= readDouble(QStringLiteral("depthDescent/maxAccumulatedMm"),
                                   &depth.maxAccumulatedMm);
    if (depthFieldsValid && depth.triggerDepthMm > 0.0 && depth.stepMm > 0.0
        && depth.maxAccumulatedMm > 0.0
        && depth.stepMm <= depth.maxAccumulatedMm) {
        loaded.depthDescent = depth;
    } else {
        loaded.depthDescent = RuntimeSettings::defaults().depthDescent;
        warnings.append(QStringLiteral("深度自动下探配置无效，已整组恢复默认值"));
    }

    readDouble(QStringLiteral("search/descendStepMm"), &loaded.search.descendStepMm);
    readDouble(QStringLiteral("search/maxAccumulatedMm"),
               &loaded.search.maxAccumulatedMm);
    readInt(QStringLiteral("search/settleMs"), &loaded.search.settleMs);
    readInt(QStringLiteral("motion/speedPercent"), &loaded.motion.speedPercent);
    readDouble(QStringLiteral("motion/velocity"), &loaded.motion.velocity);
    readDouble(QStringLiteral("motion/acceleration"), &loaded.motion.acceleration);
    readDouble(QStringLiteral("motion/radius"), &loaded.motion.radius);
    readDouble(QStringLiteral("motion/scanRecoveryRotationDeg"),
               &loaded.motion.scanRecoveryRotationDeg);
    readDouble(QStringLiteral("safety/maxSingleXyAdjustMm"),
               &loaded.safety.maxSingleXyAdjustMm);
    readDouble(QStringLiteral("safety/maxZDescendMm"),
               &loaded.safety.maxZDescendMm);
    readInt(QStringLiteral("safety/normalMotionTimeoutMs"),
            &loaded.safety.normalMotionTimeoutMs);
    readInt(QStringLiteral("safety/longZMotionTimeoutMs"),
            &loaded.safety.longZMotionTimeoutMs);
    readInt(QStringLiteral("safety/commandReadyTimeoutMs"),
            &loaded.safety.commandReadyTimeoutMs);
    readInt(QStringLiteral("safety/resetSettleMs"), &loaded.safety.resetSettleMs);
    readInt(QStringLiteral("safety/pollIntervalMs"), &loaded.safety.pollIntervalMs);
    readInt(QStringLiteral("safety/shortMotionFallbackMs"),
            &loaded.safety.shortMotionFallbackMs);

    const RuntimeSettings defaults = RuntimeSettings::defaults();
    auto warnFallback = [&](const QString &key) {
        warnings.append(QStringLiteral("%1 超出允许范围，已使用默认值").arg(key));
    };
    auto requirePositiveDouble = [&](const QString &key, double *value, double defaultValue) {
        if (std::isfinite(*value) && *value > 0.0)
            return;
        *value = defaultValue;
        warnFallback(key);
    };
    auto requireNonNegativeDouble =
        [&](const QString &key, double *value, double defaultValue) {
            if (std::isfinite(*value) && *value >= 0.0)
                return;
            *value = defaultValue;
            warnFallback(key);
        };
    auto requireFiniteDouble = [&](const QString &key, double *value, double defaultValue) {
        if (std::isfinite(*value))
            return;
        *value = defaultValue;
        warnFallback(key);
    };
    auto requirePositiveInt = [&](const QString &key, int *value, int defaultValue) {
        if (*value > 0)
            return;
        *value = defaultValue;
        warnFallback(key);
    };
    auto requireNonNegativeInt = [&](const QString &key, int *value, int defaultValue) {
        if (*value >= 0)
            return;
        *value = defaultValue;
        warnFallback(key);
    };

    requireFiniteDouble(QStringLiteral("pickup/largeBasketGrabZClearanceMm"),
                        &loaded.pickup.largeBasketGrabZClearanceMm,
                        defaults.pickup.largeBasketGrabZClearanceMm);
    requireFiniteDouble(QStringLiteral("pickup/purpleBasketGrabZClearanceMm"),
                        &loaded.pickup.purpleBasketGrabZClearanceMm,
                        defaults.pickup.purpleBasketGrabZClearanceMm);
    requireFiniteDouble(QStringLiteral("pickup/grabXCompensationMm"),
                        &loaded.pickup.grabXCompensationMm,
                        defaults.pickup.grabXCompensationMm);
    requireFiniteDouble(QStringLiteral("pickup/grabYCompensationMm"),
                        &loaded.pickup.grabYCompensationMm,
                        defaults.pickup.grabYCompensationMm);

    requirePositiveDouble(QStringLiteral("vision/xyToleranceMm"),
                          &loaded.vision.xyToleranceMm,
                          defaults.vision.xyToleranceMm);
    requirePositiveDouble(QStringLiteral("vision/rzToleranceDeg"),
                          &loaded.vision.rzToleranceDeg,
                          defaults.vision.rzToleranceDeg);
    if (loaded.vision.maxFineCorrectionCount < 0
        || loaded.vision.maxFineCorrectionCount > 2) {
        loaded.vision.maxFineCorrectionCount =
            defaults.vision.maxFineCorrectionCount;
        warnFallback(QStringLiteral("vision/maxFineCorrectionCount"));
    }
    requirePositiveInt(QStringLiteral("vision/settleMs"),
                       &loaded.vision.settleMs,
                       defaults.vision.settleMs);
    requirePositiveDouble(QStringLiteral("vision/largeRzJumpThresholdDeg"),
                          &loaded.vision.largeRzJumpThresholdDeg,
                          defaults.vision.largeRzJumpThresholdDeg);
    requireNonNegativeDouble(QStringLiteral("vision/largeRzDeltaToleranceDeg"),
                             &loaded.vision.largeRzDeltaToleranceDeg,
                             defaults.vision.largeRzDeltaToleranceDeg);
    requireNonNegativeInt(QStringLiteral("vision/maxLargeRzExecutions"),
                          &loaded.vision.maxLargeRzExecutions,
                          defaults.vision.maxLargeRzExecutions);
    requirePositiveDouble(QStringLiteral("vision/stationRoiHalfXmm"),
                          &loaded.vision.stationRoiHalfXmm,
                          defaults.vision.stationRoiHalfXmm);
    requirePositiveDouble(QStringLiteral("vision/stationRoiHalfYmm"),
                          &loaded.vision.stationRoiHalfYmm,
                          defaults.vision.stationRoiHalfYmm);
    requirePositiveDouble(QStringLiteral("vision/anchorMaxTrustXmm"),
                          &loaded.vision.anchorMaxTrustXmm,
                          defaults.vision.anchorMaxTrustXmm);
    requirePositiveDouble(QStringLiteral("vision/anchorMaxTrustYmm"),
                          &loaded.vision.anchorMaxTrustYmm,
                          defaults.vision.anchorMaxTrustYmm);
    requireNonNegativeDouble(QStringLiteral("vision/anchorSameLayerToleranceMm"),
                             &loaded.vision.anchorSameLayerToleranceMm,
                             defaults.vision.anchorSameLayerToleranceMm);
    requirePositiveDouble(QStringLiteral("vision/anchorSwitchMaxXyMm"),
                          &loaded.vision.anchorSwitchMaxXyMm,
                          defaults.vision.anchorSwitchMaxXyMm);
    requireNonNegativeInt(QStringLiteral("vision/lockMaxMissingFrames"),
                          &loaded.vision.lockMaxMissingFrames,
                          defaults.vision.lockMaxMissingFrames);
    requirePositiveDouble(QStringLiteral("vision/lockTrackRadiusMm"),
                          &loaded.vision.lockTrackRadiusMm,
                          defaults.vision.lockTrackRadiusMm);
    requireNonNegativeDouble(QStringLiteral("vision/lockSameLayerToleranceMm"),
                             &loaded.vision.lockSameLayerToleranceMm,
                             defaults.vision.lockSameLayerToleranceMm);

    if (!(loaded.search.descendStepMm > 0.0)
        || !(loaded.search.maxAccumulatedMm > 0.0)
        || loaded.search.descendStepMm > loaded.search.maxAccumulatedMm
        || loaded.search.settleMs <= 0) {
        loaded.search = defaults.search;
        warnings.append(QStringLiteral("search 配置相互约束无效，已整组使用默认值"));
    }

    if (loaded.motion.speedPercent < 1 || loaded.motion.speedPercent > 100) {
        loaded.motion.speedPercent = defaults.motion.speedPercent;
        warnFallback(QStringLiteral("motion/speedPercent"));
    }
    requirePositiveDouble(QStringLiteral("motion/velocity"),
                          &loaded.motion.velocity,
                          defaults.motion.velocity);
    requirePositiveDouble(QStringLiteral("motion/acceleration"),
                          &loaded.motion.acceleration,
                          defaults.motion.acceleration);
    requireNonNegativeDouble(QStringLiteral("motion/radius"),
                             &loaded.motion.radius,
                             defaults.motion.radius);
    requireFiniteDouble(QStringLiteral("motion/scanRecoveryRotationDeg"),
                        &loaded.motion.scanRecoveryRotationDeg,
                        defaults.motion.scanRecoveryRotationDeg);

    requirePositiveDouble(QStringLiteral("safety/maxSingleXyAdjustMm"),
                          &loaded.safety.maxSingleXyAdjustMm,
                          defaults.safety.maxSingleXyAdjustMm);
    requirePositiveDouble(QStringLiteral("safety/maxZDescendMm"),
                          &loaded.safety.maxZDescendMm,
                          defaults.safety.maxZDescendMm);
    if (loaded.safety.maxZDescendMm < loaded.depthDescent.maxAccumulatedMm
        || loaded.safety.maxZDescendMm < loaded.search.maxAccumulatedMm) {
        loaded.safety.maxZDescendMm = defaults.safety.maxZDescendMm;
        warnFallback(QStringLiteral("safety/maxZDescendMm"));
    }
    requirePositiveInt(QStringLiteral("safety/normalMotionTimeoutMs"),
                       &loaded.safety.normalMotionTimeoutMs,
                       defaults.safety.normalMotionTimeoutMs);
    requirePositiveInt(QStringLiteral("safety/longZMotionTimeoutMs"),
                       &loaded.safety.longZMotionTimeoutMs,
                       defaults.safety.longZMotionTimeoutMs);
    requirePositiveInt(QStringLiteral("safety/commandReadyTimeoutMs"),
                       &loaded.safety.commandReadyTimeoutMs,
                       defaults.safety.commandReadyTimeoutMs);
    requirePositiveInt(QStringLiteral("safety/resetSettleMs"),
                       &loaded.safety.resetSettleMs,
                       defaults.safety.resetSettleMs);
    requirePositiveInt(QStringLiteral("safety/pollIntervalMs"),
                       &loaded.safety.pollIntervalMs,
                       defaults.safety.pollIntervalMs);
    requirePositiveInt(QStringLiteral("safety/shortMotionFallbackMs"),
                       &loaded.safety.shortMotionFallbackMs,
                       defaults.safety.shortMotionFallbackMs);

    const SettingsValidation validation = validateRuntimeSettings(loaded);
    if (!validation.ok) {
        warnings.append(QStringLiteral("运行配置存在越界值，已恢复代码默认值：%1")
                            .arg(validation.errors.join(QStringLiteral("；"))));
        loaded = RuntimeSettings::defaults();
    }

    m_current = loaded;
    return {loaded, warnings};
}

bool SettingsManager::stageCandidate(const RuntimeSettings &candidate, QString *error)
{
    const SettingsValidation validation = validateRuntimeSettings(candidate);
    if (!validation.ok) {
        if (error)
            *error = validation.errors.join(QStringLiteral("；"));
        return false;
    }

    QFile::remove(m_stagedPath);
    QSettings staged(m_stagedPath, QSettings::IniFormat);
    staged.clear();
    writeSettings(&staged, candidate);
    staged.sync();
    if (staged.status() != QSettings::NoError) {
        if (error)
            *error = QStringLiteral("无法写入暂存配置文件：%1").arg(m_stagedPath);
        QFile::remove(m_stagedPath);
        return false;
    }
    return true;
}

bool SettingsManager::commitStaged(const RuntimeSettings &candidate, QString *error)
{
    if (!QFile::exists(m_stagedPath)) {
        if (error)
            *error = QStringLiteral("暂存配置文件不存在");
        return false;
    }

    QFile staged(m_stagedPath);
    if (!staged.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取暂存配置文件");
        return false;
    }
    const QByteArray bytes = staged.readAll();
    staged.close();

    QSaveFile destination(m_iniPath);
    if (!destination.open(QIODevice::WriteOnly)
        || destination.write(bytes) != bytes.size()
        || !destination.commit()) {
        if (error)
            *error = QStringLiteral("无法原子更新正式配置文件：%1").arg(m_iniPath);
        return false;
    }

    QFile::remove(m_stagedPath);
    m_current = candidate;
    return true;
}

void SettingsManager::discardStaged()
{
    QFile::remove(m_stagedPath);
}

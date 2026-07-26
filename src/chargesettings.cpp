#include "chargesettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QUuid>

#include <cmath>

namespace {

// INI 键统一集中定义，避免调用方或后续界面层散落魔法字符串。
constexpr auto kHost = "connection/host";
constexpr auto kPort = "connection/port";
constexpr auto kSlaveId = "connection/slaveId";
constexpr auto kVoltageV = "charge/voltageV";
constexpr auto kCurrentA = "charge/currentA";
constexpr auto kCutoffCurrentA = "charge/cutoffCurrentA";
constexpr auto kMaxChargeSeconds = "charge/maxChargeSeconds";
constexpr auto kResponseTimeoutMs = "timeout/responseTimeoutMs";
constexpr auto kConnectTimeoutMs = "timeout/connectTimeoutMs";
constexpr auto kPollIntervalMs = "timeout/pollIntervalMs";
constexpr auto kStartTimeoutMs = "timeout/startTimeoutMs";
constexpr auto kMonitorTimeoutMs = "timeout/monitorTimeoutMs";
constexpr auto kStopTimeoutMs = "timeout/stopTimeoutMs";
constexpr auto kMotionTimeoutMs = "timeout/motionTimeoutMs";
constexpr auto kSafeCurrentA = "safety/safeCurrentA";
constexpr auto kChargeDetectCurrentA = "safety/chargeDetectCurrentA";
constexpr auto kStartChargePercent = "threshold/startChargePercent";
constexpr auto kDispatchReadyPercent = "threshold/dispatchReadyPercent";
constexpr auto kStopChargePercent = "threshold/stopChargePercent";

bool isFinite(const double value)
{
    return std::isfinite(value);
}

void appendError(ChargeSettingsValidation &result, const bool condition, const QString &message)
{
    if (!condition)
        result.errors.append(message);
}

// 仅接受完整整数文本，避免 QSettings 将 "12ms" 等非法配置静默转换成 0。
bool readInt(const QSettings &ini, const char *key, int *value)
{
    if (!ini.contains(QLatin1String(key)))
        return false;
    bool ok = false;
    const int converted = ini.value(QLatin1String(key)).toString().toInt(&ok);
    if (ok)
        *value = converted;
    return ok;
}

bool readDouble(const QSettings &ini, const char *key, double *value)
{
    if (!ini.contains(QLatin1String(key)))
        return false;
    bool ok = false;
    const double converted = ini.value(QLatin1String(key)).toString().toDouble(&ok);
    if (ok && isFinite(converted))
        *value = converted;
    return ok && isFinite(converted);
}

void appendWarning(QStringList &warnings, const char *key)
{
    warnings.append(QStringLiteral("配置项“%1”非法，已恢复为默认值。")
                        .arg(QLatin1String(key)));
}

bool timeoutGroupIsValid(const ChargeSettings &settings)
{
    // 每个有限阶段至少要允许在截止点前发起下一轮查询；无限监控用0显式关闭
    // 上位机监控期限，因此不参与该项交叉约束。
    return settings.pollIntervalMs <= settings.startTimeoutMs
           && settings.pollIntervalMs <= settings.stopTimeoutMs
           && settings.pollIntervalMs <= settings.motionTimeoutMs
           && (settings.monitorTimeoutMs == 0
               || settings.pollIntervalMs <= settings.monitorTimeoutMs);
}

bool thresholdGroupIsValid(const ChargeSettings &settings)
{
    return settings.startChargePercent > 10
           && settings.startChargePercent < settings.dispatchReadyPercent
           && settings.dispatchReadyPercent < settings.stopChargePercent
           && settings.stopChargePercent <= 100;
}

} // namespace

ChargeSettings ChargeSettings::defaults()
{
    ChargeSettings settings;
    settings.host = QStringLiteral("192.168.115.108");
    return settings;
}

ChargeSettingsValidation validateChargeSettings(const ChargeSettings &settings)
{
    ChargeSettingsValidation result;
    appendError(result, !settings.host.trimmed().isEmpty(), QStringLiteral("充电设备地址不能为空。"));
    appendError(result, settings.port != 0, QStringLiteral("充电设备端口必须大于零。"));
    appendError(result, settings.slaveId != 0, QStringLiteral("充电站号必须大于零。"));
    appendError(result, isFinite(settings.voltageV) && settings.voltageV > 0.0 && settings.voltageV <= 100.0,
                QStringLiteral("充电电压必须为大于零且不超过 100 的有限数值。"));
    appendError(result, isFinite(settings.currentA) && settings.currentA > 0.0 && settings.currentA <= 120.0,
                QStringLiteral("充电电流必须为大于零且不超过 120 的有限数值。"));
    appendError(result, !settings.cutoffCurrentA.has_value()
                            || (isFinite(*settings.cutoffCurrentA)
                                && *settings.cutoffCurrentA > 0.0
                                && *settings.cutoffCurrentA <= 6553.5),
                QStringLiteral("截止电流启用时必须为正且乘十后不超过 65535 的有限数值。"));
    appendError(result, !settings.maxChargeSeconds.has_value()
                            || (*settings.maxChargeSeconds > 0
                                && *settings.maxChargeSeconds <= 65535),
                QStringLiteral("最大充电时长启用时必须在 1 至 65535 秒之间。"));

    appendError(result, settings.responseTimeoutMs > 0, QStringLiteral("响应超时必须大于零。"));
    appendError(result, settings.connectTimeoutMs > 0, QStringLiteral("连接超时必须大于零。"));
    appendError(result, settings.pollIntervalMs > 0, QStringLiteral("轮询间隔必须大于零。"));
    appendError(result, settings.startTimeoutMs > 0, QStringLiteral("启动超时必须大于零。"));
    appendError(result, settings.monitorTimeoutMs >= 0,
                QStringLiteral("监控超时必须大于等于零，零表示不按上位机时间停止。"));
    appendError(result, settings.stopTimeoutMs > 0, QStringLiteral("停止超时必须大于零。"));
    appendError(result, settings.motionTimeoutMs > 0, QStringLiteral("运动超时必须大于零。"));
    appendError(result, timeoutGroupIsValid(settings),
                QStringLiteral("轮询间隔不能大于启动、停止、推杆动作或已启用的监控超时。"));

    // 1.0A 是现场已验证 Python 流程用于“确认无输出后才允许缩回”的硬上限，
    // 不能把充电桩额定电流或操作员录入值当作可放宽的安全判断。
    appendError(result, isFinite(settings.safeCurrentA)
                            && settings.safeCurrentA >= 0.0
                            && settings.safeCurrentA <= 1.0,
                QStringLiteral("安全电流必须为 0 至 1.0A 的有限数值。"));
    appendError(result, isFinite(settings.chargeDetectCurrentA) && settings.chargeDetectCurrentA > 0.0,
                QStringLiteral("充电检测电流必须为正的有限数值。"));
    // 启动阈值必须严格高于 10%，三个阈值必须严格递增，防止状态切换抖动。
    appendError(result, settings.startChargePercent > 10 && settings.startChargePercent < 100,
                QStringLiteral("启动充电阈值必须在 11 到 99 之间。"));
    appendError(result, settings.dispatchReadyPercent > settings.startChargePercent
                            && settings.dispatchReadyPercent < 100,
                QStringLiteral("可派单阈值必须严格大于启动充电阈值且小于 100。"));
    appendError(result, settings.stopChargePercent > settings.dispatchReadyPercent
                            && settings.stopChargePercent <= 100,
                QStringLiteral("停止充电阈值必须严格大于可派单阈值且不超过 100。"));
    result.ok = result.errors.isEmpty();
    return result;
}

ChargeSettingsLoadResult loadChargeSettings(const QString &iniPath)
{
    ChargeSettingsLoadResult result;
    result.settings = ChargeSettings::defaults();
    if (iniPath.isEmpty() || !QFileInfo::exists(iniPath))
        return result;

    QSettings ini(iniPath, QSettings::IniFormat);
    if (ini.status() != QSettings::NoError) {
        result.warnings.append(QStringLiteral("无法读取充电参数文件，已使用默认值。"));
        return result;
    }

    const ChargeSettings defaults = ChargeSettings::defaults();
    auto readAndValidate = [&result, &ini](const char *key, auto &field, const auto &defaultValue, auto validator) {
        using ValueType = std::decay_t<decltype(field)>;
        if (!ini.contains(QLatin1String(key)))
            return;
        ValueType value{};
        const bool converted = [&]() {
            if constexpr (std::is_same_v<ValueType, double>)
                return readDouble(ini, key, &value);
            else
                return readInt(ini, key, &value);
        }();
        if (!converted || !validator(value)) {
            field = defaultValue;
            appendWarning(result.warnings, key);
            return;
        }
        field = value;
    };

    if (ini.contains(QLatin1String(kHost))) {
        const QString host = ini.value(QLatin1String(kHost)).toString().trimmed();
        if (host.isEmpty()) appendWarning(result.warnings, kHost); else result.settings.host = host;
    }
    int port = 0;
    if (ini.contains(QLatin1String(kPort)) && (!readInt(ini, kPort, &port) || port <= 0 || port > 65535))
        appendWarning(result.warnings, kPort);
    else if (ini.contains(QLatin1String(kPort))) result.settings.port = static_cast<quint16>(port);
    int slaveId = 0;
    if (ini.contains(QLatin1String(kSlaveId)) && (!readInt(ini, kSlaveId, &slaveId) || slaveId <= 0 || slaveId > 255))
        appendWarning(result.warnings, kSlaveId);
    else if (ini.contains(QLatin1String(kSlaveId))) result.settings.slaveId = static_cast<quint8>(slaveId);

    readAndValidate(kVoltageV, result.settings.voltageV, defaults.voltageV, [](double v) { return v > 0.0 && v <= 100.0; });
    readAndValidate(kCurrentA, result.settings.currentA, defaults.currentA, [](double v) { return v > 0.0 && v <= 120.0; });
    readAndValidate(kResponseTimeoutMs, result.settings.responseTimeoutMs, defaults.responseTimeoutMs, [](int v) { return v > 0; });
    readAndValidate(kConnectTimeoutMs, result.settings.connectTimeoutMs, defaults.connectTimeoutMs, [](int v) { return v > 0; });
    readAndValidate(kPollIntervalMs, result.settings.pollIntervalMs, defaults.pollIntervalMs, [](int v) { return v > 0; });
    readAndValidate(kStartTimeoutMs, result.settings.startTimeoutMs, defaults.startTimeoutMs, [](int v) { return v > 0; });
    readAndValidate(kMonitorTimeoutMs, result.settings.monitorTimeoutMs, defaults.monitorTimeoutMs, [](int v) { return v >= 0; });
    readAndValidate(kStopTimeoutMs, result.settings.stopTimeoutMs, defaults.stopTimeoutMs, [](int v) { return v > 0; });
    readAndValidate(kMotionTimeoutMs, result.settings.motionTimeoutMs, defaults.motionTimeoutMs, [](int v) { return v > 0; });
    readAndValidate(kSafeCurrentA, result.settings.safeCurrentA, defaults.safeCurrentA,
                    [](double v) { return v >= 0.0 && v <= 1.0; });
    readAndValidate(kChargeDetectCurrentA, result.settings.chargeDetectCurrentA, defaults.chargeDetectCurrentA, [](double v) { return v > 0.0; });
    readAndValidate(kStartChargePercent, result.settings.startChargePercent, defaults.startChargePercent, [](int v) { return v > 10 && v < 100; });
    readAndValidate(kDispatchReadyPercent, result.settings.dispatchReadyPercent, defaults.dispatchReadyPercent, [](int v) { return v >= 0 && v < 100; });
    readAndValidate(kStopChargePercent, result.settings.stopChargePercent, defaults.stopChargePercent, [](int v) { return v >= 0 && v <= 100; });

    if (ini.contains(QLatin1String(kCutoffCurrentA))) {
        double value = 0.0;
        // 充电桩寄存器以 0.1A 为单位，加载时即限制为可无损表示的16位值。
        // 非法可选项只保持默认未启用，不影响其他已独立验证的配置字段。
        if (readDouble(ini, kCutoffCurrentA, &value)
            && value > 0.0 && value * 10.0 <= 65535.0) {
            result.settings.cutoffCurrentA = value;
        } else {
            appendWarning(result.warnings, kCutoffCurrentA);
        }
    }
    if (ini.contains(QLatin1String(kMaxChargeSeconds))) {
        int value = 0;
        // 最大充电时长直接写单个16位秒寄存器，逐字段加载时拒绝回绕值。
        if (readInt(ini, kMaxChargeSeconds, &value)
            && value >= 1 && value <= 65535) {
            result.settings.maxChargeSeconds = value;
        } else {
            appendWarning(result.warnings, kMaxChargeSeconds);
        }
    }

    if (!timeoutGroupIsValid(result.settings)) {
        // 超时字段逐项合法仍可能形成 poll > deadline 的危险组合。整组恢复能避免
        // 混用部分现场值和部分默认值，同时保留网络、电气与电量阈值配置。
        result.settings.responseTimeoutMs = defaults.responseTimeoutMs;
        result.settings.connectTimeoutMs = defaults.connectTimeoutMs;
        result.settings.pollIntervalMs = defaults.pollIntervalMs;
        result.settings.startTimeoutMs = defaults.startTimeoutMs;
        result.settings.monitorTimeoutMs = defaults.monitorTimeoutMs;
        result.settings.stopTimeoutMs = defaults.stopTimeoutMs;
        result.settings.motionTimeoutMs = defaults.motionTimeoutMs;
        result.warnings.append(QStringLiteral(
            "超时与轮询组合不合法，已恢复该参数组的默认值。"));
    }
    if (!thresholdGroupIsValid(result.settings)) {
        // 仅恢复相互依赖的阈值组，不能因阈值录入错误丢失有效的网络、电气和超时配置。
        result.settings.startChargePercent = defaults.startChargePercent;
        result.settings.dispatchReadyPercent = defaults.dispatchReadyPercent;
        result.settings.stopChargePercent = defaults.stopChargePercent;
        result.warnings.append(QStringLiteral("充电阈值组合不合法，已恢复该阈值组的默认值。"));
    }
    Q_ASSERT(validateChargeSettings(result.settings).ok);
    return result;
}

bool saveChargeSettings(const QString &iniPath, const ChargeSettings &settings, QString *error)
{
    if (error)
        error->clear();
    const ChargeSettingsValidation validation = validateChargeSettings(settings);
    if (!validation.ok) {
        if (error) *error = validation.errors.join(QStringLiteral(" "));
        return false;
    }
    if (iniPath.isEmpty()) {
        if (error) *error = QStringLiteral("充电参数文件路径不能为空。");
        return false;
    }

    const QFileInfo targetInfo(iniPath);
    const QString directory = targetInfo.absolutePath();
    if (!QDir().mkpath(directory)) {
        if (error) *error = QStringLiteral("无法创建充电参数目录：%1").arg(directory);
        return false;
    }
    // QSettings 不能直接写入 QSaveFile，因此先将其序列化到同目录的临时文件，
    // 再把已验证的字节内容交给 QSaveFile 原子提交。临时文件与正式文件同目录，
    // 可确保 commit 使用同一文件系统上的原子替换，而不会先删除旧配置。
    // 使用 UUID 构成尚不存在的同目录文件名，避免 QTemporaryFile 在 Windows 上
    // 保留的文件句柄干扰 QSettings 创建 INI 文件。
    const QString temporaryPath = iniPath + QStringLiteral(".tmp.")
                                  + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto removeTemporaryFile = [&temporaryPath]() { QFile::remove(temporaryPath); };
    {
        QSettings ini(temporaryPath, QSettings::IniFormat);
        ini.setValue(QLatin1String(kHost), settings.host.trimmed());
        ini.setValue(QLatin1String(kPort), settings.port);
        ini.setValue(QLatin1String(kSlaveId), settings.slaveId);
        ini.setValue(QLatin1String(kVoltageV), settings.voltageV);
        ini.setValue(QLatin1String(kCurrentA), settings.currentA);
        if (settings.cutoffCurrentA) ini.setValue(QLatin1String(kCutoffCurrentA), *settings.cutoffCurrentA);
        if (settings.maxChargeSeconds) ini.setValue(QLatin1String(kMaxChargeSeconds), *settings.maxChargeSeconds);
        ini.setValue(QLatin1String(kResponseTimeoutMs), settings.responseTimeoutMs);
        ini.setValue(QLatin1String(kConnectTimeoutMs), settings.connectTimeoutMs);
        ini.setValue(QLatin1String(kPollIntervalMs), settings.pollIntervalMs);
        ini.setValue(QLatin1String(kStartTimeoutMs), settings.startTimeoutMs);
        ini.setValue(QLatin1String(kMonitorTimeoutMs), settings.monitorTimeoutMs);
        ini.setValue(QLatin1String(kStopTimeoutMs), settings.stopTimeoutMs);
        ini.setValue(QLatin1String(kMotionTimeoutMs), settings.motionTimeoutMs);
        ini.setValue(QLatin1String(kSafeCurrentA), settings.safeCurrentA);
        ini.setValue(QLatin1String(kChargeDetectCurrentA), settings.chargeDetectCurrentA);
        ini.setValue(QLatin1String(kStartChargePercent), settings.startChargePercent);
        ini.setValue(QLatin1String(kDispatchReadyPercent), settings.dispatchReadyPercent);
        ini.setValue(QLatin1String(kStopChargePercent), settings.stopChargePercent);
        ini.sync();
        if (ini.status() != QSettings::NoError) {
            removeTemporaryFile();
            if (error) *error = QStringLiteral("写入临时充电参数文件失败。");
            return false;
        }
    }

    QFile serializedFile(temporaryPath);
    if (!serializedFile.open(QIODevice::ReadOnly)) {
        removeTemporaryFile();
        if (error) *error = QStringLiteral("无法读取临时充电参数文件。");
        return false;
    }
    const QByteArray serializedContent = serializedFile.readAll();
    if (serializedFile.error() != QFile::NoError) {
        serializedFile.close();
        removeTemporaryFile();
        if (error) *error = QStringLiteral("读取临时充电参数文件失败。");
        return false;
    }
    serializedFile.close();
    removeTemporaryFile();

    QSaveFile atomicFile(iniPath);
    if (!atomicFile.open(QIODevice::WriteOnly)
        || atomicFile.write(serializedContent) != serializedContent.size()
        || !atomicFile.commit()) {
        if (error) *error = QStringLiteral("原子提交充电参数文件失败，原有配置保持不变。");
        return false;
    }
    return true;
}

#pragma once

#include <QString>
#include <QStringList>
#include <QMetaType>
#include <QtGlobal>
#include <optional>

// 自动充电流程使用的全部可配置参数。可选截止条件未配置时保持空值，
// 以便持久化层能够明确区分“未启用”与“启用但数值为零”。
struct ChargeSettings
{
    QString host;
    quint16 port = 8899;
    quint8 slaveId = 1;
    double voltageV = 58.4;
    double currentA = 50.0;
    std::optional<double> cutoffCurrentA;
    std::optional<int> maxChargeSeconds;
    int responseTimeoutMs = 5000;
    int connectTimeoutMs = 3000;
    int pollIntervalMs = 5000;
    int startTimeoutMs = 90000;
    int monitorTimeoutMs = 500000;
    int stopTimeoutMs = 20000;
    int motionTimeoutMs = 30000;
    double safeCurrentA = 1.0; ///< 现场 Python 已验证的无输出判定，上限固定为 1.0A。
    double chargeDetectCurrentA = 0.5;
    int startChargePercent = 15;
    int dispatchReadyPercent = 20;
    int stopChargePercent = 80;

    static ChargeSettings defaults();
};

struct ChargeSettingsValidation
{
    bool ok = false;
    QStringList errors;
};

struct ChargeSettingsLoadResult
{
    ChargeSettings settings;
    QStringList warnings;
};

ChargeSettingsValidation validateChargeSettings(const ChargeSettings &settings);
ChargeSettingsLoadResult loadChargeSettings(const QString &iniPath);
bool saveChargeSettings(const QString &iniPath, const ChargeSettings &settings, QString *error);

Q_DECLARE_METATYPE(ChargeSettings)

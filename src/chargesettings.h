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
    QString host;                         ///< 充电桩 RTU-over-TCP 地址，不包含协议或端口。
    quint16 port = 8899;                  ///< TCP 端口，现场 Python 验证值为 8899。
    quint8 slaveId = 1;                   ///< Modbus 从站号，写入每一帧的首字节。
    double voltageV = 58.4;               ///< 目标电压，单位 V；写寄存器前按 0.1V 转换。
    double currentA = 50.0;               ///< 目标电流，单位 A；写寄存器前按 0.1A 转换。
    std::optional<double> cutoffCurrentA; ///< 可选截止电流；空值表示不写对应寄存器。
    std::optional<int> maxChargeSeconds;  ///< 可选桩端最大时长；空值表示不写对应寄存器。
    int responseTimeoutMs = 5000;         ///< 单条 Modbus 请求等待完整响应的上限。
    int connectTimeoutMs = 3000;          ///< 每次 TCP 建连等待上限。
    int pollIntervalMs = 5000;            ///< 充电、停止和机械位置状态的轮询周期。
    int startTimeoutMs = 90000;           ///< Start 已确认后等待实际充电输出的总时限。
    int monitorTimeoutMs = 500000;        ///< 上位机充电监控上限；0 表示不按此条件停止。
    int stopTimeoutMs = 20000;            ///< Stop 已确认后等待继电器断开且电流安全的时限。
    int motionTimeoutMs = 30000;          ///< 缩枪命令确认后等待缩到位的时限。
    double safeCurrentA = 1.0; ///< 现场 Python 已验证的无输出判定，上限固定为 1.0A。
    double chargeDetectCurrentA = 0.5;    ///< 判断“已出现真实充电输出”的最小电流。
    int startChargePercent = 15;          ///< 低于该电量时锁存自动充电需求，必须大于 10%。
    int dispatchReadyPercent = 20;        ///< 有排队任务时达到该电量可提前安全停止并接单。
    int stopChargePercent = 80;           ///< 无排队任务时正常充电的目标停止电量。

    /// 返回经过现场确认的默认参数；自动充电授权不属于该持久化快照。
    static ChargeSettings defaults();
};

/** @brief 整份候选设置的集中校验结果；errors 可直接组合为现场中文提示。 */
struct ChargeSettingsValidation
{
    bool ok = false;       ///< 仅当所有独立字段和跨字段约束都满足时为 true。
    QStringList errors;    ///< 按检查顺序保存全部错误，不在首个错误处提前结束。
};

/** @brief 从磁盘加载后的可用设置与非致命降级说明。 */
struct ChargeSettingsLoadResult
{
    ChargeSettings settings; ///< 始终为可通过 validateChargeSettings() 的完整快照。
    QStringList warnings;    ///< 非法或缺失字段回退默认值时的逐项说明。
};

/// 无副作用校验整份设置，包括寄存器范围、超时组合和三级电量阈值顺序。
ChargeSettingsValidation validateChargeSettings(const ChargeSettings &settings);
/// 逐字段读取 INI；单字段非法时回退默认值，跨字段组合非法时只回退对应参数组。
ChargeSettingsLoadResult loadChargeSettings(const QString &iniPath);
/// 校验后使用同目录临时文件和 QSaveFile 原子替换，失败时保留原配置。
bool saveChargeSettings(const QString &iniPath, const ChargeSettings &settings, QString *error);

Q_DECLARE_METATYPE(ChargeSettings)

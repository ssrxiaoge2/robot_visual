#ifndef SHORTAGECALCULATOR_H
#define SHORTAGECALCULATOR_H

#include <QList>
#include <QMetaType>
#include <QString>

#include "shortageconfig.h"

/**
 * @brief 单轮现场缺料采样输入。
 *
 * 该结构只描述已经完成 HTTP/PLC 解析后的业务样本，不持有网络请求上下文。
 */
struct ShortageSample {
    qint64 actualQty = 0;                         ///< MES 当日累计产量；必须是非负整数。
    ProductModel product = ProductModel::Model88; ///< 当前 PLC 解析出的产品型号。
    ProductionMode mode = ProductionMode::L68;    ///< 当前 PLC 解析出的生产方式。
};

/**
 * @brief 面向 UI 的单工位库存测试快照。
 *
 * 测试面板语义如下：
 * - 初始化：estimatedAvailable = safetyStock + boxQuantity；
 * - 每轮扣减：按 12 工位分别执行 estimatedAvailable -= productionDelta * usage；
 * - shortage：configured=true 且 estimatedAvailable <= safetyStock；
 * - 未配置：configured=false，reason="配置缺失"，不得判定缺料。
 *
 * accumulated / awaitingAcceptance 仅为兼容旧调用点保留，测试面板应改读新字段。
 */
struct StationConsumption {
    int stationId = 0;
    qint64 accumulated = 0;        ///< 兼容旧路径：累计消耗量。
    qint64 estimatedAvailable = 0; ///< 当前工位预计可用物料，可为负数。
    int safetyStock = 0;
    int boxQuantity = 0;
    bool awaitingAcceptance = false; ///< 兼容旧路径：当前是否已暴露过缺料状态。
    bool configured = false;         ///< true 表示箱量/用量/安全库存均合法。
    bool shortage = false;           ///< true 表示预计可用量已触及/跌破安全线。
    QString reason;                  ///< 配置缺失、溢出保护等中文原因。
};

/**
 * @brief 一轮采样写入后的计算结果。
 *
 * ok=false 表示本轮样本被拒绝；pendingStations 为当前短缺工位列表，仅为兼容旧监控路径保留。
 */
struct IngestResult {
    bool ok = false;
    QList<int> pendingStations;
    QString errorMessage;
};

/**
 * @brief 纯计算预计可用物料计算器。
 *
 * 该类只负责测试面板的库存初始化、逐工位扣减、阈值判断和溢出保护。
 * 不包含 FIFO、派单或送料完成语义；confirmAccepted/markRejected 为兼容旧调用点保留。
 */
class ShortageCalculator
{
public:
    ShortageCalculator();
    explicit ShortageCalculator(const QList<MaterialConfig> &configs);

    void initializeForProduct(ProductModel product);
    IngestResult ingest(const ShortageSample &sample);
    bool confirmAccepted(int stationId);
    bool markRejected(int stationId);
    void markCommunicationInterrupted();
    QList<StationConsumption> snapshot() const;
    void reset();

private:
    struct StationRuntime {
        qint64 consumed = 0;             ///< 当前产品累计消耗，供旧界面兼容显示。
        qint64 estimatedAvailable = 0;   ///< 当前工位预计可用物料。
        bool awaitingAcceptance = false; ///< 兼容旧路径：是否已暴露过缺料事件。
        bool configured = false;         ///< false 表示配置缺失或初始化失败。
        QString reason;                  ///< 配置缺失、溢出保护等状态原因。
    };

    const MaterialConfig *configFor(int stationId, ProductModel product) const;
    QList<int> collectPendingStations(ProductModel product) const;
    void initializeStationRuntime(int stationId, ProductModel product);
    void clearRuntimeStates();

    QList<MaterialConfig> m_configs;    ///< 固化自 Excel 的 36 组配置快照。
    StationRuntime m_stationStates[12]; ///< 12 工位独立运行期状态。
    bool m_initialized = false;         ///< 是否已完成当前产品的库存初始化。
    bool m_hasActualBaseline = false;   ///< 是否已有当前产品的 actualQty 基线。
    bool m_rebaselineAfterCommunication = false; ///< 通信恢复后首轮只重建 actualQty 基线。
    ProductModel m_currentProduct = ProductModel::Model88;
    qint64 m_lastActualQty = 0;
};

Q_DECLARE_METATYPE(StationConsumption)
Q_DECLARE_METATYPE(QList<StationConsumption>)

#endif // SHORTAGECALCULATOR_H

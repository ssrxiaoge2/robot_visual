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
    qint64 actualQty = 0;                      ///< MES 当日累计产量；必须是非负整数。
    ProductModel product = ProductModel::Model88; ///< 当前 PLC 解析出的产品型号。
    ProductionMode mode = ProductionMode::L68;    ///< 当前 PLC 解析出的生产方式。
};

/**
 * @brief 面向 UI 的单工位累计快照。
 *
 * awaitingAcceptance=true 代表当前工位已有“待主调度确认是否入 FIFO”的缺料事件，
 * 这不等于送料完成，只代表阈值事件已暴露给上层。
 */
struct StationConsumption {
    int stationId = 0;
    qint64 accumulated = 0;
    int safetyStock = 0;
    int boxQuantity = 0;
    bool awaitingAcceptance = false;
};

/**
 * @brief 一轮采样写入后的计算结果。
 *
 * ok=false 表示本轮样本被拒绝，既不更新可见事件，也不应据此派单。
 */
struct IngestResult {
    bool ok = false;
    QList<int> pendingStations;
    QString errorMessage;
};

/**
 * @brief 纯计算缺料累计器。
 *
 * 该类只处理基线、累计、阈值和待确认状态，不负责网络请求，也不等待送料完成。
 * 上层一旦确认 FIFO 已接收缺料任务，只需调用 confirmAccepted() 扣减一次阈值。
 */
class ShortageCalculator
{
public:
    ShortageCalculator();
    explicit ShortageCalculator(const QList<MaterialConfig> &configs);

    IngestResult ingest(const ShortageSample &sample);
    bool confirmAccepted(int stationId);
    bool markRejected(int stationId);
    void markCommunicationInterrupted();
    QList<StationConsumption> snapshot() const;
    void reset();

private:
    struct StationRuntime {
        qint64 accumulated = 0;        ///< 当前产品/方式组合下的累计消耗。
        bool awaitingAcceptance = false; ///< 是否已有待上层确认的缺料事件。
    };

    const MaterialConfig *configFor(int stationId, ProductModel product) const;
    QList<int> collectPendingStations(ProductModel product, ProductionMode mode) const;
    void resetAccumulation(ProductModel product, ProductionMode mode, qint64 actualQty);
    void clearRuntimeStates();

    QList<MaterialConfig> m_configs;       ///< 缺料固定配置快照；默认来自 shortageconfig.h。
    StationRuntime m_stationStates[12];    ///< 12 个代码工位的运行期累计与待确认状态。
    bool m_hasBaseline = false;            ///< 是否已经建立过当前产品/方式的 actualQty 基线。
    bool m_rebaselineAfterCommunication = false; ///< 通信恢复后首轮只重建基线、不追算断线产量。
    ProductModel m_lastProduct = ProductModel::Model88;
    ProductionMode m_lastMode = ProductionMode::L68;
    qint64 m_lastActualQty = 0;
};

Q_DECLARE_METATYPE(StationConsumption)
Q_DECLARE_METATYPE(QList<StationConsumption>)

#endif // SHORTAGECALCULATOR_H

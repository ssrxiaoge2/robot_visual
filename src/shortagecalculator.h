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
 *
 * 当前新增的 recordReplenishment 也严格保持这个边界：
 * - 修改前：该类只能做“初始化 + 消耗扣减 + 阈值判断”，不知道真实倒料成功后如何回补库存。
 * - 修改后：允许在“外部已经确认真实倒料成功”这一前提下，只把指定工位的纯计算库存补回一箱。
 * - 仍然不会在这里创建任务、进入 FIFO、触发派单，也不会感知主调度状态机。
 *
 * 这样设计的原因是把“库存数学模型”和“主流程调度动作”彻底拆开：
 * - 主调度未来若需要接入真实缺料，只需在外部成功事件落地后调用本接口同步库存；
 * - e1ffb3f 对既有主流程的行为不会被这里直接改写，因为本类依旧没有任何派单副作用。
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
    /**
     * @brief 记录一次“已经确认成功”的真实倒料结果，只回补纯计算库存。
     *
     * 该接口专门服务于“真实缺料接入主流程”后的库存对账场景：
     * - 调用前提：外部执行链路已经确认某个真实补料任务完成，且确实对应当前产品；
     * - 本接口职责：仅把该工位 `estimatedAvailable` 增加一箱，并重新刷新 shortage snapshot 相关状态；
     * - 本接口明确不做：不派单、不重排队、不修改 FIFO、不写入任务系统。
     *
     * 修改前原有语义：
     * - 计算器只能随产量扣减库存，无法表达“倒料成功后补回一箱”。
     *
     * 修改后新增边界：
     * - 仅允许当前已初始化产品、且 `product == m_currentProduct` 的场景调用；
     * - 仅允许 1..12 工位；
     * - 工位配置必须存在且合法（箱量/安全库存/用量满足既有校验）；
     * - 若加一箱会导致 qint64 上溢，则返回 false，库存保持不变；
     * - 任一失败都表示“本次回补未生效”，不会偷偷改库存。
     *
     * 为什么不会影响 e1ffb3f 既有主流程：
     * - 这是新增公共接口，旧路径不主动调用就没有行为变化；
     * - 现有 ingest/confirmAccepted/markRejected 语义保持不变；
     * - 即便未来主调度接入，也只是把“真实倒料成功”映射为一次纯库存回补，不会在这里扩散成调度副作用。
     *
     * @param stationId 目标工位号，必须在 1..12。
     * @param product 本次真实倒料对应的产品型号，必须与当前初始化产品一致。
     * @return true 表示已经成功给该工位回补一箱；
     *         false 表示工位号非法、尚未初始化、产品不匹配、配置缺失/非法或发生溢出，此时库存完全不变。
     */
    bool recordReplenishment(int stationId, ProductModel product);
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

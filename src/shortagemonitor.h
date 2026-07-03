#ifndef SHORTAGEMONITOR_H
#define SHORTAGEMONITOR_H

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QTimer>

#include "customSysScheduler.h"
#include "lineconfig.h"
#include "shortagecalculator.h"

/**
 * @brief 真实缺料监控在单工位上的外部可见状态。
 *
 * 这些状态只服务调度监控 UI 和测试，不直接驱动 FIFO。
 * 修改前的 ShortageMonitor 只会输出“是否接受缺料”，不足以表达真实任务
 * 已排队、执行中、已倒料待收尾、通信暂停和换型门禁等现场状态，所以这里
 * 需要显式建模生产会话的业务阶段。
 */
enum class LiveShortageTaskState {
    Normal,                 ///< 当前无缺料、无在途真实任务，也没有通信/配置门禁。
    WaitingForQueue,        ///< 已判定缺料并已向主调度发起请求，等待主调度接单。
    Queued,                 ///< 主调度已给出 taskId，但任务尚未开始执行。
    Running,                ///< 真实任务已经开始，且尚未发生实际倒料。
    UnloadedFinishing,      ///< 已完成实际倒料并已入账一箱，但任务仍在做收姿态/码垛收尾。
    CommunicationPaused,    ///< 当前轮询通信异常，本轮暂停新增真实派单。
    ConfigurationError,     ///< 当前产品下该工位配置缺失或非法，禁止自动派单。
    WaitingOldProductTasks  ///< 已确认换型，但旧产品真实任务尚未全部结束，暂缓新产品派单。
};

/**
 * @brief 调度监控面板需要的单工位真实缺料快照。
 *
 * estimatedAvailable / safetyStock 继续来自纯计算器；
 * state 则由 ShortageMonitor 根据通信、换型门禁和任务记录推导。
 * 这样可以保证“库存模型”和“调度状态机”分层清楚，避免把 FIFO 语义混进计算器。
 */
struct LiveShortageStationSnapshot {
    int stationId = 0;                         ///< 业务工位号，固定范围 1..12。
    qint64 estimatedAvailable = 0;            ///< 当前预计可用库存；允许为负数。
    int safetyStock = 0;                      ///< 当前产品在该工位的安全阈值。
    LiveShortageTaskState state = LiveShortageTaskState::Normal; ///< 生产监控状态。
};

/**
 * @brief 生产态真实缺料会话。
 *
 * 修改前：旧实现服务“缺料信号测试面板”，核心语义是“缺料后等待 accept/reject”。
 * 修改后：该类变为真实生产监控会话，负责四请求聚合、两轮稳定确认、工位占用、
 * 倒料成功后的纯库存回补，以及向主调度发起真实缺料派单请求。
 *
 * 为什么这些修改不会直接影响 e1ffb3f 的既有主流程：
 * - 本类仍然不拥有也不改写 LineManager 的 FIFO 算法；
 * - 它只发出 `dispatchRequested(stationId)`，由上层决定是否入队以及给出 taskId；
 * - 实际库存回补通过 ShortageCalculator 的纯计算接口完成，不在这里执行硬件动作。
 */
class ShortageMonitor : public QObject
{
    Q_OBJECT
public:
    explicit ShortageMonitor(CustomSysScheduler *client,
                             QObject *parent = nullptr);

    bool isRunning() const;
    QList<LiveShortageStationSnapshot> snapshot() const;

public slots:
    void start();
    void stop();
    void confirmDispatch(int stationId, quint64 taskId, bool accepted);
    void onTaskStarted(const Task &task);
    void onMaterialUnloaded(const Task &task);
    void onTaskFinished(const Task &task);

signals:
    void dispatchRequested(int stationId);
    void statusChanged(QString text, bool healthy);
    void sampleUpdated(qint64 actualQty, ProductModel product, ProductionMode mode);
    void inventoryUpdated(QList<LiveShortageStationSnapshot> stations);
    void logMessage(QString message);

private slots:
    void onPollTimerTimeout();
    void onRoundTimeout();
    void onMesReplyReady(quint64 roundId, bool ok, qint64 actualQty, QString error);
    void onPlcReplyReady(quint64 roundId, int startAddress,
                         CustomSysScheduler::PlcBitReply result);

private:
    struct RoundState {
        quint64 roundId = 0;                     ///< 递增轮次号，防止旧响应混入新轮次。
        bool mesReceived = false;                ///< 当前轮次是否已收到 MES 结果。
        bool mesOk = false;                      ///< MES 结果是否成功可用。
        qint64 actualQty = 0;                    ///< MES 返回的日产量累计值。
        bool plc68Received = false;              ///< 生产方式分片是否已收到。
        bool plc71Received = false;              ///< 产品型号分片是否已收到。
        bool plc1998Received = false;            ///< L1998 分片是否已收到。
        CustomSysScheduler::PlcBitReply plc68;   ///< StartAddress=68 分片结果。
        CustomSysScheduler::PlcBitReply plc71;   ///< StartAddress=71 分片结果。
        CustomSysScheduler::PlcBitReply plc1998; ///< StartAddress=1998 分片结果。
    };

    struct LiveTaskRecord {
        quint64 taskId = 0;                      ///< 主调度分配的真实任务号，必须非 0。
        int stationId = 0;                       ///< 该任务负责的业务工位号。
        ProductModel product = ProductModel::Model88; ///< 任务创建时绑定的产品型号。
        TaskSource source = TaskSource::CustomerSystem; ///< 只允许现场真实任务进入本表。
        bool started = false;                    ///< true 表示任务已进入 TaskExecutor 执行。
        bool unloaded = false;                   ///< true 表示已发生实际倒料并完成库存回补。
    };

    void beginRound();
    void failRound(const QString &reason);
    void completeRoundIfReady();
    bool chooseProduct(const QHash<QString, bool> &bits,
                       ProductModel *product,
                       QString *error) const;
    bool chooseMode(const QHash<QString, bool> &bits,
                    ProductionMode *mode,
                    QString *error) const;
    bool advanceConfirmedSignals(ProductModel candidateProduct,
                                 ProductionMode candidateMode,
                                 ProductModel *effectiveProduct,
                                 ProductionMode *effectiveMode,
                                 bool *confirmedProductChanged,
                                 bool *confirmedModeChanged);
    void reevaluateDispatchForAllStations();
    void reevaluateStation(int stationId);
    void emitSnapshot();
    bool hasIncompleteCurrentRound() const;
    bool hasNotYetUnloadedTask(int stationId) const;
    bool hasTasksForOtherProduct(ProductModel product) const;
    LiveShortageTaskState stateForStation(const StationConsumption &station) const;

    CustomSysScheduler *m_client = nullptr; ///< 非拥有指针；网络层由 DeviceManager 统一持有。
    QTimer *m_pollTimer = nullptr;          ///< 5 秒轮询定时器；仅在真实模式运行中启用。
    QTimer *m_roundTimeout = nullptr;       ///< 3 秒单轮超时定时器。
    ShortageCalculator m_calculator;        ///< 纯库存模型；不派单、不控制 FIFO。
    RoundState m_round;                     ///< 当前尚未完成聚合的轮次。
    bool m_running = false;                 ///< true 允许新轮询与新真实派单。
    bool m_communicationPaused = false;     ///< true 表示最近一轮通信异常，暂停新增真实派单。

    bool m_hasConfirmedProduct = false;     ///< 是否已有两轮稳定确认的产品型号。
    bool m_hasConfirmedMode = false;        ///< 是否已有两轮稳定确认的生产方式。
    ProductModel m_confirmedProduct = ProductModel::Model88; ///< 当前稳定确认产品。
    ProductionMode m_confirmedMode = ProductionMode::L68;    ///< 当前稳定确认方式。
    ProductModel m_candidateProduct = ProductModel::Model88; ///< 连续确认中的候选产品。
    ProductionMode m_candidateMode = ProductionMode::L68;    ///< 连续确认中的候选方式。
    int m_candidateProductRounds = 0;       ///< 候选产品已连续命中的完整轮次数。
    int m_candidateModeRounds = 0;          ///< 候选方式已连续命中的完整轮次数。

    bool m_hasLastSample = false;           ///< 是否已有最近一次成功消费的样本。
    ShortageSample m_lastSample;            ///< 最近一次按“已确认产品/方式”写入计算器的样本。

    bool m_waitingForOldProductTasks = false; ///< 换型后是否仍在等待旧产品真实任务结束。
    QSet<int> m_waitingStations;              ///< 已判定缺料且已发起派单请求、等待主调度接单的工位。
    QHash<quint64, LiveTaskRecord> m_tasks;   ///< 真实任务事实表；允许“已倒料待收尾 + 下一箱排队”并存。
};

Q_DECLARE_METATYPE(LiveShortageTaskState)
Q_DECLARE_METATYPE(LiveShortageStationSnapshot)
Q_DECLARE_METATYPE(QList<LiveShortageStationSnapshot>)

#endif // SHORTAGEMONITOR_H

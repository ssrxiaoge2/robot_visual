#ifndef LIVE_SHORTAGE_COORDINATOR_H
#define LIVE_SHORTAGE_COORDINATOR_H

#include "lineconfig.h"
#include "shortageengine.h"
#include "shortagesamplecoordinator.h"

#include <QObject>
#include <functional>

/// 生产缺料任务队尾网关；实现只能追加 FIFO 和读取整线状态，禁止暴露重排/删除能力。
class IShortageTaskGateway
{
public:
    virtual ~IShortageTaskGateway() = default;
    /// 追加一箱正式缺料任务；拒收时 taskId 必须为 0，补料单号由 Engine 保持重试。
    virtual TaskEnqueueResult append(int stationId,
                                     TaskSource source,
                                     quint64 replenishmentOrderNo) = 0;
    /// 读取 LineManager 当前状态；选择 Live 不得通过该接口启动 LineManager。
    virtual LineSystemState lineState() const = 0;
};

/// 人工补料二次确认所需的只读现场摘要；不向普通 UI 暴露内部可变账本引用。
struct ManualBoxConfirmation {
    ProductModel product = ProductModel::Model88; ///< 当前稳定产品。
    ProductionMode mode = ProductionMode::LeftRight; ///< 当前稳定模式。
    int stationId = 0;             ///< 用户点击的代码工位，失败时为 0。
    QString sitePosition;          ///< 当前配置现场位置。
    QString partNumber;            ///< 当前配置品号。
    qint64 boxQuantity = 0;        ///< 确认后只创建这一箱。
    qint64 currentStock = 0;       ///< 点击时正式库存。
    qint64 minimumStock = 0;       ///< 当前最低位。
    qint64 maximumStock = 0;       ///< 当前最高位。
    bool highStockRisk = false;    ///< 库存达到/超过最高位时需要二次确认。
};

/// 正式缺料协调器：隔离 Mock/Live 来源，翻译 LineManager 事实，并驱动单一自动派单泵。
class LiveShortageCoordinator final : public QObject
{
    Q_OBJECT
public:
    /// engine/sampleCoordinator/taskGateway 均为非拥有依赖，由 DeviceManager 持有且生命周期更长。
    LiveShortageCoordinator(ShortageEngine *engine,
                            ShortageSampleCoordinator *sampleCoordinator,
                            IShortageTaskGateway *taskGateway,
                            QObject *parent = nullptr,
                            std::function<ShortageOperationResult()> liveStartGate = {});

    /// 析构时只停止非拥有采样对象，不删除外部依赖，避免 QObject 子对象析构顺序隐患。
    ~LiveShortageCoordinator() override;

    /// 返回工位当前配置和账本摘要；失败时 stationId=0，并通过 operationRejected 发布中文原因。
    ManualBoxConfirmation manualBoxConfirmation(int stationId) const;
    /// 测试/门禁只读接口：true 表示正式 Live 来源已选择。
    bool liveInputActive() const { return m_inputSource == ShortageInputSource::Live; }

public slots:
    void setInputSource(ShortageInputSource source);
    /// 启动恢复后只有 accepted=true 才解除“等待操作员确认”；false 保持停止。
    void confirmRecoveredState(bool accepted);
    void requestManualBox(int stationId, bool highStockRiskConfirmed);
    /// 保存前再次校验整线/采样/当前任务/FIFO 的入口；具体账本校验仍由 Engine 执行。
    void applyMaintenanceCorrection(ShortageMaintenanceCorrection correction);
    void onStableSample(ShortageSample sample);
    void onLineStateChanged(LineSystemState state, QString text);
    void onTaskAccepted(Task task);
    void onTaskStarted(Task task);
    void onMaterialUnloaded(Task task);
    void onTaskTerminal(Task task, QString reason);

signals:
    void snapshotChanged(ShortageUiSnapshot snapshot);
    void criticalAlarmRaised(QString reasonZh);
    void operationRejected(QString reasonZh);

private:
    void applyEngineResult(const ShortageEngineResult &result);
    void rejectOperation(const QString &reasonZh) const;
    void emitSnapshot(const ShortageEngineResult &result = {});
    void pumpDispatch();
    TaskFact taskFact(TaskFactKind kind, const Task &task, const QString &reasonZh) const;
    bool liveConfigurationValid(QString *reasonZh) const;
    bool hasOldTasksInFlight() const;
    static TaskSource sourceForOrigin(ReplenishmentOrigin origin);
    static ReplenishmentOrigin originForSource(TaskSource source);
    static QString productText(ProductModel product);
    static QString modeText(ProductionMode mode);

    ShortageEngine *m_engine = nullptr; ///< 非拥有正式账本/计划核心，只通过公开事务接口修改。
    ShortageSampleCoordinator *m_sampleCoordinator = nullptr; ///< 非拥有唯一正式采样协调器。
    IShortageTaskGateway *m_taskGateway = nullptr; ///< 非拥有 LineManager 队尾适配器。
    std::function<ShortageOperationResult()> m_liveStartGate; ///< 正式 Live 启动前只读硬门禁。
    ShortageInputSource m_inputSource = ShortageInputSource::Mock; ///< Mock/Live 互斥状态。
    LineSystemState m_lineState = LineSystemState::Idle; ///< 最近整线状态，Running 时才自动派单。
    ShortageCommunicationState m_communication = ShortageCommunicationState::Stopped; ///< 最近采样健康状态。
};

Q_DECLARE_METATYPE(ManualBoxConfirmation)

#endif // LIVE_SHORTAGE_COORDINATOR_H

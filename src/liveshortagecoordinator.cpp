#include "liveshortagecoordinator.h"

#include "shortageconfigstore.h"

#include <QDateTime>

namespace {

const ShortageStationRuntime *runtimeStation(const ShortageRuntimeState &state, int stationId)
{
    for (const ShortageStationRuntime &station : state.stations) {
        if (station.stationId == stationId)
            return &station;
    }
    return nullptr;
}

const ShortageStationConfig *stationConfig(const ShortageConfiguration &configuration,
                                           ProductModel product,
                                           int stationId)
{
    for (const ShortageStationConfig &station : configuration.stations) {
        if (station.product == product && station.stationId == stationId)
            return &station;
    }
    return nullptr;
}

bool hasChinese(const QString &text)
{
    for (const QChar ch : text) {
        if (ch.unicode() >= 0x4e00 && ch.unicode() <= 0x9fff)
            return true;
    }
    return false;
}

QString structuredError(QString actionZh, QString detailZh)
{
    if (!hasChinese(detailZh))
        detailZh.prepend(QStringLiteral("原因="));
    if (!detailZh.contains(QStringLiteral("处理动作")))
        detailZh.append(QStringLiteral("，处理动作=检查现场状态后重试或联系维护"));
    return QStringLiteral("%1失败：%2").arg(actionZh, detailZh);
}

} // namespace

LiveShortageCoordinator::LiveShortageCoordinator(ShortageEngine *engine,
                                                 ShortageSampleCoordinator *sampleCoordinator,
                                                 IShortageTaskGateway *taskGateway,
                                                 QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_sampleCoordinator(sampleCoordinator)
    , m_taskGateway(taskGateway)
{
    qRegisterMetaType<ShortageUiSnapshot>("ShortageUiSnapshot");
    qRegisterMetaType<ManualBoxConfirmation>("ManualBoxConfirmation");
    emitSnapshot();
}

LiveShortageCoordinator::~LiveShortageCoordinator()
{
    if (m_sampleCoordinator != nullptr)
        m_sampleCoordinator->stop();
}

ManualBoxConfirmation LiveShortageCoordinator::manualBoxConfirmation(int stationId) const
{
    ManualBoxConfirmation confirmation;
    if (m_engine == nullptr) {
        rejectOperation(structuredError(QStringLiteral("人工确认"), QStringLiteral("engine 缺失")));
        return confirmation;
    }

    const ShortageRuntimeState &state = m_engine->state();
    const ShortageStationRuntime *runtime = runtimeStation(state, stationId);
    const ShortageStationConfig *config =
        stationConfig(m_engine->configuration(), state.product, stationId);
    if (runtime == nullptr || config == nullptr) {
        rejectOperation(structuredError(
            QStringLiteral("人工确认"),
            QStringLiteral("stationId=%1 不存在，当前产品=%2")
                .arg(stationId)
                .arg(productText(state.product))));
        return confirmation;
    }

    confirmation.product = state.product;
    confirmation.mode = state.mode;
    confirmation.stationId = stationId;
    confirmation.sitePosition = config->sitePosition;
    confirmation.partNumber = config->partNumber;
    confirmation.boxQuantity = config->boxQuantity;
    confirmation.currentStock = runtime->stock;
    confirmation.minimumStock = config->minimumStock;
    confirmation.maximumStock = config->maximumStock;
    confirmation.highStockRisk = runtime->stock >= config->maximumStock;
    return confirmation;
}

void LiveShortageCoordinator::setInputSource(ShortageInputSource source)
{
    if (source == ShortageInputSource::Live) {
        QString reason;
        if (!liveConfigurationValid(&reason)) {
            rejectOperation(structuredError(QStringLiteral("切换 Live"), reason));
            return;
        }
        if (m_engine == nullptr || m_engine->restoreLocked() || !m_engine->state().initialized) {
            rejectOperation(structuredError(
                QStringLiteral("切换 Live"),
                QStringLiteral("启动恢复未安全安装，处理动作=保持 Mock 并进入维护确认")));
            return;
        }
    }

    if (m_inputSource == source) {
        pumpDispatch();
        return;
    }

    m_inputSource = source;
    if (m_sampleCoordinator != nullptr) {
        if (m_inputSource == ShortageInputSource::Live) {
            m_sampleCoordinator->setParameters(m_engine->configuration().parameters);
            m_sampleCoordinator->start();
            m_communication = ShortageCommunicationState::Sampling;
        } else {
            m_sampleCoordinator->stop();
            m_communication = ShortageCommunicationState::Stopped;
        }
    }
    emitSnapshot();
    pumpDispatch();
}

void LiveShortageCoordinator::confirmRecoveredState(bool accepted)
{
    if (m_engine == nullptr)
        return;
    applyEngineResult(
        m_engine->confirmRestoredState(accepted, QDateTime::currentDateTimeUtc()));
    pumpDispatch();
}

void LiveShortageCoordinator::requestManualBox(int stationId, bool highStockRiskConfirmed)
{
    if (m_engine == nullptr)
        return;
    applyEngineResult(m_engine->requestManualBox(stationId,
                                                 highStockRiskConfirmed,
                                                 QDateTime::currentDateTimeUtc()));
    pumpDispatch();
}

void LiveShortageCoordinator::applyMaintenanceCorrection(
    ShortageMaintenanceCorrection correction)
{
    if (m_engine == nullptr)
        return;
    applyEngineResult(m_engine->applyMaintenanceCorrection(correction,
                                                           QDateTime::currentDateTimeUtc()));
    pumpDispatch();
}

void LiveShortageCoordinator::onStableSample(ShortageSample sample)
{
    if (m_engine == nullptr || m_inputSource != ShortageInputSource::Live)
        return;
    const ShortageEngineResult result = m_engine->applyStableSample(sample);
    applyEngineResult(result);
    if (result.ok)
        pumpDispatch();
}

void LiveShortageCoordinator::onLineStateChanged(LineSystemState state, QString text)
{
    Q_UNUSED(text)
    m_lineState = state;
    if (m_engine != nullptr) {
        applyEngineResult(m_engine->activatePendingContextIfDrained(!hasOldTasksInFlight(),
                                                                    QDateTime::currentDateTimeUtc()));
    }
    pumpDispatch();
}

void LiveShortageCoordinator::onTaskAccepted(Task task)
{
    Q_UNUSED(task)
    // 派单接受由 pumpDispatch() 同步写入 Engine；LineManager 信号只作为 UI/日志事实保留。
}

void LiveShortageCoordinator::onTaskStarted(Task task)
{
    if (m_engine == nullptr || task.source == TaskSource::UiMock)
        return;
    applyEngineResult(m_engine->recordTaskStarted(
        taskFact(TaskFactKind::Started, task, QStringLiteral("LineManager 任务开始"))));
}

void LiveShortageCoordinator::onMaterialUnloaded(Task task)
{
    if (m_engine == nullptr || task.source == TaskSource::UiMock) {
        emitSnapshot();
        return;
    }
    applyEngineResult(m_engine->recordMaterialUnloaded(
        taskFact(TaskFactKind::MaterialUnloaded, task, QStringLiteral("LineManager 倒料完成"))));
    pumpDispatch();
}

void LiveShortageCoordinator::onTaskTerminal(Task task, QString reason)
{
    if (m_engine == nullptr || task.source == TaskSource::UiMock)
        return;
    TaskFactKind kind = TaskFactKind::Succeeded;
    if (task.state == TaskState::Failed)
        kind = TaskFactKind::Failed;
    else if (task.state == TaskState::Canceled)
        kind = TaskFactKind::Canceled;
    applyEngineResult(m_engine->recordTaskTerminal(taskFact(kind, task, reason)));
    pumpDispatch();
}

void LiveShortageCoordinator::applyEngineResult(const ShortageEngineResult &result)
{
    if (!result.ok && !result.messageZh.isEmpty())
        emit operationRejected(structuredError(QStringLiteral("协调器操作"), result.messageZh));
    if (result.criticalLock)
        emit criticalAlarmRaised(result.messageZh);
    emitSnapshot(result);
}

void LiveShortageCoordinator::rejectOperation(const QString &reasonZh) const
{
    emit const_cast<LiveShortageCoordinator *>(this)->operationRejected(reasonZh);
}

void LiveShortageCoordinator::emitSnapshot(const ShortageEngineResult &result)
{
    if (m_engine == nullptr)
        return;
    ShortageUiSnapshot snapshot;
    snapshot.inputSource = m_inputSource;
    snapshot.communication = m_communication;
    snapshot.runtime = m_engine->state();
    snapshot.hasLastProductionDelta = result.hasProductionDelta;
    snapshot.lastProductionDelta = result.productionDelta;
    snapshot.summaryLine1Zh = QStringLiteral("生产态：产品=%1，模式=%2，来源=%3，通信=%4")
                                  .arg(productText(snapshot.runtime.product),
                                       modeText(snapshot.runtime.mode),
                                       m_inputSource == ShortageInputSource::Live
                                           ? QStringLiteral("Live")
                                           : QStringLiteral("Mock"),
                                       QString::number(int(m_communication)));
    snapshot.summaryLine2Zh = QStringLiteral("活动工位=%1，等待数=%2，整线=%3，严重锁定=%4")
                                  .arg(snapshot.runtime.activeStationId)
                                  .arg(snapshot.runtime.waitingStationIds.size())
                                  .arg(int(m_lineState))
                                  .arg(snapshot.runtime.criticalLock
                                           ? snapshot.runtime.criticalReasonZh
                                           : QStringLiteral("否"));
    emit snapshotChanged(snapshot);
}

void LiveShortageCoordinator::pumpDispatch()
{
    if (m_engine == nullptr || m_taskGateway == nullptr)
        return;
    if (m_inputSource != ShortageInputSource::Live)
        return;
    const ShortageRuntimeState &state = m_engine->state();
    if (!state.initialized || !state.operatorConfirmedRestore)
        return;
    QString reason;
    if (!liveConfigurationValid(&reason))
        return;
    if (m_taskGateway->lineState() != LineSystemState::Running)
        return;

    const std::optional<ShortageDispatchRequest> request = m_engine->nextDispatchRequest();
    if (!request.has_value())
        return;

    const TaskSource source = sourceForOrigin(request->origin);
    const TaskEnqueueResult appended =
        m_taskGateway->append(request->stationId, source, request->replenishmentOrderNo);
    const ShortageEngineResult result =
        m_engine->recordDispatchResult(request->replenishmentOrderNo,
                                       appended.accepted,
                                       appended.taskId,
                                       appended.accepted
                                           ? QStringLiteral("LineManager 已接受")
                                           : appended.reason);
    applyEngineResult(result);
}

TaskFact LiveShortageCoordinator::taskFact(TaskFactKind kind,
                                           const Task &task,
                                           const QString &reasonZh) const
{
    TaskFact fact;
    fact.kind = kind;
    fact.replenishmentOrderNo = task.replenishmentOrderNo;
    fact.taskId = task.taskId;
    fact.stationId = task.stationId;
    fact.origin = originForSource(task.source);
    fact.occurredAtUtc = QDateTime::currentDateTimeUtc();
    fact.reasonZh = reasonZh;
    return fact;
}

bool LiveShortageCoordinator::liveConfigurationValid(QString *reasonZh) const
{
    if (m_engine == nullptr) {
        if (reasonZh != nullptr)
            *reasonZh = QStringLiteral("engine 缺失");
        return false;
    }
    const ShortageOperationResult validated =
        ShortageConfigStore::validate(m_engine->configuration());
    if (!validated.ok) {
        if (reasonZh != nullptr)
            *reasonZh = validated.messageZh;
        return false;
    }
    return true;
}

bool LiveShortageCoordinator::hasOldTasksInFlight() const
{
    if (m_engine == nullptr)
        return false;
    for (const ReplenishmentOrder &order : m_engine->state().orders) {
        switch (order.state) {
        case ReplenishmentOrderState::Queued:
        case ReplenishmentOrderState::Running:
        case ReplenishmentOrderState::Unloaded:
            return true;
        case ReplenishmentOrderState::AwaitingDispatch:
        case ReplenishmentOrderState::Succeeded:
        case ReplenishmentOrderState::FailedBeforeUnload:
        case ReplenishmentOrderState::FailedAfterUnload:
        case ReplenishmentOrderState::Canceled:
            break;
        }
    }
    return false;
}

TaskSource LiveShortageCoordinator::sourceForOrigin(ReplenishmentOrigin origin)
{
    return origin == ReplenishmentOrigin::Manual ? TaskSource::LiveManual
                                                 : TaskSource::LiveAutomatic;
}

ReplenishmentOrigin LiveShortageCoordinator::originForSource(TaskSource source)
{
    return source == TaskSource::LiveManual ? ReplenishmentOrigin::Manual
                                            : ReplenishmentOrigin::Automatic;
}

QString LiveShortageCoordinator::productText(ProductModel product)
{
    switch (product) {
    case ProductModel::Model88:
        return QStringLiteral("88");
    case ProductModel::Model88R:
        return QStringLiteral("88R");
    case ProductModel::Model92:
        return QStringLiteral("92");
    }
    return QStringLiteral("未知产品");
}

QString LiveShortageCoordinator::modeText(ProductionMode mode)
{
    switch (mode) {
    case ProductionMode::LeftRight:
        return QStringLiteral("L/R");
    case ProductionMode::LeftOnly:
        return QStringLiteral("L/L");
    case ProductionMode::RightOnly:
        return QStringLiteral("R/H");
    }
    return QStringLiteral("未知模式");
}

#include "shortagemonitor.h"

namespace {

constexpr int kPollIntervalMs = 5000;
constexpr int kRoundTimeoutMs = 3000;
constexpr int kStationCount = 12;

} // namespace

ShortageMonitor::ShortageMonitor(CustomSysScheduler *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
    , m_pollTimer(new QTimer(this))
    , m_roundTimeout(new QTimer(this))
{
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, &ShortageMonitor::onPollTimerTimeout);

    m_roundTimeout->setSingleShot(true);
    connect(m_roundTimeout, &QTimer::timeout, this, &ShortageMonitor::onRoundTimeout);

    if (m_client) {
        connect(m_client, &CustomSysScheduler::mesReplyReady,
                this, &ShortageMonitor::onMesReplyReady);
        connect(m_client, &CustomSysScheduler::plcReplyReady,
                this, &ShortageMonitor::onPlcReplyReady);
    }
}

bool ShortageMonitor::isRunning() const
{
    return m_running;
}

QList<LiveShortageStationSnapshot> ShortageMonitor::snapshot() const
{
    QList<LiveShortageStationSnapshot> stations;
    const QList<StationConsumption> consumption = m_calculator.snapshot();
    stations.reserve(consumption.size());

    for (const StationConsumption &item : consumption) {
        LiveShortageStationSnapshot station;
        station.stationId = item.stationId;
        station.estimatedAvailable = item.estimatedAvailable;
        station.safetyStock = item.safetyStock;
        station.state = stateForStation(item);
        stations.append(station);
    }

    return stations;
}

void ShortageMonitor::start()
{
    if (m_running || !m_client) {
        return;
    }

    m_running = true;
    emit statusChanged(QStringLiteral("现场系统待首轮数据"), true);
    emit logMessage(QStringLiteral("[ShortageMonitor] 开始真实缺料监听"));
    beginRound();
    m_pollTimer->start();
    emitSnapshot();
}

void ShortageMonitor::stop()
{
    if (!m_running) {
        return;
    }

    // stop() 的语义是“停止新增轮询和新增真实派单”，不是“把运行期库存清零重来”。
    // 因此这里仅停止定时器并丢弃未完成轮次，保留 m_calculator、m_tasks 和
    // 已确认产品/方式，让同一进程内再次 start() 时继续沿用现场运行期状态。
    m_running = false;
    m_pollTimer->stop();
    m_roundTimeout->stop();
    m_round = RoundState{};
    emit statusChanged(QStringLiteral("现场系统监听已停止"), false);
    emit logMessage(QStringLiteral("[ShortageMonitor] 停止真实缺料监听"));
    emitSnapshot();
}

void ShortageMonitor::confirmDispatch(int stationId, quint64 taskId, bool accepted)
{
    if (stationId < 1 || stationId > kStationCount) {
        return;
    }
    if (!m_waitingStations.contains(stationId)) {
        return;
    }

    if (!accepted) {
        emit logMessage(QStringLiteral("[ShortageMonitor] 工位%1 真实缺料请求暂未被主调度接收，保持待入队")
                            .arg(stationId));
        emitSnapshot();
        return;
    }
    if (taskId == 0 || !m_hasConfirmedProduct) {
        emit logMessage(QStringLiteral("[ShortageMonitor] 工位%1 接单确认无效：taskId/product 不完整")
                            .arg(stationId));
        emitSnapshot();
        return;
    }

    LiveTaskRecord record;
    record.taskId = taskId;
    record.stationId = stationId;
    record.product = m_confirmedProduct;
    record.source = TaskSource::CustomerSystem;
    m_tasks.insert(taskId, record);
    m_waitingStations.remove(stationId);
    emit logMessage(QStringLiteral("[ShortageMonitor] 工位%1 真实缺料已进入 FIFO，任务号=%2")
                        .arg(stationId)
                        .arg(taskId));
    emitSnapshot();
}

void ShortageMonitor::onTaskStarted(const Task &task)
{
    if (task.source != TaskSource::CustomerSystem) {
        return;
    }

    auto it = m_tasks.find(task.taskId);
    if (it == m_tasks.end()) {
        return;
    }

    it->started = true;
    emitSnapshot();
}

void ShortageMonitor::onMaterialUnloaded(const Task &task)
{
    // UiMock 只是调试入口，不代表现场真实倒料，绝不能修改真实库存。
    if (task.source != TaskSource::CustomerSystem) {
        return;
    }

    auto it = m_tasks.find(task.taskId);
    if (it == m_tasks.end() || it->unloaded) {
        return;
    }
    if (!m_calculator.recordReplenishment(it->stationId, it->product)) {
        emit logMessage(QStringLiteral("[ShortageMonitor] 工位%1 倒料成功后库存回补失败")
                            .arg(it->stationId));
        emitSnapshot();
        return;
    }

    it->unloaded = true;
    emit logMessage(QStringLiteral("[ShortageMonitor] 工位%1 已完成实际倒料，库存增加一箱")
                        .arg(it->stationId));
    emitSnapshot();

    // 只有当前真实模式仍在运行时，才允许根据“倒料后仍缺料”立即追加下一箱。
    // 若已切回模拟模式，则这里只完成库存对账；再次 start() 后再统一重新评估。
    if (m_running) {
        reevaluateStation(it->stationId);
    }
}

void ShortageMonitor::onTaskFinished(const Task &task)
{
    if (task.source != TaskSource::CustomerSystem) {
        return;
    }

    const auto it = m_tasks.find(task.taskId);
    if (it == m_tasks.end()) {
        return;
    }

    const int stationId = it->stationId;
    m_tasks.erase(it);

    if (m_waitingForOldProductTasks && m_hasConfirmedProduct
        && !hasTasksForOtherProduct(m_confirmedProduct)) {
        m_waitingForOldProductTasks = false;
        emit logMessage(QStringLiteral("[ShortageMonitor] 旧产品真实任务已结束，解除换型派单门禁"));
    }

    emitSnapshot();
    if (m_running) {
        reevaluateStation(stationId);
        if (!m_waitingForOldProductTasks) {
            reevaluateDispatchForAllStations();
        }
    }
}

void ShortageMonitor::onPollTimerTimeout()
{
    if (!m_running) {
        return;
    }
    if (hasIncompleteCurrentRound()) {
        emit logMessage(QStringLiteral("[ShortageMonitor] 上一轮尚未完成，跳过本次 5 秒 tick"));
        return;
    }
    beginRound();
}

void ShortageMonitor::onRoundTimeout()
{
    if (!m_running || !hasIncompleteCurrentRound()) {
        return;
    }
    failRound(QStringLiteral("现场系统 3 秒内未返回完整四请求"));
}

void ShortageMonitor::onMesReplyReady(quint64 roundId, bool ok, qint64 actualQty, QString error)
{
    if (!m_running || roundId != m_round.roundId || !hasIncompleteCurrentRound()) {
        return;
    }

    m_round.mesReceived = true;
    m_round.mesOk = ok;
    m_round.actualQty = actualQty;
    if (!ok) {
        failRound(error.isEmpty() ? QStringLiteral("MES 响应失败") : error);
        return;
    }
    completeRoundIfReady();
}

void ShortageMonitor::onPlcReplyReady(quint64 roundId,
                                      int startAddress,
                                      CustomSysScheduler::PlcBitReply result)
{
    if (!m_running || roundId != m_round.roundId || !hasIncompleteCurrentRound()) {
        return;
    }

    switch (startAddress) {
    case 68:
        m_round.plc68Received = true;
        m_round.plc68 = result;
        break;
    case 71:
        m_round.plc71Received = true;
        m_round.plc71 = result;
        break;
    case 1998:
        m_round.plc1998Received = true;
        m_round.plc1998 = result;
        break;
    default:
        failRound(QStringLiteral("收到未知 PLC 分片：StartAddress=%1").arg(startAddress));
        return;
    }

    if (!result.ok) {
        failRound(result.errorMessage.isEmpty()
                      ? QStringLiteral("PLC 分片 %1 失败").arg(startAddress)
                      : result.errorMessage);
        return;
    }
    completeRoundIfReady();
}

void ShortageMonitor::beginRound()
{
    if (!m_client) {
        return;
    }

    static quint64 nextRoundId = 1;
    m_round = RoundState{};
    m_round.roundId = nextRoundId++;
    m_roundTimeout->start(kRoundTimeoutMs);

    emit statusChanged(QStringLiteral("现场系统轮询中"), true);
    emit logMessage(QStringLiteral("[ShortageMonitor] 启动轮次 #%1").arg(m_round.roundId));

    m_client->fetchMesDayData(m_round.roundId);
    m_client->fetchPlcBits(m_round.roundId, 68, 2);
    m_client->fetchPlcBits(m_round.roundId, 71, 3);
    m_client->fetchPlcBits(m_round.roundId, 1998, 1);
}

void ShortageMonitor::failRound(const QString &reason)
{
    m_roundTimeout->stop();
    m_round = RoundState{};
    m_communicationPaused = true;
    m_calculator.markCommunicationInterrupted();
    emit statusChanged(reason, false);
    emit logMessage(QStringLiteral("[ShortageMonitor] %1").arg(reason));
    emitSnapshot();
}

void ShortageMonitor::completeRoundIfReady()
{
    if (!hasIncompleteCurrentRound()) {
        return;
    }
    if (!m_round.mesReceived || !m_round.plc68Received
        || !m_round.plc71Received || !m_round.plc1998Received) {
        return;
    }

    m_roundTimeout->stop();

    QHash<QString, bool> bits = m_round.plc68.bits;
    auto mergeBits = [&bits](const QHash<QString, bool> &fragment) {
        for (auto it = fragment.cbegin(); it != fragment.cend(); ++it) {
            bits.insert(it.key(), it.value());
        }
    };
    mergeBits(m_round.plc71.bits);
    mergeBits(m_round.plc1998.bits);

    ProductModel candidateProduct = ProductModel::Model88;
    QString productError;
    if (!chooseProduct(bits, &candidateProduct, &productError)) {
        failRound(productError);
        return;
    }

    ProductionMode candidateMode = ProductionMode::L68;
    QString modeError;
    if (!chooseMode(bits, &candidateMode, &modeError)) {
        failRound(modeError);
        return;
    }

    ProductModel effectiveProduct = candidateProduct;
    ProductionMode effectiveMode = candidateMode;
    bool confirmedProductChanged = false;
    bool confirmedModeChanged = false;
    if (!advanceConfirmedSignals(candidateProduct, candidateMode,
                                 &effectiveProduct, &effectiveMode,
                                 &confirmedProductChanged, &confirmedModeChanged)) {
        emit statusChanged(QStringLiteral("等待真实缺料信号连续两轮稳定确认"), true);
        m_round = RoundState{};
        m_communicationPaused = false;
        emitSnapshot();
        return;
    }

    if (confirmedProductChanged) {
        m_calculator.initializeForProduct(effectiveProduct);
        m_waitingForOldProductTasks = hasTasksForOtherProduct(effectiveProduct);
        emit logMessage(QStringLiteral("[ShortageMonitor] 产品已稳定切换为 %1")
                            .arg(productModelText(effectiveProduct)));
    } else if (confirmedModeChanged) {
        // 仅方式切换时保留库存，但下一轮只重建 MES 基线，不追补切换边界期间产量。
        m_calculator.markCommunicationInterrupted();
        emit logMessage(QStringLiteral("[ShortageMonitor] 生产方式已稳定切换为 %1")
                            .arg(productionModeText(effectiveMode)));
    }

    const ShortageSample sample{m_round.actualQty, effectiveProduct, effectiveMode};
    const IngestResult ingestResult = m_calculator.ingest(sample);
    if (!ingestResult.ok) {
        failRound(ingestResult.errorMessage);
        return;
    }

    m_communicationPaused = false;
    m_hasLastSample = true;
    m_lastSample = sample;
    emit sampleUpdated(sample.actualQty, sample.product, sample.mode);
    emit statusChanged(QStringLiteral("现场系统监听正常"), true);
    m_round = RoundState{};
    emitSnapshot();
    if (!m_waitingForOldProductTasks) {
        reevaluateDispatchForAllStations();
    }
}

bool ShortageMonitor::chooseProduct(const QHash<QString, bool> &bits,
                                    ProductModel *product,
                                    QString *error) const
{
    const bool l71 = bits.value(QStringLiteral("L71"), false);
    const bool l72 = bits.value(QStringLiteral("L72"), false);
    const bool l73 = bits.value(QStringLiteral("L73"), false);
    const int trueCount = (l71 ? 1 : 0) + (l72 ? 1 : 0) + (l73 ? 1 : 0);
    if (trueCount != 1) {
        if (error) {
            *error = QStringLiteral("PLC 产品位无效：L71/L72/L73 必须且只能有一个为 true");
        }
        return false;
    }

    if (product) {
        *product = l71 ? ProductModel::Model88
                       : (l72 ? ProductModel::Model88R : ProductModel::Model92);
    }
    return true;
}

bool ShortageMonitor::chooseMode(const QHash<QString, bool> &bits,
                                 ProductionMode *mode,
                                 QString *error) const
{
    const bool l68 = bits.value(QStringLiteral("L68"), false);
    const bool l69 = bits.value(QStringLiteral("L69"), false);
    const bool l1998 = bits.value(QStringLiteral("L1998"), false);
    const int trueCount = (l68 ? 1 : 0) + (l69 ? 1 : 0) + (l1998 ? 1 : 0);
    if (trueCount != 1) {
        if (error) {
            *error = QStringLiteral("PLC 生产方式位无效：L68/L69/L1998 必须且只能有一个为 true");
        }
        return false;
    }

    if (mode) {
        *mode = l68 ? ProductionMode::L68
                    : (l69 ? ProductionMode::L69 : ProductionMode::L1998);
    }
    return true;
}

bool ShortageMonitor::advanceConfirmedSignals(ProductModel candidateProduct,
                                              ProductionMode candidateMode,
                                              ProductModel *effectiveProduct,
                                              ProductionMode *effectiveMode,
                                              bool *confirmedProductChanged,
                                              bool *confirmedModeChanged)
{
    if (candidateProduct == m_candidateProduct) {
        ++m_candidateProductRounds;
    } else {
        m_candidateProduct = candidateProduct;
        m_candidateProductRounds = 1;
    }

    if (candidateMode == m_candidateMode) {
        ++m_candidateModeRounds;
    } else {
        m_candidateMode = candidateMode;
        m_candidateModeRounds = 1;
    }

    if (confirmedProductChanged) {
        *confirmedProductChanged = false;
    }
    if (confirmedModeChanged) {
        *confirmedModeChanged = false;
    }

    if (m_candidateProductRounds >= 2) {
        if (!m_hasConfirmedProduct || m_confirmedProduct != m_candidateProduct) {
            if (confirmedProductChanged) {
                *confirmedProductChanged = m_hasConfirmedProduct;
            }
            m_confirmedProduct = m_candidateProduct;
        }
        m_hasConfirmedProduct = true;
    }

    if (m_candidateModeRounds >= 2) {
        if (!m_hasConfirmedMode || m_confirmedMode != m_candidateMode) {
            if (confirmedModeChanged) {
                *confirmedModeChanged = m_hasConfirmedMode;
            }
            m_confirmedMode = m_candidateMode;
        }
        m_hasConfirmedMode = true;
    }

    if (effectiveProduct) {
        *effectiveProduct = m_confirmedProduct;
    }
    if (effectiveMode) {
        *effectiveMode = m_confirmedMode;
    }
    return m_hasConfirmedProduct && m_hasConfirmedMode;
}

void ShortageMonitor::reevaluateDispatchForAllStations()
{
    const QList<StationConsumption> stations = m_calculator.snapshot();
    for (const StationConsumption &station : stations) {
        reevaluateStation(station.stationId);
    }
}

void ShortageMonitor::reevaluateStation(int stationId)
{
    const QList<StationConsumption> stations = m_calculator.snapshot();
    if (stationId < 1 || stationId > stations.size()) {
        return;
    }

    const StationConsumption &station = stations.at(stationId - 1);
    if (!m_running || !station.shortage || !station.configured || m_waitingForOldProductTasks) {
        return;
    }
    if (m_communicationPaused || m_waitingStations.contains(stationId) || hasNotYetUnloadedTask(stationId)) {
        return;
    }

    m_waitingStations.insert(stationId);
    emit dispatchRequested(stationId);
    emit logMessage(QStringLiteral("[ShortageMonitor] 工位%1 触发真实缺料派单请求").arg(stationId));
    emitSnapshot();
}

void ShortageMonitor::emitSnapshot()
{
    emit inventoryUpdated(snapshot());
}

bool ShortageMonitor::hasIncompleteCurrentRound() const
{
    return m_round.roundId != 0;
}

bool ShortageMonitor::hasNotYetUnloadedTask(int stationId) const
{
    for (auto it = m_tasks.cbegin(); it != m_tasks.cend(); ++it) {
        if (it->stationId == stationId && !it->unloaded) {
            return true;
        }
    }
    return false;
}

bool ShortageMonitor::hasTasksForOtherProduct(ProductModel product) const
{
    for (auto it = m_tasks.cbegin(); it != m_tasks.cend(); ++it) {
        if (it->product != product) {
            return true;
        }
    }
    return false;
}

LiveShortageTaskState ShortageMonitor::stateForStation(const StationConsumption &station) const
{
    if (!station.configured) {
        return LiveShortageTaskState::ConfigurationError;
    }
    if (m_waitingForOldProductTasks && station.shortage) {
        return LiveShortageTaskState::WaitingOldProductTasks;
    }

    for (auto it = m_tasks.cbegin(); it != m_tasks.cend(); ++it) {
        if (it->stationId != station.stationId) {
            continue;
        }
        if (it->unloaded) {
            return LiveShortageTaskState::UnloadedFinishing;
        }
        if (it->started) {
            return LiveShortageTaskState::Running;
        }
        return LiveShortageTaskState::Queued;
    }

    if (m_waitingStations.contains(station.stationId)) {
        return LiveShortageTaskState::WaitingForQueue;
    }
    if (m_communicationPaused) {
        return LiveShortageTaskState::CommunicationPaused;
    }
    return LiveShortageTaskState::Normal;
}

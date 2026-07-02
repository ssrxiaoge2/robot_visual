#include "shortagemonitor.h"

namespace {

constexpr int kPollIntervalMs = 5000;
constexpr int kRoundTimeoutMs = 3000;

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

QList<StationConsumption> ShortageMonitor::snapshot() const
{
    return m_calculator.snapshot();
}

void ShortageMonitor::start()
{
    if (m_running || !m_client) {
        return;
    }

    m_running = true;
    emit statusChanged(QStringLiteral("现场系统待首轮数据"), true);
    emit logMessage(QStringLiteral("[ShortageMonitor] 开始现场缺料监听"));
    beginRound();
    m_pollTimer->start();
}

void ShortageMonitor::stop()
{
    if (!m_running) {
        return;
    }

    m_running = false;
    m_pollTimer->stop();
    m_roundTimeout->stop();
    m_round = RoundState{};
    emit statusChanged(QStringLiteral("现场系统监听已停止"), false);
    emit logMessage(QStringLiteral("[ShortageMonitor] 停止现场缺料监听"));
}

void ShortageMonitor::confirmShortageAccepted(int stationId, bool accepted)
{
    if (!m_running) {
        return;
    }

    if (!accepted) {
        const QString reason = QStringLiteral("工位%1 缺料事件被主调度拒收，保留待重试").arg(stationId);
        m_calculator.markRejected(stationId);
        if (m_lastRejectedReason != reason) {
            m_lastRejectedReason = reason;
            emit logMessage(QStringLiteral("[ShortageMonitor] %1").arg(reason));
        }
        emit statusChanged(reason, false);
        emitSnapshot();
        return;
    }

    if (!m_calculator.confirmAccepted(stationId)) {
        emit logMessage(QStringLiteral("[ShortageMonitor] 工位%1 缺料确认失败：当前没有待确认阈值").arg(stationId));
        emitSnapshot();
        return;
    }

    m_lastRejectedReason.clear();
    emitSnapshot();
    if (!m_hasLastSample) {
        return;
    }

    const IngestResult refresh = m_calculator.ingest(m_lastSample);
    if (!refresh.ok) {
        emit statusChanged(refresh.errorMessage, false);
        emit logMessage(QStringLiteral("[ShortageMonitor] 刷新待派单工位失败：%1").arg(refresh.errorMessage));
        emitSnapshot();
        return;
    }

    emitSnapshot();
    emitPendingStations(refresh.pendingStations);
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

    ProductModel product = ProductModel::Model88;
    QString productError;
    if (!chooseProduct(bits, &product, &productError)) {
        failRound(productError);
        return;
    }

    ProductionMode mode = ProductionMode::L68;
    QString modeWarning;
    QString modeError;
    if (!chooseMode(bits, &mode, &modeWarning, &modeError)) {
        failRound(modeError);
        return;
    }

    if (!modeWarning.isEmpty() && m_lastModeWarning != modeWarning) {
        m_lastModeWarning = modeWarning;
        emit logMessage(QStringLiteral("[ShortageMonitor] %1").arg(modeWarning));
    } else if (modeWarning.isEmpty()) {
        m_lastModeWarning.clear();
    }

    m_lastSample = ShortageSample{m_round.actualQty, product, mode};
    m_hasLastSample = true;
    emit sampleUpdated(m_lastSample.actualQty, m_lastSample.product, m_lastSample.mode, bits);

    const IngestResult ingestResult = m_calculator.ingest(m_lastSample);
    if (!ingestResult.ok) {
        failRound(ingestResult.errorMessage);
        return;
    }

    emit statusChanged(QStringLiteral("现场系统监听正常"), true);
    emitSnapshot();
    m_round = RoundState{};
    emitPendingStations(ingestResult.pendingStations);
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
                                 QString *warning,
                                 QString *error) const
{
    const bool l68 = bits.value(QStringLiteral("L68"), false);
    const bool l69 = bits.value(QStringLiteral("L69"), false);
    const bool l1998 = bits.value(QStringLiteral("L1998"), false);
    const int trueCount = (l68 ? 1 : 0) + (l69 ? 1 : 0) + (l1998 ? 1 : 0);
    if (trueCount == 0) {
        if (error) {
            *error = QStringLiteral("PLC 生产方式位无效：L68/L69/L1998 不能全为 false");
        }
        return false;
    }

    if (warning && trueCount > 1) {
        *warning = QStringLiteral("PLC 生产方式位同时为 true，按 L68 -> L69 -> L1998 优先级取首个");
    }

    if (mode) {
        *mode = l68 ? ProductionMode::L68
                    : (l69 ? ProductionMode::L69 : ProductionMode::L1998);
    }
    return true;
}

void ShortageMonitor::emitPendingStations(const QList<int> &stations)
{
    for (int stationId : stations) {
        if (stationId < 1 || stationId > 12) {
            continue;
        }
        emit shortageRequested(stationId);
    }
}

void ShortageMonitor::emitSnapshot()
{
    emit consumptionUpdated(m_calculator.snapshot());
}

bool ShortageMonitor::hasIncompleteCurrentRound() const
{
    return m_round.roundId != 0;
}

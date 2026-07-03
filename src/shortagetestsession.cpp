#include "shortagetestsession.h"

namespace {

constexpr int kPollIntervalMs = 5000;
constexpr int kRoundTimeoutMs = 3000;

} // namespace

ShortageTestSession::ShortageTestSession(CustomSysScheduler *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
    , m_pollTimer(new QTimer(this))
    , m_roundTimeout(new QTimer(this))
{
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout,
            this, &ShortageTestSession::onPollTimerTimeout);

    m_roundTimeout->setSingleShot(true);
    connect(m_roundTimeout, &QTimer::timeout,
            this, &ShortageTestSession::onRoundTimeout);

    if (m_client) {
        connect(m_client, &CustomSysScheduler::mesReplyReady,
                this, &ShortageTestSession::onMesReplyReady);
        connect(m_client, &CustomSysScheduler::plcReplyReady,
                this, &ShortageTestSession::onPlcReplyReady);
    }
}

bool ShortageTestSession::isRunning() const
{
    return m_running;
}

QList<StationConsumption> ShortageTestSession::snapshot() const
{
    return m_calculator.snapshot();
}

void ShortageTestSession::start()
{
    if (m_running || !m_client) {
        return;
    }

    ++m_sessionId;
    m_running = true;
    m_round = RoundState{};
    m_stableState = StableState::Unknown;
    m_hasAcceptedActualQty = false;
    m_lastAcceptedActualQty = 0;
    emit statusChanged(QStringLiteral("等待首轮基线"), true);
    beginRound();
    m_pollTimer->start();
}

void ShortageTestSession::stop()
{
    if (!m_running) {
        return;
    }

    ++m_sessionId;
    m_running = false;
    m_pollTimer->stop();
    m_roundTimeout->stop();
    m_round = RoundState{};
    emit statusChanged(QStringLiteral("缺料信号测试已停止"), false);
}

void ShortageTestSession::onPollTimerTimeout()
{
    if (!m_running || hasIncompleteCurrentRound()) {
        return;
    }
    beginRound();
}

void ShortageTestSession::onRoundTimeout()
{
    if (!m_running || !hasIncompleteCurrentRound()) {
        return;
    }
    failRound(QStringLiteral("现场系统 3 秒内未返回完整四请求"));
}

void ShortageTestSession::onMesReplyReady(quint64 roundId, bool ok, qint64 actualQty, QString error)
{
    if (!m_running || !hasIncompleteCurrentRound()
        || roundId != m_round.roundId || m_round.sessionId != m_sessionId) {
        return;
    }

    m_round.mesReceived = true;
    if (!ok) {
        failRound(error.isEmpty() ? QStringLiteral("MES 响应失败") : error);
        return;
    }
    m_round.actualQty = actualQty;
    completeRoundIfReady();
}

void ShortageTestSession::onPlcReplyReady(quint64 roundId,
                                          int startAddress,
                                          CustomSysScheduler::PlcBitReply result)
{
    if (!m_running || !hasIncompleteCurrentRound()
        || roundId != m_round.roundId || m_round.sessionId != m_sessionId) {
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
                      ? QStringLiteral("PLC 分片失败")
                      : result.errorMessage);
        return;
    }
    completeRoundIfReady();
}

void ShortageTestSession::beginRound()
{
    if (!m_client) {
        return;
    }

    static quint64 nextRoundId = 1;
    m_round = RoundState{};
    m_round.roundId = nextRoundId++;
    m_round.sessionId = m_sessionId;
    m_roundTimeout->start(kRoundTimeoutMs);

    m_client->fetchMesDayData(m_round.roundId);
    m_client->fetchPlcBits(m_round.roundId, 68, 2);
    m_client->fetchPlcBits(m_round.roundId, 71, 3);
    m_client->fetchPlcBits(m_round.roundId, 1998, 1);
}

void ShortageTestSession::completeRoundIfReady()
{
    if (!hasIncompleteCurrentRound()
        || !m_round.mesReceived
        || !m_round.plc68Received
        || !m_round.plc71Received
        || !m_round.plc1998Received) {
        return;
    }

    QHash<QString, bool> bits = m_round.plc68.bits;
    for (auto it = m_round.plc71.bits.cbegin(); it != m_round.plc71.bits.cend(); ++it)
        bits.insert(it.key(), it.value());
    for (auto it = m_round.plc1998.bits.cbegin(); it != m_round.plc1998.bits.cend(); ++it)
        bits.insert(it.key(), it.value());

    ProductModel product = ProductModel::Model88;
    QString productError;
    if (!chooseProduct(bits, &product, &productError)) {
        failRound(productError);
        return;
    }

    ProductionMode mode = ProductionMode::L68;
    QString modeError;
    if (!chooseMode(bits, &mode, &modeError)) {
        failRound(modeError);
        return;
    }

    const qint64 delta = m_hasAcceptedActualQty ? (m_round.actualQty - m_lastAcceptedActualQty) : 0;
    emit sampleUpdated(m_round.actualQty, delta, product, mode, bits);

    // 第一轮先建立候选和库存快照；只有连续两轮一致后才按 delta 正式扣减。
    if (m_stableState == StableState::Unknown
        || product != m_candidateProduct
        || mode != m_candidateMode) {
        m_candidateProduct = product;
        m_candidateMode = mode;
        m_confirmedProduct = product;
        m_confirmedMode = mode;
        m_stableState = StableState::WaitingSecondRound;
        m_calculator.initializeForProduct(product);
        // 第一轮稳定确认前仍要把 actualQty 记为纯计算器基线，这样第二轮才能产生 delta 扣减。
        const IngestResult baselineResult = m_calculator.ingest({m_round.actualQty, product, mode});
        if (!baselineResult.ok) {
            failRound(baselineResult.errorMessage);
            return;
        }
        emit statusChanged(QStringLiteral("等待生产信号稳定"), true);
        emitInventory();
        m_hasAcceptedActualQty = true;
        m_lastAcceptedActualQty = m_round.actualQty;
        m_roundTimeout->stop();
        m_round = RoundState{};
        return;
    }

    m_stableState = StableState::Confirmed;
    m_confirmedProduct = product;
    m_confirmedMode = mode;

    const IngestResult result = m_calculator.ingest({m_round.actualQty, product, mode});
    if (!result.ok) {
        failRound(result.errorMessage);
        return;
    }

    emit statusChanged(QStringLiteral("现场系统测试正常"), true);
    emitInventory();
    m_hasAcceptedActualQty = true;
    m_lastAcceptedActualQty = m_round.actualQty;
    m_roundTimeout->stop();
    m_round = RoundState{};
}

void ShortageTestSession::failRound(const QString &reason)
{
    m_roundTimeout->stop();
    m_round = RoundState{};
    m_calculator.markCommunicationInterrupted();
    emit statusChanged(reason, false);
    emitInventory();
}

bool ShortageTestSession::hasIncompleteCurrentRound() const
{
    return m_round.roundId != 0;
}

void ShortageTestSession::emitInventory()
{
    emit inventoryUpdated(m_calculator.snapshot());
}

bool ShortageTestSession::chooseProduct(const QHash<QString, bool> &bits,
                                        ProductModel *product,
                                        QString *error) const
{
    const bool l71 = bits.value(QStringLiteral("L71"), false);
    const bool l72 = bits.value(QStringLiteral("L72"), false);
    const bool l73 = bits.value(QStringLiteral("L73"), false);
    const int trueCount = (l71 ? 1 : 0) + (l72 ? 1 : 0) + (l73 ? 1 : 0);
    if (trueCount != 1) {
        if (error)
            *error = QStringLiteral("产品信号不唯一");
        return false;
    }

    if (product) {
        *product = l71 ? ProductModel::Model88
                       : (l72 ? ProductModel::Model88R : ProductModel::Model92);
    }
    return true;
}

bool ShortageTestSession::chooseMode(const QHash<QString, bool> &bits,
                                     ProductionMode *mode,
                                     QString *error) const
{
    const bool l68 = bits.value(QStringLiteral("L68"), false);
    const bool l69 = bits.value(QStringLiteral("L69"), false);
    const bool l1998 = bits.value(QStringLiteral("L1998"), false);
    const int trueCount = (l68 ? 1 : 0) + (l69 ? 1 : 0) + (l1998 ? 1 : 0);
    if (trueCount != 1) {
        if (error)
            *error = QStringLiteral("生产方式信号不唯一");
        return false;
    }

    if (mode) {
        *mode = l68 ? ProductionMode::L68
                    : (l69 ? ProductionMode::L69 : ProductionMode::L1998);
    }
    return true;
}

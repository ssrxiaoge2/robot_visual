#include "shortagesamplecoordinator.h"

#include <QStringList>
#include <QtGlobal>

namespace {

constexpr int kModeStart68 = 68;
constexpr int kModeLength68 = 2;
constexpr int kProductStart71 = 71;
constexpr int kProductLength71 = 3;
constexpr int kModeStart1998 = 1998;
constexpr int kModeLength1998 = 1;
constexpr int kRequiredStableRounds = 2;

QString contextText(ProductModel product, ProductionMode mode)
{
    const QString productText = product == ProductModel::Model88
        ? QStringLiteral("88")
        : product == ProductModel::Model88R ? QStringLiteral("88R") : QStringLiteral("92");
    const QString modeText = mode == ProductionMode::LeftRight
        ? QStringLiteral("L/R")
        : mode == ProductionMode::LeftOnly ? QStringLiteral("L/L") : QStringLiteral("R/H");
    return QStringLiteral("%1/%2").arg(productText, modeText);
}

} // namespace

ShortageSampleCoordinator::ShortageSampleCoordinator(CustomSysScheduler *scheduler,
                                                     QObject *parent)
    : QObject(parent)
    , m_scheduler(scheduler)
{
    qRegisterMetaType<ShortageSample>("ShortageSample");
    qRegisterMetaType<ShortageCommunicationState>("ShortageCommunicationState");
    qRegisterMetaType<ProductModel>("ProductModel");
    qRegisterMetaType<ProductionMode>("ProductionMode");

    m_roundTimeoutTimer.setSingleShot(true);
    m_nextRoundTimer.setSingleShot(true);

    connect(&m_roundTimeoutTimer, &QTimer::timeout,
            this, &ShortageSampleCoordinator::onRoundTimeout);
    connect(&m_nextRoundTimer, &QTimer::timeout,
            this, &ShortageSampleCoordinator::beginRound);

    if (m_scheduler) {
        connect(m_scheduler, &CustomSysScheduler::mesReplyReady,
                this, &ShortageSampleCoordinator::onMesReply);
        connect(m_scheduler, &CustomSysScheduler::plcReplyReady,
                this, &ShortageSampleCoordinator::onPlcReply);
    }
}

void ShortageSampleCoordinator::setParameters(const ShortageParameters &parameters)
{
    m_parameters = parameters;
    m_parameters.sampleIntervalSeconds =
        boundedSampleIntervalSeconds(parameters.sampleIntervalSeconds);
    m_parameters.roundTimeoutSeconds =
        boundedRoundTimeoutSeconds(parameters.roundTimeoutSeconds,
                                   m_parameters.sampleIntervalSeconds);
    m_parameters.communicationAlarmMinutes =
        boundedAlarmMinutes(parameters.communicationAlarmMinutes);
}

void ShortageSampleCoordinator::triggerNextRoundForTest()
{
    if (!m_running)
        return;
    m_nextRoundTimer.stop();
    beginRound();
}

void ShortageSampleCoordinator::triggerRoundTimeoutForTest()
{
    onRoundTimeout();
}

void ShortageSampleCoordinator::advanceFailureDurationForTest(int seconds)
{
    if (!m_failureStartedAtUtc.isValid())
        m_failureStartedAtUtc = QDateTime::currentDateTimeUtc();
    m_failureStartedAtUtc = m_failureStartedAtUtc.addSecs(-qMax(0, seconds));
}

void ShortageSampleCoordinator::activateConfirmedContextForTestOrCaller(ProductModel product,
                                                                        ProductionMode mode)
{
    if (!m_hasPendingConfirmedContext
        || product != m_pendingProduct
        || mode != m_pendingMode) {
        return;
    }

    m_hasStableContext = true;
    m_stableProduct = product;
    m_stableMode = mode;
    m_hasPendingConfirmedContext = false;
    m_hasCandidateContext = false;
    m_candidateCount = 0;
}

void ShortageSampleCoordinator::start()
{
    if (m_running)
        return;

    m_running = true;
    beginRound();
}

void ShortageSampleCoordinator::stop()
{
    m_running = false;
    m_roundTimeoutTimer.stop();
    m_nextRoundTimer.stop();
    m_round.active = false;
    m_state = ShortageCommunicationState::Stopped;
    emit communicationStateChanged(m_state, QStringLiteral("采样已人工停止"));
}

void ShortageSampleCoordinator::onMesReply(quint64 roundId,
                                           CustomSysScheduler::LiveMesDayReply reply)
{
    if (!m_running || !m_round.active || roundId != m_round.roundId)
        return;

    if (!reply.ok) {
        rejectActiveRound(QStringLiteral("MES 日数据失败：%1").arg(reply.errorMessage));
        return;
    }

    if (reply.actualQty < 0) {
        rejectActiveRound(QStringLiteral("MES actualQty 非法：%1，必须为 0～int64 最大值")
                              .arg(reply.actualQty));
        return;
    }

    m_round.hasMes = true;
    m_round.actualQty = reply.actualQty;
    tryCompleteRound();
}

void ShortageSampleCoordinator::onPlcReply(quint64 roundId,
                                           int startAddress,
                                           CustomSysScheduler::PlcBitReply reply)
{
    if (!m_running || !m_round.active || roundId != m_round.roundId)
        return;

    if (!reply.ok) {
        rejectActiveRound(QStringLiteral("PLC L%1 读取失败：%2")
                              .arg(startAddress)
                              .arg(reply.errorMessage));
        return;
    }

    QString reason;
    if (startAddress == kModeStart68) {
        if (!validatePlcRange(reply, startAddress, {68, 69}, &reason)) {
            rejectActiveRound(reason);
            return;
        }
        m_round.hasMode68 = true;
        m_round.l68 = reply.values.value(68);
        m_round.l69 = reply.values.value(69);
    } else if (startAddress == kProductStart71) {
        if (!validatePlcRange(reply, startAddress, {71, 72, 73}, &reason)) {
            rejectActiveRound(reason);
            return;
        }
        m_round.hasProduct71 = true;
        m_round.l71 = reply.values.value(71);
        m_round.l72 = reply.values.value(72);
        m_round.l73 = reply.values.value(73);
    } else if (startAddress == kModeStart1998) {
        if (!validatePlcRange(reply, startAddress, {1998}, &reason)) {
            rejectActiveRound(reason);
            return;
        }
        m_round.hasMode1998 = true;
        m_round.l1998 = reply.values.value(1998);
    } else {
        rejectActiveRound(QStringLiteral("PLC 起始地址异常：L%1，不属于正式缺料四请求").arg(startAddress));
        return;
    }

    tryCompleteRound();
}

void ShortageSampleCoordinator::onRoundTimeout()
{
    if (!m_running || !m_round.active)
        return;

    QStringList missing;
    if (!m_round.hasMes)
        missing.append(QStringLiteral("MES actualQty"));
    if (!m_round.hasMode68)
        missing.append(QStringLiteral("PLC L68-L69"));
    if (!m_round.hasProduct71)
        missing.append(QStringLiteral("PLC L71-L73"));
    if (!m_round.hasMode1998)
        missing.append(QStringLiteral("PLC L1998"));

    rejectActiveRound(QStringLiteral("round=%1 超时，缺少：%2")
                          .arg(m_round.roundId)
                          .arg(missing.join(QStringLiteral("、"))));
}

void ShortageSampleCoordinator::beginRound()
{
    if (!m_running)
        return;

    m_nextRoundTimer.stop();
    m_roundTimeoutTimer.stop();

    m_activeRoundIntervalSeconds =
        boundedSampleIntervalSeconds(m_parameters.sampleIntervalSeconds);
    m_activeRoundTimeoutSeconds =
        boundedRoundTimeoutSeconds(m_parameters.roundTimeoutSeconds,
                                   m_activeRoundIntervalSeconds);

    m_round = RoundAccumulator{};
    m_round.roundId = m_nextRoundId++;
    m_round.active = true;
    m_round.startedAtUtc = QDateTime::currentDateTimeUtc();

    markCommunicationHealthy(QStringLiteral("开始采样 round=%1").arg(m_round.roundId));
    requestRound(m_round.roundId);
    m_roundTimeoutTimer.start(m_activeRoundTimeoutSeconds * 1000);
}

void ShortageSampleCoordinator::requestRound(quint64 roundId)
{
    if (!m_scheduler) {
        rejectActiveRound(QStringLiteral("未配置真实缺料通信对象"));
        return;
    }

    m_scheduler->fetchMesDayData(roundId);
    m_scheduler->fetchPlcBits(roundId, kModeStart68, kModeLength68);
    m_scheduler->fetchPlcBits(roundId, kProductStart71, kProductLength71);
    m_scheduler->fetchPlcBits(roundId, kModeStart1998, kModeLength1998);
}

void ShortageSampleCoordinator::tryCompleteRound()
{
    if (!m_round.active || !m_round.hasMes || !m_round.hasMode68
        || !m_round.hasProduct71 || !m_round.hasMode1998) {
        return;
    }

    ProductModel product = ProductModel::Model88;
    ProductionMode mode = ProductionMode::LeftRight;
    QString reason;
    if (!resolveProduct(&product, &reason)) {
        rejectActiveRound(reason);
        return;
    }
    if (!resolveMode(&mode, &reason)) {
        rejectActiveRound(reason);
        return;
    }

    m_round.active = false;
    m_roundTimeoutTimer.stop();
    handleCompletedRound(product, mode);
    scheduleNextRound();
}

void ShortageSampleCoordinator::rejectActiveRound(const QString &reason)
{
    if (!m_round.active)
        return;

    const quint64 roundId = m_round.roundId;
    m_round.active = false;
    m_roundTimeoutTimer.stop();
    emit sampleRejected(roundId, reason);
    markCommunicationFailed(reason);
    scheduleNextRound();
}

void ShortageSampleCoordinator::rejectRecoveryNeedsReview(const QString &reason)
{
    const quint64 roundId = m_round.roundId;
    m_running = false;
    m_round.active = false;
    m_roundTimeoutTimer.stop();
    m_nextRoundTimer.stop();
    emit sampleRejected(roundId, reason);
    m_state = ShortageCommunicationState::RecoveryNeedsReview;
    emit communicationStateChanged(m_state, reason);
}

void ShortageSampleCoordinator::markCommunicationHealthy(const QString &reason)
{
    m_state = ShortageCommunicationState::Sampling;
    emit communicationStateChanged(m_state, reason);
}

void ShortageSampleCoordinator::markCommunicationFailed(const QString &reason)
{
    if (!m_failureStartedAtUtc.isValid())
        m_failureStartedAtUtc = QDateTime::currentDateTimeUtc();

    m_interruptedSinceLastSample = true;
    const qint64 failedSeconds = m_failureStartedAtUtc.secsTo(QDateTime::currentDateTimeUtc());
    const qint64 alarmSeconds = qint64{m_parameters.communicationAlarmMinutes} * 60;
    m_state = failedSeconds >= alarmSeconds
        ? ShortageCommunicationState::Alarm
        : ShortageCommunicationState::Interrupted;
    emit communicationStateChanged(m_state, reason);
}

void ShortageSampleCoordinator::scheduleNextRound()
{
    if (!m_running)
        return;
    m_nextRoundTimer.start(qMax(1, m_activeRoundIntervalSeconds) * 1000);
}

void ShortageSampleCoordinator::handleCompletedRound(ProductModel product,
                                                     ProductionMode mode)
{
    const bool sameAsStable = m_hasStableContext
        && product == m_stableProduct
        && mode == m_stableMode;

    if (sameAsStable) {
        m_hasCandidateContext = false;
        m_candidateCount = 0;
        const bool baselineMatchesContext = m_hasSampleBaseline
            && product == m_baselineProduct
            && mode == m_baselineMode;
        if (m_interruptedSinceLastSample && baselineMatchesContext
            && m_round.actualQty < m_lastStableActualQty) {
            rejectRecoveryNeedsReview(
                QStringLiteral("断线恢复 actualQty=%1 小于旧基线 %2，必须联系维护人员确认")
                    .arg(m_round.actualQty)
                    .arg(m_lastStableActualQty));
            return;
        }
        emitStableSample(product, mode, m_interruptedSinceLastSample);
        return;
    }

    if (m_hasPendingConfirmedContext
        && product == m_pendingProduct
        && mode == m_pendingMode) {
        return;
    }

    if (!m_hasCandidateContext
        || product != m_candidateProduct
        || mode != m_candidateMode) {
        m_hasCandidateContext = true;
        m_candidateProduct = product;
        m_candidateMode = mode;
        m_candidateCount = 1;
        return;
    }

    ++m_candidateCount;
    if (m_candidateCount < kRequiredStableRounds)
        return;

    const bool initialContext = !m_hasStableContext;
    m_hasCandidateContext = false;
    m_candidateCount = 0;
    m_hasPendingConfirmedContext = true;
    m_pendingProduct = product;
    m_pendingMode = mode;
    emit contextChangeConfirmed(product, mode);

    if (initialContext) {
        // 初次稳定没有旧上下文需要排空，必须把样本交给上层建立基线。
        m_hasStableContext = true;
        m_stableProduct = product;
        m_stableMode = mode;
        m_hasPendingConfirmedContext = false;
        emitStableSample(product, mode, m_interruptedSinceLastSample);
        return;
    }

    // 换型只确认待切换上下文；旧任务排空前不得覆盖 stable context 或输出新上下文样本。
}

void ShortageSampleCoordinator::emitStableSample(ProductModel product,
                                                 ProductionMode mode,
                                                 bool recoveredAfterInterruption)
{
    ShortageSample sample;
    sample.roundId = m_round.roundId;
    sample.product = product;
    sample.mode = mode;
    sample.actualQty = m_round.actualQty;
    sample.capturedAtUtc = QDateTime::currentDateTimeUtc();
    sample.recoveredAfterInterruption = recoveredAfterInterruption;

    m_hasSampleBaseline = true;
    m_baselineProduct = product;
    m_baselineMode = mode;
    m_lastStableActualQty = sample.actualQty;
    m_interruptedSinceLastSample = false;
    m_failureStartedAtUtc = QDateTime();

    emit stableSampleReady(sample);
}

bool ShortageSampleCoordinator::validatePlcRange(const CustomSysScheduler::PlcBitReply &reply,
                                                 int startAddress,
                                                 std::initializer_list<int> addresses,
                                                 QString *reason) const
{
    for (int address : addresses) {
        if (!reply.values.contains(address)) {
            if (reason)
                *reason = QStringLiteral("PLC L%1 响应缺少 L%2").arg(startAddress).arg(address);
            return false;
        }
    }
    return true;
}

bool ShortageSampleCoordinator::resolveProduct(ProductModel *product, QString *reason) const
{
    const int trueCount = int(m_round.l71) + int(m_round.l72) + int(m_round.l73);
    if (trueCount != 1) {
        if (reason) {
            *reason = QStringLiteral("产品三选一无效，必须 L71/L72/L73 恰好一个为 true；当前 %1")
                          .arg(allBitValuesText());
        }
        return false;
    }

    if (m_round.l71)
        *product = ProductModel::Model88;
    else if (m_round.l72)
        *product = ProductModel::Model88R;
    else
        *product = ProductModel::Model92;
    return true;
}

bool ShortageSampleCoordinator::resolveMode(ProductionMode *mode, QString *reason) const
{
    const int trueCount = int(m_round.l68) + int(m_round.l69) + int(m_round.l1998);
    if (trueCount != 1) {
        if (reason) {
            *reason = QStringLiteral("模式三选一无效，必须 L68/L69/L1998 恰好一个为 true；当前 %1")
                          .arg(allBitValuesText());
        }
        return false;
    }

    if (m_round.l68)
        *mode = ProductionMode::LeftRight;
    else if (m_round.l69)
        *mode = ProductionMode::LeftOnly;
    else
        *mode = ProductionMode::RightOnly;
    return true;
}

QString ShortageSampleCoordinator::allBitValuesText() const
{
    return QStringList{
        bitText(QStringLiteral("L68"), m_round.l68),
        bitText(QStringLiteral("L69"), m_round.l69),
        bitText(QStringLiteral("L71"), m_round.l71),
        bitText(QStringLiteral("L72"), m_round.l72),
        bitText(QStringLiteral("L73"), m_round.l73),
        bitText(QStringLiteral("L1998"), m_round.l1998)
    }.join(QStringLiteral("，"));
}

QString ShortageSampleCoordinator::bitText(const QString &name, bool value)
{
    return QStringLiteral("%1=%2").arg(name, value ? QStringLiteral("true") : QStringLiteral("false"));
}

int ShortageSampleCoordinator::boundedSampleIntervalSeconds(int value)
{
    return qBound(5, value, 300);
}

int ShortageSampleCoordinator::boundedRoundTimeoutSeconds(int value, int intervalSeconds)
{
    const int bounded = qBound(1, value, 30);
    return qMin(bounded, qMax(1, intervalSeconds - 1));
}

int ShortageSampleCoordinator::boundedAlarmMinutes(int value)
{
    return qBound(1, value, 60);
}

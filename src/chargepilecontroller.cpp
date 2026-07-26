#include "chargepilecontroller.h"

#include "chargepileprotocol.h"

#include <QMetaType>
#include <QtMath>

namespace {

constexpr quint8 kReadHoldingRegisters = 0x03;
constexpr quint8 kReadInputRegisters = 0x04;
constexpr quint8 kWriteSingleCoil = 0x05;
constexpr quint8 kWriteSingleRegister = 0x06;

// 现场 Python 脚本规定相邻 RTU 请求至少间隔 50ms。Qt 事件分发存在毫秒级抖动，
// 因此这里从上一帧完整响应后保守等待 70ms，避免对端实际观测值短于 50ms。
constexpr int kMinimumRequestIntervalMs = 70;
constexpr int kMaximumRecoveryConnectAttempts = 2;

// 厂家 2026-07-04 现场确认：启动前和充电监控可忽略 E2/E3/E4/E7/E8。
constexpr quint16 kPrestartIgnoredErrors =
    (1u << 1) | (1u << 2) | (1u << 3) | (1u << 6) | (1u << 7);
// 缩回时 BMS 已脱离触点，现场 Python 只允许忽略 E8，机械故障绝不豁免。
constexpr quint16 kRetractIgnoredErrors = (1u << 7);

bool snapshotConfirmsSafe(const ChargePileSnapshot &snapshot,
                          const ChargeSettings &settings)
{
    return snapshot.retracted
           && !snapshot.extended
           && !snapshot.working
           && !snapshot.relayOn
           && snapshot.outputCurrentA <= settings.safeCurrentA;
}

} // namespace

std::optional<ChargePileWriteIdentity>
chargePileWriteIdentity(const QByteArrayView frame)
{
    if (frame.size() < 6)
        return std::nullopt;

    const quint8 function = quint8(frame.at(1));
    if (function != kWriteSingleCoil
        && function != kWriteSingleRegister) {
        return std::nullopt;
    }

    const auto wordAt = [frame](const qsizetype offset) {
        return quint16((quint16(quint8(frame.at(offset))) << 8)
                       | quint8(frame.at(offset + 1)));
    };
    return ChargePileWriteIdentity{
        function,
        wordAt(2),
        wordAt(4)
    };
}

ChargePileController::ChargePileController(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<ChargePileSnapshot>("ChargePileSnapshot");
    qRegisterMetaType<ChargePileController::State>("ChargePileController::State");
    qRegisterMetaType<ChargePileController::SessionOrigin>(
        "ChargePileController::SessionOrigin");
    qRegisterMetaType<ChargePileController::StopReason>(
        "ChargePileController::StopReason");

    m_responseTimer.setSingleShot(true);
    m_actionPollTimer.setSingleShot(true);
    m_phaseTimer.setSingleShot(true);

    connect(&m_socket, &QTcpSocket::connected,
            this, &ChargePileController::handleConnected);
    connect(&m_socket, &QTcpSocket::readyRead,
            this, &ChargePileController::handleReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred,
            this, &ChargePileController::handleSocketError);
    connect(&m_responseTimer, &QTimer::timeout,
            this, &ChargePileController::handleResponseTimeout);
    connect(&m_actionPollTimer, &QTimer::timeout,
            this, &ChargePileController::handleActionTimer);
    connect(&m_phaseTimer, &QTimer::timeout,
            this, &ChargePileController::handlePhaseTimer);
}

bool ChargePileController::canApplySettings(const ChargeSettings &settings,
                                            QString *error) const
{
    if (error)
        error->clear();

    const ChargeSettingsValidation validation = validateChargeSettings(settings);
    if (!validation.ok) {
        if (error) {
            *error = QStringLiteral("充电桩设置校验失败：%1")
                         .arg(validation.errors.join(QStringLiteral("；")));
        }
        return false;
    }

    // 从正常充电接受到最终完整快照明确安全之前，整份设置都属于同一个安全
    // 恢复上下文。即使上一轮已经以 Fault/Unknown 结束，也不能切换目标、放宽
    // safeCurrentA 或改变恢复时序，否则旧桩里程碑会被错误应用到新桩。
    if (m_safetyRecoveryContextValid) {
        if (error) {
            *error =
                QStringLiteral("[充电会话%1][%2] 安全恢复上下文尚未最终确认安全，拒绝修改整份充电设置。")
                    .arg(m_sessionId).arg(originText(m_sessionOrigin));
        }
        return false;
    }
    if (m_queryInProgress) {
        if (error)
            *error = QStringLiteral("控制器正在执行查询或充电会话，拒绝修改整份充电设置。");
        return false;
    }
    return true;
}

bool ChargePileController::applySettings(const ChargeSettings &settings,
                                         QString *error)
{
    QString rejection;
    if (!canApplySettings(settings, &rejection)) {
        if (error)
            *error = rejection;
        emit logMessage(rejection);
        return false;
    }
    if (error)
        error->clear();

    const bool targetChanged = communicationTargetChanged(m_settings, settings);
    const bool hasCommunicationContext =
        m_queryInProgress
        || m_socket.state() != QAbstractSocket::UnconnectedState
        || m_snapshot.sampledAt.isValid()
        || m_pendingRequest.has_value()
        || m_inFlightRequest.has_value();
    m_settings = settings;
    if (targetChanged && hasCommunicationContext) {
        invalidateCommunicationContext(
            QStringLiteral("通信目标已变更，旧连接和状态快照已失效。"));
    }
    return true;
}

void ChargePileController::queryStatus()
{
    if (m_queryInProgress) {
        const QString reason = QStringLiteral("状态查询正在进行或充电会话正在执行，拒绝并发查询。");
        emit logMessage(reason);
        emit queryFinished(false, reason);
        return;
    }

    const ChargeSettingsValidation validation = validateChargeSettings(m_settings);
    if (!validation.ok) {
        const QString reason = QStringLiteral("充电桩设置无效：%1")
                                   .arg(validation.errors.join(QStringLiteral("；")));
        emit logMessage(reason);
        emit queryFinished(false, reason);
        return;
    }

    m_queryInProgress = true;
    m_flowMode = FlowMode::Query;
    clearTransportWork();
    enqueueSnapshotReads(State::Prechecking,
                         QStringLiteral("正在顺序读取充电桩状态。"),
                         [this] { finishQuery(); });

    if (m_socket.state() == QAbstractSocket::ConnectedState) {
        beginNextRequest();
    } else {
        setState(State::Connecting, QStringLiteral("正在连接充电桩。"));
        m_responseTimer.start(m_settings.connectTimeoutMs);
        m_socket.connectToHost(m_settings.host.trimmed(), m_settings.port);
    }
}

bool ChargePileController::startCharge(const SessionOrigin origin, QString *error)
{
    if (error)
        error->clear();
    if (m_queryInProgress) {
        if (error)
            *error = QStringLiteral("控制器正在执行其他查询或充电会话。");
        return false;
    }
    if (m_unknownGate) {
        if (error)
            *error = QStringLiteral("充电桩状态未知，只允许完整只读查询或保守安全关闭。");
        return false;
    }
    if (m_safetyRecoveryContextValid) {
        if (error) {
            *error =
                QStringLiteral("上一充电安全恢复上下文尚未由最终完整快照确认安全，禁止开始新充电。");
        }
        return false;
    }

    const ChargeSettingsValidation validation = validateChargeSettings(m_settings);
    if (!validation.ok) {
        if (error)
            *error = QStringLiteral("充电桩设置无效：%1")
                         .arg(validation.errors.join(QStringLiteral("；")));
        return false;
    }
    // 即使未来设置校验被其他调用路径绕过，写队列入口仍不允许任何可选值
    // 转换后回绕为另一个16位寄存器值。
    if ((m_settings.cutoffCurrentA.has_value()
         && (qRound64(*m_settings.cutoffCurrentA * 10.0) < 1
             || qRound64(*m_settings.cutoffCurrentA * 10.0) > 0xFFFF))
        || (m_settings.maxChargeSeconds.has_value()
            && (*m_settings.maxChargeSeconds < 1
                || *m_settings.maxChargeSeconds > 0xFFFF))) {
        if (error)
            *error = QStringLiteral("可选充电参数超出16位寄存器范围。");
        return false;
    }

    ++m_sessionId;
    m_sessionOrigin = origin;
    m_stopReason = StopReason::Manual;
    m_seenChargingOutput = false;
    m_safeStopRequested = false;
    m_safeShutdownStarted = false;
    m_applicationShutdownRequested = false;
    m_applicationShutdownFinishedEmitted = false;
    m_sessionFinishedEmitted = false;
    clearSafetyRecoveryContext();
    m_safetyRecoveryContextValid = true;
    m_conservativeRecoveryActive = false;
    m_recoveryAction = RecoveryAction::None;
    m_recoveryAttempts = 0;
    m_recoveryReason.clear();
    m_terminalFailureAfterRetractOff.clear();
    m_queryInProgress = true;
    m_flowMode = FlowMode::Charge;
    clearTransportWork();

    emit logMessage(QStringLiteral("[充电会话%1][%2] 接受启动请求。")
                    .arg(m_sessionId).arg(originText(origin)));
    enqueueSnapshotReads(State::Prechecking,
                         QStringLiteral("步骤1/5：执行连接后完整预检。"),
                         [this] {
        QString reason;
        if (!validatePrestartSnapshot(&reason)) {
            failOperation(reason);
            return;
        }
        beginParameterWrites();
    });

    if (m_socket.state() == QAbstractSocket::ConnectedState) {
        beginNextRequest();
    } else {
        setState(State::Connecting, QStringLiteral("正在连接充电桩。"));
        m_responseTimer.start(m_settings.connectTimeoutMs);
        m_socket.connectToHost(m_settings.host.trimmed(), m_settings.port);
    }
    return true;
}

void ChargePileController::requestSafeStop(const StopReason reason)
{
    if (m_flowMode != FlowMode::Charge || !m_queryInProgress)
        return;

    if (!m_safeStopRequested
        || stopReasonPriority(reason) > stopReasonPriority(m_stopReason)) {
        m_stopReason = reason;
    }
    m_safeStopRequested = true;
    emit logMessage(QStringLiteral("[充电会话%1] 请求安全收尾：%2")
                    .arg(m_sessionId).arg(stopReasonText(m_stopReason)));

    // 收尾入口已经建立后，后续请求只能升级原因。此时停止命令可能处于 70ms
    // 节流待发状态；若再次清空 pending，会既不发送也无法由 beginSafeShutdown()
    // 重建（入口防重标志已置位），从而永久卡在 SendingStop。
    if (m_safeShutdownStarted)
        return;

    // 已经写入套接字的命令必须等待其唯一响应，不能取消后盲目重发；尚未写出的
    // 队列则立即丢弃，使该在途请求结束后直接进入同一个收尾入口。
    m_requestQueue.clear();
    m_onQueueDrained = {};
    m_phaseTimer.stop();
    m_deferredPhase = DeferredPhase::None;
    if (m_pendingRequest.has_value() && !m_inFlightRequest.has_value()) {
        m_actionPollTimer.stop();
        m_pendingRequest.reset();
    }
    if (!m_inFlightRequest.has_value()) {
        if (m_socket.state() == QAbstractSocket::ConnectedState) {
            beginSafeShutdown();
        } else {
            setState(State::Connecting,
                     QStringLiteral("正在重新连接充电桩以执行安全收尾。"));
            m_responseTimer.start(m_settings.connectTimeoutMs);
            m_socket.connectToHost(m_settings.host.trimmed(), m_settings.port);
        }
    }
}

bool ChargePileController::requestConservativeRecovery(
    const SessionOrigin origin, const StopReason reason, QString *error)
{
    return beginConservativeRecovery(origin, reason, false, error);
}

bool ChargePileController::beginConservativeRecovery(
    const SessionOrigin origin, const StopReason reason,
    const bool notifyApplication, QString *error)
{
    if (error)
        error->clear();
    if (m_queryInProgress) {
        if (error)
            *error = QStringLiteral("控制器仍有查询、充电或恢复操作在途。");
        return false;
    }
    const ChargeSettingsValidation validation = validateChargeSettings(m_settings);
    if (!validation.ok) {
        if (error)
            *error = QStringLiteral("充电桩设置无效：%1")
                         .arg(validation.errors.join(QStringLiteral("；")));
        return false;
    }

    // 不安全终态后的第二次、第三次恢复仍属于同一安全上下文。只有控制器此前
    // 没有任何可延续的会话时才建立新编号并初始化里程碑。
    if (!m_safetyRecoveryContextValid) {
        ++m_sessionId;
        clearSafetyRecoveryContext();
        m_safetyRecoveryContextValid = true;
    }
    m_sessionOrigin = origin;
    m_stopReason = reason;
    // 预检五组真实状态完成前保持为 false，避免 handleReadyRead() 在首帧后
    // 把保守恢复误导入普通停止入口并盲目重发结果不确定的 Stop。
    m_safeStopRequested = false;
    m_safeShutdownStarted = false;
    // chargeSessionFinished 是同步信号：自动协调器可能在旧的关闭收尾以
    // unsafe 结束的同一调用栈中立即建立下一轮保守恢复。普通恢复不能把
    // 尚未发布完成结果的应用关闭所有权降级；只有全新的关闭入口才初始化
    // one-shot，继承流程沿用原标志直到最终 safe/unsafe。
    const bool inheritsPendingApplicationShutdown =
        m_applicationShutdownRequested
        && !m_applicationShutdownFinishedEmitted;
    if (notifyApplication) {
        if (!inheritsPendingApplicationShutdown)
            m_applicationShutdownFinishedEmitted = false;
        m_applicationShutdownRequested = true;
    } else {
        m_applicationShutdownRequested =
            inheritsPendingApplicationShutdown;
    }
    m_sessionFinishedEmitted = false;
    m_conservativeRecoveryActive = true;
    m_recoveryAction = RecoveryAction::None;
    m_recoveryAttempts = 0;
    m_recoveryReason.clear();
    m_terminalFailureAfterRetractOff.clear();
    m_queryInProgress = true;
    m_flowMode = FlowMode::Charge;
    clearTransportWork();

    emit logMessage(
        QStringLiteral("[充电会话%1][%2] 接受不安全终态后的保守恢复请求。")
            .arg(m_sessionId).arg(originText(origin)));
    enqueueSnapshotReads(
        State::Prechecking,
        QStringLiteral("保守恢复：先完整读取真实状态，禁止盲目重复未知写命令。"),
        [this] { beginConservativeRecoveryAfterSnapshot(); });

    if (m_socket.state() == QAbstractSocket::ConnectedState) {
        beginNextRequest();
    } else {
        setState(State::Connecting,
                 QStringLiteral("正在连接充电桩以执行保守恢复。"));
        m_responseTimer.start(m_settings.connectTimeoutMs);
        m_socket.connectToHost(m_settings.host.trimmed(), m_settings.port);
    }
    return true;
}

void ChargePileController::requestApplicationShutdown()
{
    // 同一个关闭请求只允许发布一个最终结果；重复调用只升级当前安全流程，
    // 不能重新打开通知边沿或建立第二条停止链。
    if (!m_applicationShutdownRequested) {
        m_applicationShutdownRequested = true;
        m_applicationShutdownFinishedEmitted = false;
    }

    if (m_flowMode == FlowMode::Charge && m_queryInProgress) {
        if (m_conservativeRecoveryActive) {
            if (stopReasonPriority(StopReason::ApplicationShutdown)
                > stopReasonPriority(m_stopReason)) {
                m_stopReason = StopReason::ApplicationShutdown;
            }
            emit logMessage(
                QStringLiteral("[充电会话%1] 程序关闭接管当前保守恢复，仅升级原因，不旁路未知写命令矩阵。")
                    .arg(m_sessionId));
            return;
        }
        requestSafeStop(StopReason::ApplicationShutdown);
        return;
    }

    if (!shutdownRequired()) {
        emitApplicationShutdownFinishedOnce(
            true, QStringLiteral("充电桩已确认处于安全状态。"));
        return;
    }

    // 关闭请求到来时即使没有活动充电会话，也必须先完成五组真实只读快照，
    // 然后复用与人工/自动恢复相同的未知写命令矩阵。禁止直接 beginSafeShutdown()
    // 绕过 Unknown Stop、缩回 OFF 或 Reset 的“不重发”约束。
    if (m_flowMode == FlowMode::Query && m_queryInProgress) {
        emit queryFinished(false, QStringLiteral("状态查询被程序关闭安全收尾接管。"));
        clearTransportWork();
        m_socket.abort();
        m_queryInProgress = false;
        m_flowMode = FlowMode::None;
    }

    QString error;
    const SessionOrigin origin = m_safetyRecoveryContextValid
                                     ? m_sessionOrigin
                                     : SessionOrigin::Manual;
    if (!beginConservativeRecovery(origin, StopReason::ApplicationShutdown,
                                   true, &error)) {
        emitApplicationShutdownFinishedOnce(
            false,
            QStringLiteral("程序关闭安全收尾无法启动：%1").arg(error));
    }
}

bool ChargePileController::isBusy() const
{
    return m_queryInProgress;
}

bool ChargePileController::shutdownRequired() const
{
    return m_safetyRecoveryContextValid
           || m_state != State::SafeComplete
           || !snapshotConfirmsSafe(m_snapshot, m_settings);
}

ChargePileSnapshot ChargePileController::snapshot() const
{
    return m_snapshot;
}

std::optional<ChargePileWriteIdentity>
ChargePileController::uncertainWriteIdentity() const
{
    return m_uncertainWrite;
}

void ChargePileController::enqueueRead(
    const quint8 function, const quint16 address, const quint16 count,
    std::function<void(const QByteArray &response)> onSuccess)
{
    Request request;
    request.expectedSlaveId = m_settings.slaveId;
    request.expectedFunction = function;
    request.frame = ChargePileProtocol::buildReadRequest(
        m_settings.slaveId, function, address, count);
    request.expectedDataBytes = count * 2;
    request.isWriteCommand = false;
    request.onSuccess = std::move(onSuccess);
    m_requestQueue.enqueue(std::move(request));
}

void ChargePileController::enqueueInputRead(
    const quint16 address, const quint16 count,
    std::function<void(const QByteArray &response)> onSuccess)
{
    enqueueRead(kReadInputRegisters, address, count, std::move(onSuccess));
}

void ChargePileController::enqueueWriteRegister(
    const quint16 address, const quint16 value, std::function<void()> onSuccess)
{
    Request request;
    request.expectedSlaveId = m_settings.slaveId;
    request.expectedFunction = kWriteSingleRegister;
    request.frame = ChargePileProtocol::buildWriteRegisterRequest(
        m_settings.slaveId, address, value);
    request.isWriteCommand = true;
    request.onSuccess = [callback = std::move(onSuccess)](const QByteArray &) {
        if (callback)
            callback();
    };
    m_requestQueue.enqueue(std::move(request));
}

void ChargePileController::enqueueWriteCoil(
    const quint16 address, const bool on, std::function<void()> onSuccess)
{
    Request request;
    request.expectedSlaveId = m_settings.slaveId;
    request.expectedFunction = kWriteSingleCoil;
    request.frame = ChargePileProtocol::buildWriteCoilRequest(
        m_settings.slaveId, address, on);
    request.isWriteCommand = true;
    request.onSuccess = [callback = std::move(onSuccess)](const QByteArray &) {
        if (callback)
            callback();
    };
    m_requestQueue.enqueue(std::move(request));
}

void ChargePileController::enqueueSnapshotReads(
    const State state, const QString &text, std::function<void()> onComplete)
{
    setState(state, text);
    enqueueInputRead(ChargePileProtocol::kRegOutVoltage, 2,
                     [this](const QByteArray &response) {
        m_snapshot.outputVoltageV = responseWord(response, 3) / 10.0;
        m_snapshot.outputCurrentA = responseWord(response, 5) / 10.0;
    });
    enqueueInputRead(ChargePileProtocol::kRegInputSignals, 1,
                     [this](const QByteArray &response) {
        m_snapshot.inputWord = responseWord(response, 3);
        m_snapshot.extended = ChargePileProtocol::inputSignalBit(
            m_snapshot.inputWord, ChargePileProtocol::kInputExtendedBit);
        m_snapshot.retracted = ChargePileProtocol::inputSignalBit(
            m_snapshot.inputWord, ChargePileProtocol::kInputRetractedBit);
    });
    enqueueInputRead(ChargePileProtocol::kRegOutputSignals, 1,
                     [this](const QByteArray &response) {
        m_snapshot.outputWord = responseWord(response, 3);
        m_snapshot.working = ChargePileProtocol::outputSignalBit(
            m_snapshot.outputWord, ChargePileProtocol::kOutputWorkingBit);
        m_snapshot.relayOn = ChargePileProtocol::outputSignalBit(
            m_snapshot.outputWord, ChargePileProtocol::kOutputRelayBit);
    });
    enqueueInputRead(ChargePileProtocol::kRegEvent, 1,
                     [this](const QByteArray &response) {
        m_snapshot.eventWord = responseWord(response, 3);
    });
    enqueueInputRead(ChargePileProtocol::kRegError, 1,
                     [this](const QByteArray &response) {
        m_snapshot.faultWord = responseWord(response, 3);
    });
    m_onQueueDrained = [this, callback = std::move(onComplete)] {
        publishSnapshot();
        if (callback)
            callback();
    };
}

void ChargePileController::beginNextRequest()
{
    if (!m_queryInProgress || m_inFlightRequest.has_value()
        || m_pendingRequest.has_value()) {
        return;
    }
    if (!m_requestQueue.isEmpty()) {
        m_pendingRequest = m_requestQueue.dequeue();
        scheduleCurrentRequest();
        return;
    }

    std::function<void()> completed = std::move(m_onQueueDrained);
    m_onQueueDrained = {};
    if (completed)
        completed();
    // 阶段完成回调通常会排入下一阶段请求。回调也可能已主动调用 beginNextRequest()，
    // 因而再次进入时先由函数顶部的 pending/in-flight 保护消除重复发送。
    // 仅当回调确实排入了下一批请求时继续；监控回调可能只启动阶段定时器，
    // 若在空队列上无条件递归会造成栈溢出。
    if (m_queryInProgress && !m_requestQueue.isEmpty())
        beginNextRequest();
}

void ChargePileController::scheduleCurrentRequest()
{
    if (!m_pendingRequest.has_value())
        return;
    if (!m_lastSendTimer.isValid()
        || m_lastSendTimer.elapsed() >= kMinimumRequestIntervalMs) {
        sendCurrentRequest();
        return;
    }
    m_actionPollTimer.start(
        kMinimumRequestIntervalMs - int(m_lastSendTimer.elapsed()));
}

void ChargePileController::sendCurrentRequest()
{
    if (!m_queryInProgress || !m_pendingRequest.has_value())
        return;
    if (m_socket.state() != QAbstractSocket::ConnectedState) {
        failOperation(QStringLiteral("充电桩连接已断开，无法发送请求。"));
        return;
    }

    Request request = std::move(*m_pendingRequest);
    m_pendingRequest.reset();

    // 必须在 write() 前进入可记录上下文。短写或 -1 无法证明设备端完全没有
    // 收到任何字节，因此写命令一律按结果不确定处理，并由 failOperation()
    // 从 m_inFlightRequest 精确保存功能码、地址和值。
    request.startedAt = QDateTime::currentDateTime();
    m_inFlightRequest = std::move(request);
    const qint64 written = writeFrame(m_inFlightRequest->frame);
    if (written != m_inFlightRequest->frame.size()) {
        failOperation(QStringLiteral("充电桩请求写入套接字失败。"),
                      m_inFlightRequest->isWriteCommand);
        return;
    }

    m_lastSendTimer.start();
    m_responseTimer.start(m_settings.responseTimeoutMs);
    emit logMessage(QStringLiteral("[充电会话%1][%2] TX %3")
                    .arg(m_sessionId)
                    .arg(m_flowMode == FlowMode::Charge
                             ? originText(m_sessionOrigin)
                             : QStringLiteral("只读"))
                    .arg(QString::fromLatin1(
                        m_inFlightRequest->frame.toHex(' ').toUpper())));
}

qint64 ChargePileController::writeFrame(const QByteArray &frame)
{
    return m_socket.write(frame);
}

void ChargePileController::handleConnected()
{
    if (!m_queryInProgress)
        return;
    m_responseTimer.stop();
    if (m_recoveryAction != RecoveryAction::None) {
        resumeAfterRecovery();
        return;
    }
    if (m_flowMode == FlowMode::Charge
        && m_safeStopRequested && !m_safeShutdownStarted) {
        beginSafeShutdown();
        return;
    }
    beginNextRequest();
}

void ChargePileController::handleReadyRead()
{
    m_receiveBuffer.append(m_socket.readAll());
    if (!m_queryInProgress) {
        emit logMessage(QStringLiteral("收到已结束操作的充电桩响应。"));
        return;
    }
    if (!m_inFlightRequest.has_value()) {
        handleReadFailure(QStringLiteral("收到不属于已写入请求的充电桩响应。"));
        return;
    }

    const Request request = *m_inFlightRequest;
    const ChargePileProtocol::FrameExtractResult result =
        ChargePileProtocol::takeResponseFrame(
            &m_receiveBuffer, request.expectedSlaveId, request.expectedFunction);
    if (result.status == ChargePileProtocol::FrameExtractStatus::Incomplete)
        return;
    if (result.status == ChargePileProtocol::FrameExtractStatus::Invalid) {
        if (request.isWriteCommand) {
            failOperation(QStringLiteral("充电桩写响应无效：%1").arg(result.reason), true);
        } else {
            handleReadFailure(QStringLiteral("充电桩读响应无效：%1").arg(result.reason));
        }
        return;
    }

    m_responseTimer.stop();
    if (result.exceptionCode != 0) {
        if (request.isWriteCommand) {
            failOperation(QStringLiteral("充电桩写命令返回Modbus异常：%1")
                          .arg(result.reason), true);
        } else {
            handleReadFailure(QStringLiteral("充电桩读取返回Modbus异常：%1")
                              .arg(result.reason));
        }
        return;
    }
    if (!request.isWriteCommand
        && (result.frame.size() < 5
            || quint8(result.frame.at(2)) != request.expectedDataBytes)) {
        handleReadFailure(QStringLiteral("充电桩读响应数据长度与请求不匹配。"));
        return;
    }
    if (request.isWriteCommand && result.frame != request.frame) {
        failOperation(QStringLiteral("充电桩写命令回显与请求不一致。"), true);
        return;
    }

    const qint64 elapsedMs = request.startedAt.msecsTo(QDateTime::currentDateTime());
    emit logMessage(QStringLiteral("[充电会话%1] RX %2，耗时%3ms")
                    .arg(m_sessionId)
                    .arg(QString::fromLatin1(result.frame.toHex(' ').toUpper()))
                    .arg(elapsedMs));
    m_inFlightRequest.reset();
    request.onSuccess(result.frame);
    if (!m_queryInProgress)
        return;
    if (!m_receiveBuffer.isEmpty()) {
        handleReadFailure(QStringLiteral("充电桩响应包含未关联的尾随数据。"));
        return;
    }

    m_lastSendTimer.start();
    if (m_flowMode == FlowMode::Charge
        && m_safeStopRequested && !m_safeShutdownStarted) {
        m_requestQueue.clear();
        m_onQueueDrained = {};
        beginSafeShutdown();
        return;
    }
    beginNextRequest();
}

void ChargePileController::handleSocketError(
    const QAbstractSocket::SocketError socketError)
{
    Q_UNUSED(socketError)
    if (!m_queryInProgress)
        return;
    if (m_recoveryAction != RecoveryAction::None
        && !m_inFlightRequest.has_value()) {
        m_responseTimer.stop();
        schedulePhase(DeferredPhase::RecoveryReconnect, 10);
        return;
    }
    const bool uncertainWrite =
        m_inFlightRequest.has_value() && m_inFlightRequest->isWriteCommand;
    if (uncertainWrite) {
        failOperation(QStringLiteral("充电桩写命令期间TCP通信错误：%1")
                      .arg(m_socket.errorString()), true);
    } else {
        handleReadFailure(QStringLiteral("充电桩TCP通信错误：%1")
                          .arg(m_socket.errorString()));
    }
}

void ChargePileController::handleResponseTimeout()
{
    if (!m_queryInProgress)
        return;
    const bool uncertainWrite =
        m_inFlightRequest.has_value() && m_inFlightRequest->isWriteCommand;
    if (uncertainWrite) {
        failOperation(QStringLiteral("充电桩写命令响应超时。"), true);
    } else if (m_recoveryAction != RecoveryAction::None
               && !m_inFlightRequest.has_value()) {
        schedulePhase(DeferredPhase::RecoveryReconnect, 10);
    } else {
        handleReadFailure(m_inFlightRequest.has_value()
                              ? QStringLiteral("充电桩读取响应超时。")
                              : QStringLiteral("连接充电桩超时。"));
    }
}

void ChargePileController::handleActionTimer()
{
    sendCurrentRequest();
}

void ChargePileController::handlePhaseTimer()
{
    const DeferredPhase phase = m_deferredPhase;
    m_deferredPhase = DeferredPhase::None;
    switch (phase) {
    case DeferredPhase::WaitForStartPoll:
        pollWaitingForStart();
        break;
    case DeferredPhase::MonitoringPoll:
        pollMonitoring();
        break;
    case DeferredPhase::WaitForNoOutputPoll:
        pollWaitingForNoOutput();
        break;
    case DeferredPhase::WaitForRetractedPoll:
        pollWaitingForRetracted();
        break;
    case DeferredPhase::RecoveryReconnect:
        startRecoveryConnection();
        break;
    case DeferredPhase::None:
        break;
    }
}

void ChargePileController::setState(const State state, const QString &text)
{
    if (m_state == state)
        return;
    m_state = state;
    const QString message =
        m_flowMode == FlowMode::Charge
            ? QStringLiteral("[充电会话%1][%2] %3")
                  .arg(m_sessionId).arg(originText(m_sessionOrigin), text)
            : text;
    emit stateChanged(m_state, message);
    emit logMessage(message);
}

void ChargePileController::failOperation(
    const QString &reason, const bool commandResultUnknown)
{
    if (!m_queryInProgress)
        return;

    const FlowMode failedFlow = m_flowMode;
    const SessionOrigin failedOrigin = m_sessionOrigin;
    const bool notifyApplication = m_applicationShutdownRequested;
    const QString finalReason =
        commandResultUnknown
            ? QStringLiteral("%1；写命令结果不确定，设备状态未知，需要人工确认。")
                  .arg(reason)
            : reason;
    if (commandResultUnknown && m_inFlightRequest.has_value()) {
        // 只有确实是写请求时才更新不确定身份。安全收尾的读重连耗尽也会以
        // Unknown 结束，但此时必须保留上一轮尚未解决的写身份，不能被读帧覆盖。
        const auto identity =
            chargePileWriteIdentity(QByteArrayView(m_inFlightRequest->frame));
        if (identity.has_value())
            m_uncertainWrite = identity;
    }
    clearTransportWork();
    m_socket.abort();
    m_queryInProgress = false;
    m_conservativeRecoveryActive = false;
    if (commandResultUnknown)
        m_unknownGate = true;
    // 在清除flowMode前变更状态，使失败日志仍携带会话编号与手动/自动来源。
    setState(commandResultUnknown ? State::Unknown : State::Fault, finalReason);
    m_flowMode = FlowMode::None;

    if (failedFlow == FlowMode::Query) {
        emit queryFinished(false, finalReason);
        return;
    }
    if (failedFlow == FlowMode::Charge && !m_sessionFinishedEmitted) {
        m_sessionFinishedEmitted = true;
        emit chargeSessionFinished(false, failedOrigin, finalReason);
    }
    const bool applicationShutdownInheritedByReentrantRecovery =
        m_queryInProgress
        && m_flowMode == FlowMode::Charge
        && m_conservativeRecoveryActive
        && m_applicationShutdownRequested
        && !m_applicationShutdownFinishedEmitted;
    if (notifyApplication
        && !applicationShutdownInheritedByReentrantRecovery) {
        emitApplicationShutdownFinishedOnce(false, finalReason);
    }
}

void ChargePileController::finishQuery()
{
    if (m_flowMode != FlowMode::Query)
        return;
    m_queryInProgress = false;
    if (snapshotConfirmsSafe(m_snapshot, m_settings)) {
        m_unknownGate = false;
        if (safeSnapshotResolvesRecoveryContext()) {
            // Start/Stop/Reset/参数写的效果都能由“明确缩到位且无输出”的完整
            // 快照充分覆盖；此时只读权威查询可以结束旧上下文。缩回线圈
            // ON/OFF 则不能仅凭位置证明电平已释放，助手会保留上下文。
            clearSafetyRecoveryContext();
        }
        setState(State::SafeComplete, QStringLiteral("充电桩状态确认安全。"));
    } else if (m_unknownGate) {
        setState(State::Unknown,
                 QStringLiteral("完整只读查询完成，但状态仍不安全，未知门禁保持。"));
    } else {
        setState(State::Idle, QStringLiteral("充电桩只读状态查询完成。"));
    }
    m_flowMode = FlowMode::None;
    emit queryFinished(true, QStringLiteral("充电桩只读状态查询完成。"));
}

void ChargePileController::finishChargeSession(
    const bool safe, const QString &message)
{
    if (m_flowMode != FlowMode::Charge || m_sessionFinishedEmitted)
        return;
    const SessionOrigin origin = m_sessionOrigin;
    const bool notifyApplication = m_applicationShutdownRequested;
    m_sessionFinishedEmitted = true;
    m_queryInProgress = false;
    m_flowMode = FlowMode::None;
    m_conservativeRecoveryActive = false;
    m_responseTimer.stop();
    m_actionPollTimer.stop();
    m_phaseTimer.stop();
    if (safe)
        clearSafetyRecoveryContext();
    emit chargeSessionFinished(safe, origin, message);
    // unsafe Automatic 终态可能同步触发继承关闭所有权的新恢复。此时旧阶段
    // 不能抢先发布 application=false；应由新恢复的最终权威结果发布一次。
    const bool applicationShutdownInheritedByReentrantRecovery =
        m_queryInProgress
        && m_flowMode == FlowMode::Charge
        && m_conservativeRecoveryActive
        && m_applicationShutdownRequested
        && !m_applicationShutdownFinishedEmitted;
    if (notifyApplication
        && !applicationShutdownInheritedByReentrantRecovery) {
        emitApplicationShutdownFinishedOnce(safe, message);
    }
}

void ChargePileController::clearSafetyRecoveryContext()
{
    m_startCommandConfirmed = false;
    m_stopCommandConfirmed = false;
    m_retractOnConfirmed = false;
    m_retractOffConfirmed = false;
    m_resetCommandConfirmed = false;
    m_uncertainWrite.reset();
    m_safetyRecoveryContextValid = false;
    m_conservativeRecoveryActive = false;
}

bool ChargePileController::safeSnapshotResolvesRecoveryContext() const
{
    if (!m_safetyRecoveryContextValid)
        return true;

    // 已确认缩回 ON 但没有 OFF 正常回显时，位置到位不能证明手动线圈已释放。
    if (m_retractOnConfirmed && !m_retractOffConfirmed)
        return false;
    if (!m_uncertainWrite.has_value())
        return true;

    const ChargePileWriteIdentity &uncertain = *m_uncertainWrite;
    if (uncertain.function == kWriteSingleRegister)
        return true;
    if (uncertain.function != kWriteSingleCoil)
        return false;
    if (uncertain.address == ChargePileProtocol::kCoilRetract)
        return false;

    return uncertain.address == ChargePileProtocol::kCoilStart
           || uncertain.address == ChargePileProtocol::kCoilStop
           || uncertain.address == ChargePileProtocol::kCoilReset;
}

void ChargePileController::emitApplicationShutdownFinishedOnce(
    const bool safe, const QString &message)
{
    if (!m_applicationShutdownRequested
        || m_applicationShutdownFinishedEmitted) {
        return;
    }
    m_applicationShutdownFinishedEmitted = true;
    emit applicationShutdownFinished(safe, message);
}

void ChargePileController::beginParameterWrites()
{
    setState(State::WritingParameters,
             QStringLiteral("步骤2/5：写入充电参数。"));
    m_expectedVoltageRaw = quint16(qRound(m_settings.voltageV * 10.0));
    m_expectedCurrentRaw = quint16(qRound(m_settings.currentA * 10.0));
    m_expectedCutoffRaw.reset();
    m_expectedMaxSecondsRaw.reset();
    if (m_settings.cutoffCurrentA.has_value()) {
        m_expectedCutoffRaw =
            quint16(qRound(*m_settings.cutoffCurrentA * 10.0));
    }
    if (m_settings.maxChargeSeconds.has_value())
        m_expectedMaxSecondsRaw = quint16(*m_settings.maxChargeSeconds);

    enqueueWriteRegister(ChargePileProtocol::kRegSetVoltage,
                         m_expectedVoltageRaw);
    enqueueWriteRegister(ChargePileProtocol::kRegSetCurrent,
                         m_expectedCurrentRaw);
    if (m_expectedCutoffRaw.has_value()) {
        enqueueWriteRegister(ChargePileProtocol::kRegSetCutoffCurrent,
                             *m_expectedCutoffRaw);
    }
    if (m_expectedMaxSecondsRaw.has_value()) {
        enqueueWriteRegister(ChargePileProtocol::kRegSetMaxSeconds,
                             *m_expectedMaxSecondsRaw);
    }
    m_onQueueDrained = [this] { beginParameterReadback(); };
}

void ChargePileController::beginParameterReadback()
{
    setState(State::ReadingBackParameters,
             QStringLiteral("回读并按协议原始整数核对充电参数。"));
    enqueueRead(kReadHoldingRegisters, ChargePileProtocol::kRegSetVoltage, 2,
                [this](const QByteArray &response) {
        const quint16 voltage = responseWord(response, 3);
        const quint16 current = responseWord(response, 5);
        if (voltage != m_expectedVoltageRaw || current != m_expectedCurrentRaw) {
            failOperation(
                QStringLiteral("电压/电流参数回读不一致：期望%1/%2，实际%3/%4。")
                    .arg(m_expectedVoltageRaw).arg(m_expectedCurrentRaw)
                    .arg(voltage).arg(current));
        }
    });
    if (m_expectedCutoffRaw.has_value()) {
        enqueueRead(kReadHoldingRegisters,
                    ChargePileProtocol::kRegSetCutoffCurrent, 1,
                    [this](const QByteArray &response) {
            const quint16 actual = responseWord(response, 3);
            if (actual != *m_expectedCutoffRaw) {
                failOperation(QStringLiteral("截止电流参数回读不一致。"));
            }
        });
    }
    if (m_expectedMaxSecondsRaw.has_value()) {
        enqueueRead(kReadHoldingRegisters,
                    ChargePileProtocol::kRegSetMaxSeconds, 1,
                    [this](const QByteArray &response) {
            const quint16 actual = responseWord(response, 3);
            if (actual != *m_expectedMaxSecondsRaw) {
                failOperation(QStringLiteral("最大充电时间参数回读不一致。"));
            }
        });
    }
    m_onQueueDrained = [this] { beginSecondPrecheck(); };
}

void ChargePileController::beginSecondPrecheck()
{
    enqueueSnapshotReads(
        State::Prechecking,
        QStringLiteral("参数回读后再次完整确认启动条件。"),
        [this] {
        QString reason;
        if (!validatePrestartSnapshot(&reason)) {
            failOperation(reason);
            return;
        }
        sendStartCommand();
    });
}

void ChargePileController::sendStartCommand()
{
    setState(State::SendingStart,
             QStringLiteral("步骤3/5：发送正常启动/允许充电命令。"));
    enqueueWriteCoil(ChargePileProtocol::kCoilStart, true, [this] {
        m_startCommandConfirmed = true;
        emit logMessage(QStringLiteral("[充电会话%1] 启动命令已获得唯一正常回显。")
                        .arg(m_sessionId));
    });
    m_onQueueDrained = [this] {
        setState(State::WaitingForStart,
                 QStringLiteral("等待伸到位、工作或输出电流上升。"));
        m_phaseElapsedTimer.restart();
        pollWaitingForStart();
    };
}

void ChargePileController::pollWaitingForStart()
{
    if (m_safeStopRequested) {
        beginSafeShutdown();
        return;
    }
    enqueueSnapshotReads(State::WaitingForStart,
                         QStringLiteral("轮询启动后的机械与输出状态。"),
                         [this] {
        if (snapshotHasBlockingFault(kPrestartIgnoredErrors)) {
            requestSafeStop(StopReason::Fault);
            return;
        }
        if (m_snapshot.extended || m_snapshot.working
            || m_snapshot.outputCurrentA > m_settings.chargeDetectCurrentA) {
            m_seenChargingOutput =
                m_snapshot.working || m_snapshot.relayOn
                || m_snapshot.outputCurrentA > m_settings.chargeDetectCurrentA;
            setState(State::Monitoring,
                     QStringLiteral("步骤4/5：进入充电监控。"));
            m_phaseElapsedTimer.restart();
            schedulePhase(DeferredPhase::MonitoringPoll,
                          m_settings.pollIntervalMs);
            return;
        }
        if (m_phaseElapsedTimer.elapsed() >= m_settings.startTimeoutMs) {
            requestSafeStop(StopReason::MonitorTimeout);
            return;
        }
        schedulePhase(DeferredPhase::WaitForStartPoll,
                      m_settings.pollIntervalMs);
    });
    beginNextRequest();
}

void ChargePileController::pollMonitoring()
{
    if (m_safeStopRequested) {
        beginSafeShutdown();
        return;
    }
    enqueueSnapshotReads(State::Monitoring,
                         QStringLiteral("轮询充电状态。"),
                         [this] {
        if (snapshotHasBlockingFault(kPrestartIgnoredErrors)) {
            requestSafeStop(StopReason::Fault);
            return;
        }
        if (m_snapshot.working || m_snapshot.relayOn
            || m_snapshot.outputCurrentA > m_settings.chargeDetectCurrentA) {
            m_seenChargingOutput = true;
        }
        if (m_snapshot.eventWord >= 41 && m_snapshot.eventWord <= 47) {
            requestSafeStop(StopReason::AutomaticTargetReached);
            return;
        }
        if (m_seenChargingOutput && snapshotHasNoOutput()) {
            requestSafeStop(StopReason::AutomaticTargetReached);
            return;
        }
        if (m_settings.monitorTimeoutMs > 0
            && m_phaseElapsedTimer.elapsed() >= m_settings.monitorTimeoutMs) {
            requestSafeStop(StopReason::MonitorTimeout);
            return;
        }
        schedulePhase(DeferredPhase::MonitoringPoll,
                      m_settings.pollIntervalMs);
    });
    beginNextRequest();
}

void ChargePileController::beginSafeShutdown()
{
    if (!m_queryInProgress || m_flowMode != FlowMode::Charge
        || m_safeShutdownStarted || m_inFlightRequest.has_value()) {
        return;
    }
    m_safeShutdownStarted = true;
    m_safeStopRequested = true;
    m_phaseTimer.stop();
    m_deferredPhase = DeferredPhase::None;
    m_requestQueue.clear();
    m_pendingRequest.reset();
    m_onQueueDrained = {};

    if (m_stopCommandConfirmed) {
        setState(State::WaitingForNoOutput,
                 QStringLiteral("停止命令已确认，继续等待无输出，不重复发送停止。"));
        m_phaseElapsedTimer.restart();
        pollWaitingForNoOutput();
        return;
    }

    setState(State::SendingStop,
             QStringLiteral("步骤5/5：发送停止命令，开始统一安全收尾。"));
    enqueueWriteCoil(ChargePileProtocol::kCoilStop, true, [this] {
        m_stopCommandConfirmed = true;
        // 已确认的 Stop 是未知 Start 的安全反向命令；后续恢复应从“等待无输出”
        // 继续，不能再保留旧 Start 身份而重复发送 Stop。
        if (m_uncertainWrite.has_value()
            && m_uncertainWrite->function == kWriteSingleCoil
            && m_uncertainWrite->address == ChargePileProtocol::kCoilStart) {
            m_uncertainWrite.reset();
        }
    });
    m_onQueueDrained = [this] {
        setState(State::WaitingForNoOutput,
                 QStringLiteral("等待工作清零、继电器断开且电流降到安全值。"));
        m_phaseElapsedTimer.restart();
        pollWaitingForNoOutput();
    };
    beginNextRequest();
}

void ChargePileController::pollWaitingForNoOutput()
{
    enqueueSnapshotReads(State::WaitingForNoOutput,
                         QStringLiteral("轮询停止后的输出状态。"),
                         [this] {
        if (snapshotHasBlockingFault(kPrestartIgnoredErrors)) {
            failOperation(QStringLiteral("等待停止输出时出现非豁免故障。"));
            return;
        }
        if (snapshotHasNoOutput()) {
            sendRetractCommand();
            return;
        }
        if (m_phaseElapsedTimer.elapsed() >= m_settings.stopTimeoutMs) {
            failOperation(QStringLiteral("等待停止输出超时，禁止发送缩回。"));
            return;
        }
        schedulePhase(DeferredPhase::WaitForNoOutputPoll,
                      m_settings.pollIntervalMs);
    });
    beginNextRequest();
}

void ChargePileController::sendRetractCommand()
{
    if (m_retractOnConfirmed) {
        setState(State::WaitingForRetracted,
                 QStringLiteral("缩回ON已确认，继续等待到位，不重复发送。"));
        m_phaseElapsedTimer.restart();
        pollWaitingForRetracted();
        return;
    }
    setState(State::Retracting, QStringLiteral("发送手动缩回线圈 ON。"));
    enqueueWriteCoil(ChargePileProtocol::kCoilRetract, true, [this] {
        m_retractOnConfirmed = true;
    });
    m_onQueueDrained = [this] {
        setState(State::WaitingForRetracted,
                 QStringLiteral("等待机构明确缩到位。"));
        m_phaseElapsedTimer.restart();
        pollWaitingForRetracted();
    };
    beginNextRequest();
}

void ChargePileController::pollWaitingForRetracted()
{
    enqueueSnapshotReads(State::WaitingForRetracted,
                         QStringLiteral("轮询缩回到位状态。"),
                         [this] {
        if (snapshotHasBlockingFault(kRetractIgnoredErrors)) {
            releaseRetractAfterFailure(QStringLiteral("缩回过程中出现非 E8 故障。"));
            return;
        }
        if (m_snapshot.retracted && !m_snapshot.extended) {
            // Python 的 finally 会在到位或异常后释放手动缩回线圈；成功路径必须
            // 先写 OFF，再复位，避免把可能为电平保持的线圈长期置位。
            enqueueWriteCoil(ChargePileProtocol::kCoilRetract, false, [this] {
                m_retractOffConfirmed = true;
                // OFF 的正常回显同时解决此前“不确定的缩回 ON”；安全上下文
                // 由此推进到“线圈已释放”，下一轮不得再次发送 ON/OFF。
                if (m_uncertainWrite.has_value()
                    && m_uncertainWrite->function == kWriteSingleCoil
                    && m_uncertainWrite->address
                           == ChargePileProtocol::kCoilRetract
                    && m_uncertainWrite->value == 0xFF00) {
                    m_uncertainWrite.reset();
                }
            });
            m_onQueueDrained = [this] { sendResetAndFinalCheck(); };
            beginNextRequest();
            return;
        }
        if (m_phaseElapsedTimer.elapsed() >= m_settings.motionTimeoutMs) {
            releaseRetractAfterFailure(QStringLiteral("等待缩到位超时。"));
            return;
        }
        schedulePhase(DeferredPhase::WaitForRetractedPoll,
                      m_settings.pollIntervalMs);
    });
    beginNextRequest();
}

void ChargePileController::sendResetAndFinalCheck()
{
    if (m_resetCommandConfirmed) {
        beginFinalSafetyQuery();
        return;
    }
    setState(State::Resetting, QStringLiteral("发送复位命令。"));
    enqueueWriteCoil(ChargePileProtocol::kCoilReset, true, [this] {
        m_resetCommandConfirmed = true;
        // Reset 已确认代表此前停止、缩回和释放里程碑均已越过；旧的不确定
        // 身份不再控制重发矩阵，后续仅允许最终完整只读确认。
        m_uncertainWrite.reset();
    });
    m_onQueueDrained = [this] {
        beginFinalSafetyQuery();
    };
    beginNextRequest();
}

void ChargePileController::beginFinalSafetyQuery()
{
    enqueueSnapshotReads(State::Resetting,
                         QStringLiteral("执行复位后的最终完整安全查询。"),
                         [this] {
        if (!snapshotConfirmsSafe(m_snapshot, m_settings)) {
            setState(State::Fault,
                     QStringLiteral("最终查询未确认无输出且明确缩到位。"));
            finishChargeSession(
                false, QStringLiteral("安全收尾最终确认失败，需要人工检查。"));
            return;
        }
        // Unknown 只能由完整快照明确安全来解除。保守关闭的终检与手动查询使用
        // 同一套 snapshotConfirmsSafe 判据，因此成功分支必须同步放行后续充电，
        // 不能只把界面状态改成 SafeComplete 而遗留内部未知门禁。
        m_unknownGate = false;
        setState(State::SafeComplete,
                 QStringLiteral("工作清零、继电器断开、电流安全且机构明确缩到位。"));
        finishChargeSession(
            true, QStringLiteral("充电会话已完成统一安全收尾。"));
    });
    beginNextRequest();
}

void ChargePileController::beginConservativeRecoveryAfterSnapshot()
{
    m_safeStopRequested = true;
    // 保守矩阵已经接管安全推进。标记“收尾入口已建立”，防止最后一帧预检
    // 的 handleReadyRead() 在本回调返回后再次调用普通 beginSafeShutdown()，
    // 从而覆盖 Unknown Retract/Reset 分支并重复较晚阶段写命令。
    m_safeShutdownStarted = true;

    const auto uncertainCoilIs = [this](const quint16 address) {
        return m_uncertainWrite.has_value()
               && m_uncertainWrite->function == kWriteSingleCoil
               && m_uncertainWrite->address == address;
    };
    const bool hasLaterMechanicalUncertainty =
        uncertainCoilIs(ChargePileProtocol::kCoilRetract)
        || uncertainCoilIs(ChargePileProtocol::kCoilReset);

    // 每次新恢复先从持久化的最远安全里程碑继续。该顺序高于旧的不确定
    // 写身份的前提是里程碑阶段不早于该身份。比如 Stop 已确认但缩回 ON
    // 结果未知时，Unknown ON 属于更晚阶段，绝不能被较早的 Stop 覆盖。
    if (m_resetCommandConfirmed) {
        m_uncertainWrite.reset();
        beginFinalSafetyQuery();
        return;
    }
    if (m_retractOffConfirmed
        && !uncertainCoilIs(ChargePileProtocol::kCoilReset)) {
        m_uncertainWrite.reset();
        sendResetAndFinalCheck();
        return;
    }
    if (m_retractOnConfirmed
        && !hasLaterMechanicalUncertainty) {
        if (m_uncertainWrite.has_value()
            && m_uncertainWrite->function == kWriteSingleCoil
            && m_uncertainWrite->address == ChargePileProtocol::kCoilRetract
            && m_uncertainWrite->value == 0x0000) {
            finishConservativeRecoveryUnsafe(
                QStringLiteral("缩回 OFF 结果不确定，位置到位不能证明线圈已经释放。"));
            return;
        }
        if (m_uncertainWrite.has_value()
            && m_uncertainWrite->function == kWriteSingleCoil
            && (m_uncertainWrite->address == ChargePileProtocol::kCoilStart
                || m_uncertainWrite->address == ChargePileProtocol::kCoilStop)) {
            m_uncertainWrite.reset();
        }
        setState(State::WaitingForRetracted,
                 QStringLiteral("缩回ON已确认，按最新快照继续等待到位，不重复发送。"));
        m_phaseElapsedTimer.restart();
        pollWaitingForRetracted();
        return;
    }
    if (m_stopCommandConfirmed
        && !hasLaterMechanicalUncertainty) {
        if (m_uncertainWrite.has_value()
            && m_uncertainWrite->function == kWriteSingleCoil
            && m_uncertainWrite->address == ChargePileProtocol::kCoilStart) {
            m_uncertainWrite.reset();
        }
        setState(State::WaitingForNoOutput,
                 QStringLiteral("停止已确认，按最新快照继续无输出阶段，不重复发送。"));
        m_phaseElapsedTimer.restart();
        if (snapshotHasNoOutput())
            sendRetractCommand();
        else
            pollWaitingForNoOutput();
        return;
    }

    if (!m_uncertainWrite.has_value()) {
        m_safeShutdownStarted = false;
        beginSafeShutdown();
        return;
    }

    const ChargePileWriteIdentity uncertain = *m_uncertainWrite;
    if (uncertain.function == 0x06) {
        // 参数寄存器写结果不确定时，保守恢复不再写任何参数，只执行停止链。
        m_safeShutdownStarted = false;
        beginSafeShutdown();
        return;
    }
    if (uncertain.function != 0x05) {
        finishConservativeRecoveryUnsafe(
            QStringLiteral("未知写命令类型无法安全自动恢复。"));
        return;
    }

    if (uncertain.address == ChargePileProtocol::kCoilStart) {
        // Start 结果不确定时只能发送语义相反的 Stop，绝不能再次发送 Start。
        m_safeShutdownStarted = false;
        beginSafeShutdown();
        return;
    }
    if (uncertain.address == ChargePileProtocol::kCoilStop) {
        if (!snapshotHasNoOutput()) {
            finishConservativeRecoveryUnsafe(
                QStringLiteral("停止命令结果不确定且仍检测到输出，禁止盲目重发停止。"));
            return;
        }
        m_stopCommandConfirmed = true;
        m_uncertainWrite.reset();
        setState(State::WaitingForNoOutput,
                 QStringLiteral("停止结果不确定，但完整查询已确认无输出，继续缩回。"));
        sendRetractCommand();
        return;
    }
    if (uncertain.address == ChargePileProtocol::kCoilRetract) {
        if (uncertain.value == 0xFF00
            && m_snapshot.retracted && !m_snapshot.extended) {
            // ON 不确定但机构已明确缩到位时，只允许发送相反的 OFF，不重复 ON。
            m_retractOnConfirmed = true;
            enqueueWriteCoil(ChargePileProtocol::kCoilRetract, false, [this] {
                m_retractOffConfirmed = true;
                if (m_uncertainWrite.has_value()
                    && m_uncertainWrite->function == kWriteSingleCoil
                    && m_uncertainWrite->address
                           == ChargePileProtocol::kCoilRetract
                    && m_uncertainWrite->value == 0xFF00) {
                    m_uncertainWrite.reset();
                }
            });
            m_onQueueDrained = [this] { sendResetAndFinalCheck(); };
            beginNextRequest();
            return;
        }
        finishConservativeRecoveryUnsafe(
            uncertain.value == 0x0000
                ? QStringLiteral("缩回 OFF 结果不确定，位置到位不能证明线圈已经释放。")
                : QStringLiteral("缩回 ON 结果不确定且未确认缩到位，禁止重复写入。"));
        return;
    }
    if (uncertain.address == ChargePileProtocol::kCoilReset) {
        if (snapshotConfirmsSafe(m_snapshot, m_settings)) {
            m_unknownGate = false;
            setState(State::SafeComplete,
                     QStringLiteral("复位结果不确定，但最终完整快照已明确安全。"));
            finishChargeSession(true, QStringLiteral("保守恢复已由完整快照确认安全。"));
        } else {
            finishConservativeRecoveryUnsafe(
                QStringLiteral("复位结果不确定且完整快照未确认安全，禁止重复复位。"));
        }
        return;
    }

    finishConservativeRecoveryUnsafe(
        QStringLiteral("结果不确定的线圈命令不属于可自动恢复范围。"));
}

void ChargePileController::finishConservativeRecoveryUnsafe(
    const QString &reason)
{
    m_unknownGate = true;
    setState(State::Unknown, reason);
    finishChargeSession(false, reason);
}

void ChargePileController::handleReadFailure(const QString &reason)
{
    if (m_flowMode != FlowMode::Charge || !m_queryInProgress) {
        failOperation(reason);
        return;
    }

    if (m_retractOnConfirmed && !m_retractOffConfirmed) {
        beginCommunicationRecovery(RecoveryAction::ReleaseRetractAndFail, reason);
    } else if (m_resetCommandConfirmed) {
        beginCommunicationRecovery(RecoveryAction::RetryFinalSafetyQuery, reason);
    } else if (m_stopCommandConfirmed) {
        beginCommunicationRecovery(RecoveryAction::ResumeWaitingForNoOutput, reason);
    } else if (m_startCommandConfirmed) {
        beginCommunicationRecovery(RecoveryAction::BeginSafeStop, reason);
    } else {
        failOperation(reason);
    }
}

void ChargePileController::beginCommunicationRecovery(
    const RecoveryAction action, const QString &reason)
{
    m_recoveryAction = action;
    m_recoveryReason = reason;
    clearTransportWork();
    m_socket.abort();
    emit logMessage(QStringLiteral("[充电会话%1][%2] 只读通信失败：%3；从已确认安全里程碑有限重连。")
                    .arg(m_sessionId).arg(originText(m_sessionOrigin), reason));
    startRecoveryConnection();
}

void ChargePileController::startRecoveryConnection()
{
    if (!m_queryInProgress || m_recoveryAction == RecoveryAction::None)
        return;
    if (m_recoveryAttempts >= kMaximumRecoveryConnectAttempts) {
        const QString reason =
            QStringLiteral("%1；有限重连失败，设备状态未知，需要人工确认。")
                .arg(m_recoveryReason);
        m_recoveryAction = RecoveryAction::None;
        failOperation(reason, true);
        return;
    }

    ++m_recoveryAttempts;
    m_responseTimer.stop();
    m_socket.abort();
    setState(State::Connecting,
             QStringLiteral("正在重连充电桩以继续安全收尾。"));
    emit logMessage(QStringLiteral("[充电会话%1] 安全恢复连接尝试 %2/%3。")
                    .arg(m_sessionId).arg(m_recoveryAttempts)
                    .arg(kMaximumRecoveryConnectAttempts));
    m_responseTimer.start(m_settings.connectTimeoutMs);
    m_socket.connectToHost(m_settings.host.trimmed(), m_settings.port);
}

void ChargePileController::resumeAfterRecovery()
{
    const RecoveryAction action = m_recoveryAction;
    m_recoveryAction = RecoveryAction::None;
    switch (action) {
    case RecoveryAction::BeginSafeStop:
        m_safeShutdownStarted = false;
        requestSafeStop(StopReason::Fault);
        break;
    case RecoveryAction::ResumeWaitingForNoOutput:
        setState(State::WaitingForNoOutput,
                 QStringLiteral("重连成功，停止已确认，仅恢复无输出查询。"));
        m_phaseElapsedTimer.restart();
        pollWaitingForNoOutput();
        break;
    case RecoveryAction::ReleaseRetractAndFail:
        releaseRetractAfterFailure(m_recoveryReason);
        break;
    case RecoveryAction::RetryFinalSafetyQuery:
        beginFinalSafetyQuery();
        break;
    case RecoveryAction::None:
        break;
    }
}

void ChargePileController::releaseRetractAfterFailure(const QString &reason)
{
    m_phaseTimer.stop();
    m_requestQueue.clear();
    m_pendingRequest.reset();
    m_onQueueDrained = {};
    m_terminalFailureAfterRetractOff = reason;

    if (m_socket.state() != QAbstractSocket::ConnectedState) {
        beginCommunicationRecovery(RecoveryAction::ReleaseRetractAndFail, reason);
        return;
    }
    if (m_retractOffConfirmed) {
        const QString terminalReason = m_terminalFailureAfterRetractOff;
        m_terminalFailureAfterRetractOff.clear();
        failOperation(terminalReason);
        return;
    }

    setState(State::Retracting,
             QStringLiteral("缩回阶段异常，先释放缩回线圈OFF，禁止复位。"));
    enqueueWriteCoil(ChargePileProtocol::kCoilRetract, false, [this] {
        m_retractOffConfirmed = true;
        if (m_uncertainWrite.has_value()
            && m_uncertainWrite->function == kWriteSingleCoil
            && m_uncertainWrite->address == ChargePileProtocol::kCoilRetract
            && m_uncertainWrite->value == 0xFF00) {
            m_uncertainWrite.reset();
        }
    });
    m_onQueueDrained = [this] {
        const QString terminalReason = m_terminalFailureAfterRetractOff;
        m_terminalFailureAfterRetractOff.clear();
        failOperation(terminalReason);
    };
    beginNextRequest();
}

void ChargePileController::schedulePhase(
    const DeferredPhase phase, const int delayMs)
{
    if (!m_queryInProgress || m_flowMode != FlowMode::Charge)
        return;
    m_deferredPhase = phase;
    m_phaseTimer.start(qMax(1, delayMs));
}

bool ChargePileController::snapshotHasBlockingFault(
    const quint16 ignoredMask) const
{
    const quint16 blocking = m_snapshot.faultWord & ~ignoredMask;
    const bool ignoredErrorOnly =
        m_snapshot.faultWord != 0 && blocking == 0;
    const bool outputFault = ChargePileProtocol::outputSignalBit(
        m_snapshot.outputWord, ChargePileProtocol::kOutputFaultBit);
    return blocking != 0 || (outputFault && !ignoredErrorOnly);
}

bool ChargePileController::snapshotHasNoOutput() const
{
    return !m_snapshot.working
           && !m_snapshot.relayOn
           && m_snapshot.outputCurrentA <= m_settings.safeCurrentA;
}

bool ChargePileController::validatePrestartSnapshot(QString *reason) const
{
    if (snapshotHasBlockingFault(kPrestartIgnoredErrors)) {
        if (reason)
            *reason = QStringLiteral("存在非厂家豁免故障，禁止启动。");
        return false;
    }
    if (m_snapshot.extended && m_snapshot.retracted) {
        if (reason)
            *reason = QStringLiteral("伸到位和缩到位同时为1，禁止启动。");
        return false;
    }
    if (m_snapshot.working || m_snapshot.relayOn) {
        if (reason)
            *reason = QStringLiteral("设备已工作或继电器已吸合，禁止重复启动。");
        return false;
    }
    return true;
}

void ChargePileController::publishSnapshot()
{
    // 只有一整轮五组状态均成功后才清除连续恢复计数；仅重连成功但首帧再次
    // 失败不会重置预算，从而避免设备“能连但不能读”时无限重连。
    m_recoveryAttempts = 0;
    m_snapshot.sampledAt = QDateTime::currentDateTime();
    emit snapshotChanged(m_snapshot);
    emit logMessage(
        QStringLiteral("[充电会话%1] 状态 U=%2V I=%3A 输入=0x%4 输出=0x%5 事件=%6 故障=0x%7")
            .arg(m_sessionId)
            .arg(m_snapshot.outputVoltageV, 0, 'f', 1)
            .arg(m_snapshot.outputCurrentA, 0, 'f', 1)
            .arg(m_snapshot.inputWord, 4, 16, QLatin1Char('0'))
            .arg(m_snapshot.outputWord, 4, 16, QLatin1Char('0'))
            .arg(m_snapshot.eventWord)
            .arg(m_snapshot.faultWord, 4, 16, QLatin1Char('0')));
}

void ChargePileController::clearTransportWork()
{
    m_responseTimer.stop();
    m_actionPollTimer.stop();
    m_phaseTimer.stop();
    m_receiveBuffer.clear();
    m_requestQueue.clear();
    m_pendingRequest.reset();
    m_inFlightRequest.reset();
    m_onQueueDrained = {};
    m_deferredPhase = DeferredPhase::None;
}

int ChargePileController::stopReasonPriority(const StopReason reason)
{
    switch (reason) {
    case StopReason::Manual: return 10;
    case StopReason::AutomaticTaskReady: return 20;
    case StopReason::AutomaticTargetReached: return 30;
    case StopReason::AutomaticDisabled: return 40;
    case StopReason::LineStop: return 60;
    case StopReason::MonitorTimeout: return 70;
    case StopReason::Fault: return 80;
    case StopReason::ApplicationShutdown: return 100;
    }
    return 0;
}

QString ChargePileController::stopReasonText(const StopReason reason)
{
    switch (reason) {
    case StopReason::Manual: return QStringLiteral("人工停止");
    case StopReason::AutomaticTaskReady: return QStringLiteral("已有任务且达到允许接单电量");
    case StopReason::AutomaticTargetReached: return QStringLiteral("达到正常停止条件");
    case StopReason::AutomaticDisabled: return QStringLiteral("自动充电已关闭");
    case StopReason::LineStop: return QStringLiteral("主调度停止");
    case StopReason::ApplicationShutdown: return QStringLiteral("程序关闭");
    case StopReason::Fault: return QStringLiteral("充电桩故障");
    case StopReason::MonitorTimeout: return QStringLiteral("上位机监控超时");
    }
    return QStringLiteral("未知原因");
}

QString ChargePileController::originText(const SessionOrigin origin)
{
    return origin == SessionOrigin::Manual
               ? QStringLiteral("手动")
               : QStringLiteral("自动");
}

void ChargePileController::invalidateCommunicationContext(const QString &reason)
{
    const FlowMode oldFlow = m_flowMode;
    const SessionOrigin oldOrigin = m_sessionOrigin;
    const bool notifyApplication = m_applicationShutdownRequested;
    const bool wasInProgress = m_queryInProgress;
    clearTransportWork();
    m_socket.abort();
    m_lastSendTimer.invalidate();
    m_snapshot = ChargePileSnapshot{};
    m_queryInProgress = false;
    m_conservativeRecoveryActive = false;
    m_unknownGate = true;
    setState(State::Unknown, reason);
    m_flowMode = FlowMode::None;
    if (!wasInProgress)
        return;
    if (oldFlow == FlowMode::Query)
        emit queryFinished(false, reason);
    else if (oldFlow == FlowMode::Charge)
        emit chargeSessionFinished(false, oldOrigin, reason);
    if (notifyApplication)
        emitApplicationShutdownFinishedOnce(false, reason);
}

bool ChargePileController::communicationTargetChanged(
    const ChargeSettings &previous, const ChargeSettings &next)
{
    return previous.host.trimmed() != next.host.trimmed()
           || previous.port != next.port
           || previous.slaveId != next.slaveId;
}

quint16 ChargePileController::responseWord(
    const QByteArray &frame, const int offset)
{
    return (quint16(quint8(frame.at(offset))) << 8)
           | quint8(frame.at(offset + 1));
}

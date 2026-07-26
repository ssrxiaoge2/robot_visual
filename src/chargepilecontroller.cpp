#include "chargepilecontroller.h"

#include "chargepileprotocol.h"

#include <QMetaType>

namespace {

constexpr quint8 kReadInputRegisters = 0x04;
// QTcpSocket::write() 返回时数据可能仍在 Qt 的发送缓冲；比现场规定多保留 20ms
// 余量，确保以对端实际接收时刻度量时，相邻 RTU 请求也不会短于 50ms。
constexpr int kMinimumRequestIntervalMs = 70;

bool snapshotConfirmsSafe(const ChargePileSnapshot &snapshot, const ChargeSettings &settings)
{
    // “无输出”同时要求工作位、继电器位关闭，且电流不超过当前设置的安全阈值。
    return snapshot.retracted
           && !snapshot.extended
           && !snapshot.working
           && !snapshot.relayOn
           && snapshot.outputCurrentA <= settings.safeCurrentA;
}

} // namespace

ChargePileController::ChargePileController(QObject *parent)
    : QObject(parent)
{
    // 显式注册使跨线程连接和 QSignalSpy 都能可靠读取自定义快照与状态枚举。
    qRegisterMetaType<ChargePileSnapshot>("ChargePileSnapshot");
    qRegisterMetaType<ChargePileController::State>("ChargePileController::State");

    m_responseTimer.setSingleShot(true);
    m_actionPollTimer.setSingleShot(true);

    connect(&m_socket, &QTcpSocket::connected, this, &ChargePileController::handleConnected);
    connect(&m_socket, &QTcpSocket::readyRead, this, &ChargePileController::handleReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred,
            this, &ChargePileController::handleSocketError);
    connect(&m_responseTimer, &QTimer::timeout,
            this, &ChargePileController::handleResponseTimeout);
    connect(&m_actionPollTimer, &QTimer::timeout,
            this, &ChargePileController::handleActionTimer);
}

void ChargePileController::applySettings(const ChargeSettings &settings)
{
    const ChargeSettingsValidation validation = validateChargeSettings(settings);
    if (!validation.ok) {
        emit logMessage(QStringLiteral("充电桩设置校验失败：%1")
                        .arg(validation.errors.join(QStringLiteral("；"))));
        return;
    }

    const bool targetChanged = communicationTargetChanged(m_settings, settings);
    const bool hasCommunicationContext = m_queryInProgress
                                         || m_socket.state() != QAbstractSocket::UnconnectedState
                                         || m_snapshot.sampledAt.isValid()
                                         || m_pendingRequest.has_value()
                                         || m_inFlightRequest.has_value();
    m_settings = settings;

    // 电压、超时等同目标参数可直接热更新；仅 host/port/slave 变化才失效会话。
    if (targetChanged && hasCommunicationContext) {
        invalidateCommunicationContext(QStringLiteral("通信目标已变更，旧连接和状态快照已失效。"));
    }
}

void ChargePileController::queryStatus()
{
    if (m_queryInProgress) {
        const QString reason = QStringLiteral("状态查询正在进行，拒绝并发查询请求。");
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
    m_receiveBuffer.clear();
    m_requestQueue.clear();
    m_pendingRequest.reset();
    m_inFlightRequest.reset();

    // 五组状态严格按照现场 Python 脚本的读取顺序排队，且全部为 0x04。
    enqueueRead(ChargePileProtocol::kRegOutVoltage, 2, [this](const QByteArray &response) {
        m_snapshot.outputVoltageV = responseWord(response, 3) / 10.0;
        m_snapshot.outputCurrentA = responseWord(response, 5) / 10.0;
    });
    enqueueRead(ChargePileProtocol::kRegInputSignals, 1, [this](const QByteArray &response) {
        m_snapshot.inputWord = responseWord(response, 3);
        m_snapshot.extended = ChargePileProtocol::inputSignalBit(
            m_snapshot.inputWord, ChargePileProtocol::kInputExtendedBit);
        m_snapshot.retracted = ChargePileProtocol::inputSignalBit(
            m_snapshot.inputWord, ChargePileProtocol::kInputRetractedBit);
    });
    enqueueRead(ChargePileProtocol::kRegOutputSignals, 1, [this](const QByteArray &response) {
        m_snapshot.outputWord = responseWord(response, 3);
        m_snapshot.working = ChargePileProtocol::outputSignalBit(
            m_snapshot.outputWord, ChargePileProtocol::kOutputWorkingBit);
        m_snapshot.relayOn = ChargePileProtocol::outputSignalBit(
            m_snapshot.outputWord, ChargePileProtocol::kOutputRelayBit);
    });
    enqueueRead(ChargePileProtocol::kRegEvent, 1, [this](const QByteArray &response) {
        m_snapshot.eventWord = responseWord(response, 3);
    });
    enqueueRead(ChargePileProtocol::kRegError, 1, [this](const QByteArray &response) {
        m_snapshot.faultWord = responseWord(response, 3);
    });

    if (m_socket.state() == QAbstractSocket::ConnectedState) {
        handleConnected();
        return;
    }

    setState(State::Connecting, QStringLiteral("正在连接充电桩。"));
    m_responseTimer.start(m_settings.connectTimeoutMs);
    m_socket.connectToHost(m_settings.host.trimmed(), m_settings.port);
}

bool ChargePileController::isBusy() const
{
    return m_queryInProgress;
}

bool ChargePileController::shutdownRequired() const
{
    // SafeComplete 是唯一允许宣告安全的状态；其余状态一律采用保守判断。
    return m_state != State::SafeComplete || !snapshotConfirmsSafe(m_snapshot, m_settings);
}

ChargePileSnapshot ChargePileController::snapshot() const
{
    return m_snapshot;
}

void ChargePileController::enqueueRead(
    const quint16 address, const quint16 count,
    std::function<void(const QByteArray &response)> onSuccess)
{
    Request request;
    request.expectedSlaveId = m_settings.slaveId;
    request.expectedFunction = kReadInputRegisters;
    request.frame = ChargePileProtocol::buildReadRequest(
        m_settings.slaveId, kReadInputRegisters, address, count);
    request.expectedDataBytes = count * 2;
    request.isWriteCommand = false;
    request.onSuccess = std::move(onSuccess);
    m_requestQueue.enqueue(std::move(request));
}

void ChargePileController::beginNextRequest()
{
    if (!m_queryInProgress)
        return;
    if (m_requestQueue.isEmpty()) {
        finishQuery();
        return;
    }

    // 取出的请求先处于待发送阶段；只有 write() 成功后才能变为可接收响应的在途请求。
    m_pendingRequest = m_requestQueue.dequeue();
    scheduleCurrentRequest();
}

void ChargePileController::scheduleCurrentRequest()
{
    if (!m_pendingRequest.has_value())
        return;

    if (!m_lastSendTimer.isValid() || m_lastSendTimer.elapsed() >= kMinimumRequestIntervalMs) {
        sendCurrentRequest();
        return;
    }

    const int remaining = kMinimumRequestIntervalMs - int(m_lastSendTimer.elapsed());
    m_actionPollTimer.start(remaining);
}

void ChargePileController::sendCurrentRequest()
{
    if (!m_queryInProgress || !m_pendingRequest.has_value())
        return;

    Request request = std::move(*m_pendingRequest);
    m_pendingRequest.reset();
    // 任务三的硬性保险：即使未来误把写请求排入队列，本阶段也不允许送往网络。
    if (request.isWriteCommand || request.expectedFunction != kReadInputRegisters) {
        failQuery(QStringLiteral("只读查询控制器拒绝发送写命令。"));
        return;
    }
    if (m_socket.state() != QAbstractSocket::ConnectedState) {
        failQuery(QStringLiteral("充电桩连接已断开，无法发送只读查询。"));
        return;
    }

    if (m_socket.write(request.frame) != request.frame.size()) {
        failQuery(QStringLiteral("充电桩只读查询写入套接字失败。"));
        return;
    }
    // write() 成功后才允许接收路径将数据归属给该请求，消除节流窗口的旧帧歧义。
    request.startedAt = QDateTime::currentDateTime();
    m_lastSendTimer.start();
    m_inFlightRequest = std::move(request);
    m_responseTimer.start(m_settings.responseTimeoutMs);
}

void ChargePileController::handleConnected()
{
    if (!m_queryInProgress)
        return;

    m_responseTimer.stop();
    setState(State::Prechecking, QStringLiteral("正在顺序读取充电桩状态。"));
    beginNextRequest();
}

void ChargePileController::handleReadyRead()
{
    m_receiveBuffer.append(m_socket.readAll());
    if (!m_queryInProgress) {
        emit logMessage(QStringLiteral("收到已结束查询的充电桩响应。"));
        return;
    }
    if (!m_inFlightRequest.has_value()) {
        // 该分支包括节流等待期；延迟或重复旧帧绝不能被待发送请求错误接收。
        failQuery(QStringLiteral("收到不属于已写入请求的充电桩响应，已保守中止查询。"));
        return;
    }

    const Request request = *m_inFlightRequest;
    const ChargePileProtocol::FrameExtractResult result =
        ChargePileProtocol::takeResponseFrame(
            &m_receiveBuffer, request.expectedSlaveId, request.expectedFunction);
    if (result.status == ChargePileProtocol::FrameExtractStatus::Incomplete)
        return;
    if (result.status == ChargePileProtocol::FrameExtractStatus::Invalid) {
        failQuery(QStringLiteral("充电桩响应无效：%1").arg(result.reason));
        return;
    }

    m_responseTimer.stop();
    if (result.exceptionCode != 0) {
        failQuery(QStringLiteral("充电桩拒绝只读查询：%1").arg(result.reason));
        return;
    }
    if (result.frame.size() < 5 || quint8(result.frame.at(2)) != request.expectedDataBytes) {
        failQuery(QStringLiteral("充电桩响应数据长度与请求不匹配。"));
        return;
    }

    m_inFlightRequest.reset();
    request.onSuccess(result.frame);
    // 同一次 readyRead 可能已把重复帧一并追加到缓存。下一请求尚未 write()，
    // 不能等待未来网络通知再处理该尾随数据，否则会在下一响应到达时错配。
    if (!m_receiveBuffer.isEmpty()) {
        failQuery(QStringLiteral("充电桩响应包含未关联的尾随数据，已保守中止查询。"));
        return;
    }
    beginNextRequest();
}

void ChargePileController::handleSocketError(const QAbstractSocket::SocketError socketError)
{
    Q_UNUSED(socketError)
    if (m_queryInProgress) {
        failQuery(QStringLiteral("充电桩TCP通信错误：%1").arg(m_socket.errorString()));
    }
}

void ChargePileController::handleResponseTimeout()
{
    if (!m_queryInProgress)
        return;

    if (m_inFlightRequest.has_value()) {
        failQuery(QStringLiteral("充电桩只读查询响应超时。"));
    } else {
        failQuery(QStringLiteral("连接充电桩超时。"));
    }
}

void ChargePileController::handleActionTimer()
{
    // 定时器只在前一请求已完成且下一请求待发时触发，天然不可能产生并发发送。
    sendCurrentRequest();
}

void ChargePileController::setState(const State state, const QString &text)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(m_state, text);
    emit logMessage(text);
}

void ChargePileController::failQuery(const QString &reason)
{
    if (!m_queryInProgress)
        return;

    m_queryInProgress = false;
    m_responseTimer.stop();
    m_actionPollTimer.stop();
    m_requestQueue.clear();
    m_pendingRequest.reset();
    m_inFlightRequest.reset();
    // 失败后断开，由下一次人工 queryStatus() 显式建立新连接；不做自动重试。
    m_socket.abort();
    setState(State::Fault, reason);
    emit queryFinished(false, reason);
}

void ChargePileController::finishQuery()
{
    m_queryInProgress = false;
    m_snapshot.sampledAt = QDateTime::currentDateTime();
    if (snapshotConfirmsSafe(m_snapshot, m_settings)) {
        setState(State::SafeComplete, QStringLiteral("充电桩状态确认安全。"));
    } else {
        setState(State::Idle, QStringLiteral("充电桩只读状态查询完成。"));
    }
    emit snapshotChanged(m_snapshot);
    emit queryFinished(true, QStringLiteral("充电桩只读状态查询完成。"));
}

void ChargePileController::invalidateCommunicationContext(const QString &reason)
{
    const bool wasQueryInProgress = m_queryInProgress;
    m_queryInProgress = false;
    m_responseTimer.stop();
    m_actionPollTimer.stop();
    m_requestQueue.clear();
    m_pendingRequest.reset();
    m_inFlightRequest.reset();
    m_receiveBuffer.clear();
    m_lastSendTimer.invalidate();
    // abort() 立即丢弃旧目标尚未发送或尚未读取的 TCP 数据，下一次查询会重新连接。
    m_socket.abort();
    m_snapshot = ChargePileSnapshot{};
    setState(State::Unknown, reason);
    if (wasQueryInProgress)
        emit queryFinished(false, reason);
}

bool ChargePileController::communicationTargetChanged(
    const ChargeSettings &previous, const ChargeSettings &next)
{
    // slaveId 虽不改变 TCP 端点，却改变所有 RTU 帧归属，必须与 host/port 等同处理。
    return previous.host.trimmed() != next.host.trimmed()
           || previous.port != next.port
           || previous.slaveId != next.slaveId;
}

quint16 ChargePileController::responseWord(const QByteArray &frame, const int offset)
{
    // 响应数据区为 Modbus 大端 16 位寄存器，调用前已校验字节数与完整性。
    return (quint16(quint8(frame.at(offset))) << 8) | quint8(frame.at(offset + 1));
}

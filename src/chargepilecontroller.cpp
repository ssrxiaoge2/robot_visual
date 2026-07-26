#include "chargepilecontroller.h"

#include "chargepileprotocol.h"

#include <QMetaType>

namespace {

constexpr quint8 kReadInputRegisters = 0x04;
// QTcpSocket::write() 返回时数据可能仍在 Qt 的发送缓冲；比现场规定多保留 5ms
// 余量，确保以对端实际接收时刻度量时，相邻 RTU 请求也不会短于 50ms。
constexpr int kMinimumRequestIntervalMs = 55;

bool snapshotConfirmsSafe(const ChargePileSnapshot &snapshot, const ChargeSettings &settings)
{
    // “无输出”同时要求工作位、继电器位关闭，且电流不超过当前设置的安全阈值。
    return snapshot.retracted
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
    if (m_queryInProgress) {
        emit logMessage(QStringLiteral("充电桩查询进行中，拒绝替换通信设置。"));
        return;
    }

    const ChargeSettingsValidation validation = validateChargeSettings(settings);
    if (!validation.ok) {
        emit logMessage(QStringLiteral("充电桩设置校验失败：%1")
                        .arg(validation.errors.join(QStringLiteral("；"))));
        return;
    }

    m_settings = settings;
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
    m_currentRequest.reset();

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

    // 仅在前一帧已校验、回调执行并从 m_currentRequest 移除后才取下一帧。
    m_currentRequest = m_requestQueue.dequeue();
    scheduleCurrentRequest();
}

void ChargePileController::scheduleCurrentRequest()
{
    if (!m_currentRequest.has_value())
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
    if (!m_queryInProgress || !m_currentRequest.has_value())
        return;

    Request &request = *m_currentRequest;
    // 任务三的硬性保险：即使未来误把写请求排入队列，本阶段也不允许送往网络。
    if (request.isWriteCommand || request.expectedFunction != kReadInputRegisters) {
        failQuery(QStringLiteral("只读查询控制器拒绝发送写命令。"));
        return;
    }
    if (m_socket.state() != QAbstractSocket::ConnectedState) {
        failQuery(QStringLiteral("充电桩连接已断开，无法发送只读查询。"));
        return;
    }

    request.startedAt = QDateTime::currentDateTime();
    m_lastSendTimer.start();
    if (m_socket.write(request.frame) != request.frame.size()) {
        failQuery(QStringLiteral("充电桩只读查询写入套接字失败。"));
        return;
    }
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
    if (!m_queryInProgress || !m_currentRequest.has_value()) {
        emit logMessage(QStringLiteral("收到未关联到在途请求的充电桩响应。"));
        return;
    }

    const Request request = *m_currentRequest;
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

    m_currentRequest.reset();
    request.onSuccess(result.frame);
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

    if (m_currentRequest.has_value()) {
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
    m_currentRequest.reset();
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

quint16 ChargePileController::responseWord(const QByteArray &frame, const int offset)
{
    // 响应数据区为 Modbus 大端 16 位寄存器，调用前已校验字节数与完整性。
    return (quint16(quint8(frame.at(offset))) << 8) | quint8(frame.at(offset + 1));
}

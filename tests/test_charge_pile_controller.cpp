#include "chargepilecontroller.h"
#include "chargepileprotocol.h"
#include "chargesettings.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>

#include <algorithm>

using namespace ChargePileProtocol;

/**
 * @brief 回环充电桩夹具，模拟 TCP 透传链路后的 Modbus RTU 从站。
 *
 * 夹具保留客户端发来的每帧原始报文与接收时刻，既能验证读寄存器顺序和
 * 50ms 最小发送间隔，也能把首个响应分两次写入以覆盖 TCP 拆包场景。
 */
class FakeChargePile : public QObject
{
    Q_OBJECT

public:
    bool listen()
    {
        connect(&m_server, &QTcpServer::newConnection,
                this, &FakeChargePile::acceptClient);
        m_clock.start();
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    quint16 port() const { return m_server.serverPort(); }
    QList<QByteArray> requests() const { return m_requests; }
    QList<qint64> requestTimesMs() const { return m_requestTimesMs; }
    int connectionCount() const { return m_connectionCount; }

    /** 开启完整充电脚本：状态会随启动、停止、缩回和复位命令按现场流程变化。 */
    void enableChargeScenario()
    {
        m_chargeScenario = true;
        m_voltageTenths = 0;
        m_currentTenths = 0;
        m_inputWord = 0x0008;
        m_outputWord = 0;
        m_eventWord = 0;
        m_faultWord = 0x0080;
    }

    /** 令参数回读的电压原始整数与写入值不一致，用于证明启动前的硬校验。 */
    void setReadbackVoltageRaw(const quint16 value) { m_readbackVoltageOverride = value; }

    /** 设置缩回命令后的故障字；E8 应豁免，E12 等机械故障必须中止。 */
    void setFaultAfterRetract(const quint16 value) { m_faultAfterRetract = value; }

    /** 设置停止后的残余电流，验证未确认无输出前绝不发送缩回。 */
    void setCurrentAfterStop(const quint16 value) { m_currentAfterStop = value; }

    /** 直接调整当前输出电流原始值，用于同一持久恢复上下文的第二轮权威快照。 */
    void setLiveOutputCurrentRaw(const quint16 value) { m_currentTenths = value; }

    /** 调整当前故障字，模拟现场复位或人工排障后的下一轮权威状态。 */
    void setLiveFaultWord(const quint16 value) { m_faultWord = value; }

    /** 启动后始终不出现工作、继电器或电流，验证不会把初始 0A 当作自然结束。 */
    void setNeverProduceChargingOutput(const bool enabled) { m_neverProduceChargingOutput = enabled; }

    /** 丢弃指定线圈第一次写回显，模拟命令已经到桩但结果对上位机不确定。 */
    void dropFirstEchoForCoil(const quint16 address) { m_dropEchoCoil = address; }
    void mismatchFirstEchoForCoil(const quint16 address) { m_mismatchEchoCoil = address; }
    void dropFirstOffEchoForCoil(const quint16 address) { m_dropOffEchoCoil = address; }

    /** 在指定已确认命令后的下一轮完整状态读取首帧断开TCP，仅触发一次。 */
    void disconnectNextStatusAfterStart() { m_disconnectAfterStart = true; }
    void corruptNextStatusCrcAfterStart() { m_corruptStatusAfterStart = true; }
    void exceptionNextStatusAfterStart() { m_exceptionStatusAfterStart = true; }
    void timeoutNextStatusAfterStart() { m_timeoutStatusAfterStart = true; }
    void suppressAllStatusResponsesAfterStart()
    {
        m_suppressAllStatusResponsesAfterStart = true;
    }
    void suppressAllStatusResponsesAfterStop(const bool enabled)
    {
        m_suppressAllStatusResponsesAfterStop = enabled;
    }
    void suppressAllStatusResponsesAfterReset(const bool enabled)
    {
        m_suppressAllStatusResponsesAfterReset = enabled;
    }
    void disconnectNextStatusAfterStop() { m_disconnectAfterStop = true; }
    void disconnectNextStatusAfterRetractOn() { m_disconnectAfterRetractOn = true; }
    void disconnectNextStatusAfterReset() { m_disconnectAfterReset = true; }

    bool receivedWriteTo(const quint16 address) const
    {
        return std::any_of(m_requests.cbegin(), m_requests.cend(), [address](const QByteArray &frame) {
            return frame.size() == 8
                   && quint8(frame.at(1)) == quint8(0x06)
                   && requestAddress(frame) == address;
        });
    }

    bool receivedWriteValue(const quint16 address, const quint16 value) const
    {
        return std::any_of(m_requests.cbegin(), m_requests.cend(),
                           [address, value](const QByteArray &frame) {
            return frame.size() == 8
                   && quint8(frame.at(1)) == quint8(0x06)
                   && requestAddress(frame) == address
                   && requestValue(frame) == value;
        });
    }

    bool receivedCoil(const quint16 address) const { return coilCount(address) > 0; }

    int coilCount(const quint16 address) const
    {
        return int(std::count_if(m_requests.cbegin(), m_requests.cend(),
                                 [address](const QByteArray &frame) {
            return frame.size() == 8
                   && quint8(frame.at(1)) == quint8(0x05)
                   && requestAddress(frame) == address
                   && requestValue(frame) == quint16(0xFF00);
        }));
    }

    int coilOffCount(const quint16 address) const
    {
        return int(std::count_if(m_requests.cbegin(), m_requests.cend(),
                                 [address](const QByteArray &frame) {
            return frame.size() == 8
                   && quint8(frame.at(1)) == quint8(0x05)
                   && requestAddress(frame) == address
                   && requestValue(frame) == quint16(0x0000);
        }));
    }

    int completeStatusReadsBeforeFirstStart() const
    {
        int voltageReads = 0;
        for (const QByteArray &frame : m_requests) {
            if (quint8(frame.at(1)) == quint8(0x05)
                && requestAddress(frame) == kCoilStart) {
                break;
            }
            if (quint8(frame.at(1)) == quint8(0x04)
                && requestAddress(frame) == kRegOutVoltage) {
                ++voltageReads;
            }
        }
        return voltageReads;
    }

signals:
    /** 首轮预检开始时同步通知测试，以便在参数写入前尝试修改活动会话设置。 */
    void firstPrecheckStatusReadReceived();

    /** 测试可在回显发出前同步尝试切换目标，精确覆盖缩回活动阶段。 */
    void retractOnReceived();

    /** 关闭响应可让真实控制器走到响应超时和断开连接分支。 */
public:
    void setRespondToRequests(const bool enabled) { m_respondToRequests = enabled; }

    /** 设置返回帧站号，用于验证控制器切换 Modbus 从站后会按新站号重新查询。 */
    void setResponseSlaveId(const quint8 slaveId) { m_responseSlaveId = slaveId; }

    /**
     * @brief 设置五组状态读回的原始寄存器值。
     *
     * 测试通过不同的缩回、伸出及输出组合覆盖控制器的安全判断，而不绕过
     * 真实 TCP 与 RTU 拆包路径。
     */
    void setStatusWords(const quint16 voltageTenths, const quint16 currentTenths,
                        const quint16 inputWord, const quint16 outputWord,
                        const quint16 eventWord = 0x0020, const quint16 faultWord = 0x0080)
    {
        m_voltageTenths = voltageTenths;
        m_currentTenths = currentTenths;
        m_inputWord = inputWord;
        m_outputWord = outputWord;
        m_eventWord = eventWord;
        m_faultWord = faultWord;
    }

    /**
     * @brief 在指定请求的正常响应后追加同一帧，模拟网关延迟转发的重复旧帧。
     *
     * 重复帧在下一条请求的 70ms 节流等待窗口内送达，用于检验控制器不会
     * 把尚未写出的下一条请求错误地标记为可接收响应。
     */
    void duplicateResponseAfterRequest(const int requestNumber)
    {
        m_duplicateResponseAfterRequest = requestNumber;
    }

public:
    static quint16 requestAddress(const QByteArray &request)
    {
        return (quint16(quint8(request.at(2))) << 8) | quint8(request.at(3));
    }

    static quint16 requestValue(const QByteArray &request)
    {
        return (quint16(quint8(request.at(4))) << 8) | quint8(request.at(5));
    }

private:
    static QByteArray wordPayload(const quint16 first, const quint16 second)
    {
        QByteArray payload;
        payload.append(char((first >> 8) & 0xFF));
        payload.append(char(first & 0xFF));
        payload.append(char((second >> 8) & 0xFF));
        payload.append(char(second & 0xFF));
        return payload;
    }

    static QByteArray readResponse(const quint8 slaveId, const quint8 function,
                                   const QByteArray &payload)
    {
        QByteArray response;
        response.append(char(slaveId));
        response.append(char(function));
        response.append(char(payload.size()));
        response.append(payload);
        const quint16 crc = crc16(QByteArrayView(response));
        response.append(char(crc & 0xFF));
        response.append(char((crc >> 8) & 0xFF));
        return response;
    }

    void updateScenarioBeforeResponse(const QByteArray &request)
    {
        if (!m_chargeScenario)
            return;

        const quint8 function = quint8(request.at(1));
        const quint16 address = requestAddress(request);
        if (function == 0x06) {
            m_holdingRegisters[address] = requestValue(request);
            return;
        }
        if (function != 0x05 || requestValue(request) != 0xFF00)
            return;

        if (address == kCoilStart) {
            m_started = true;
            m_chargeStatusReadCount = 0;
            m_inputWord = 0x0004;
            if (!m_neverProduceChargingOutput) {
                m_currentTenths = 50;
                m_outputWord = 0x0210;
            } else {
                m_currentTenths = 0;
                m_outputWord = 0;
            }
        } else if (address == kCoilStop) {
            m_stopped = true;
            m_outputWord = 0;
            m_currentTenths = m_currentAfterStop;
        } else if (address == kCoilRetract) {
            m_retractOnConfirmed = true;
            m_inputWord = 0x0008;
            m_faultWord = m_faultAfterRetract;
            emit retractOnReceived();
        } else if (address == kCoilReset) {
            m_resetConfirmed = true;
            m_inputWord = 0x0008;
            m_outputWord = 0;
            m_currentTenths = 0;
        }
    }

    QByteArray responseFor(const QByteArray &request)
    {
        const quint8 function = quint8(request.at(1));
        const quint16 address = requestAddress(request);
        if (m_chargeScenario && function == 0x04 && address == kRegOutVoltage
            && !m_started && !m_firstPrecheckStatusReadEmitted) {
            m_firstPrecheckStatusReadEmitted = true;
            emit firstPrecheckStatusReadReceived();
        }
        updateScenarioBeforeResponse(request);

        if ((function == 0x05 || function == 0x06)) {
            if (function == 0x05 && address == m_dropEchoCoil && !m_droppedEcho) {
                m_droppedEcho = true;
                return {};
            }
            if (function == 0x05 && address == m_dropOffEchoCoil
                && requestValue(request) == 0x0000 && !m_droppedOffEcho) {
                m_droppedOffEcho = true;
                return {};
            }
            if (function == 0x05 && address == m_mismatchEchoCoil
                && !m_mismatchedEcho) {
                m_mismatchedEcho = true;
                return buildWriteCoilRequest(m_responseSlaveId, address, false);
            }
            return request;
        }

        if (function == 0x03) {
            QByteArray payload;
            const quint16 count = requestValue(request);
            for (quint16 offset = 0; offset < count; ++offset) {
                quint16 value = m_holdingRegisters.value(address + offset, 0);
                if (address + offset == kRegSetVoltage && m_readbackVoltageOverride.has_value())
                    value = *m_readbackVoltageOverride;
                payload.append(char((value >> 8) & 0xFF));
                payload.append(char(value & 0xFF));
            }
            return readResponse(m_responseSlaveId, function, payload);
        }

        if (m_chargeScenario && function == 0x04 && address == kRegOutVoltage && m_started
            && !m_stopped) {
            ++m_chargeStatusReadCount;
            // 第一轮为启动确认，第二轮证明已观察过输出，第三轮模拟自然停充。
            if (!m_neverProduceChargingOutput && m_chargeStatusReadCount >= 3) {
                m_currentTenths = 0;
                m_outputWord = 0;
            }
        }
        switch (address) {
        case kRegOutVoltage:
            return readResponse(m_responseSlaveId, function,
                                wordPayload(m_voltageTenths, m_currentTenths));
        case kRegInputSignals:
            return readResponse(m_responseSlaveId, function,
                                wordPayload(0, m_inputWord).right(2));
        case kRegOutputSignals:
            return readResponse(m_responseSlaveId, function,
                                wordPayload(0, m_outputWord).right(2));
        case kRegEvent:
            return readResponse(m_responseSlaveId, function,
                                wordPayload(0, m_eventWord).right(2));
        case kRegError:
            return readResponse(m_responseSlaveId, function,
                                wordPayload(0, m_faultWord).right(2));
        default:
            return {};
        }
    }

    static QByteArray exceptionResponse(const quint8 slaveId,
                                        const quint8 function,
                                        const quint8 exceptionCode)
    {
        QByteArray response;
        response.append(char(slaveId));
        response.append(char(function | 0x80));
        response.append(char(exceptionCode));
        const quint16 crc = crc16(QByteArrayView(response));
        response.append(char(crc & 0xFF));
        response.append(char((crc >> 8) & 0xFF));
        return response;
    }

    bool shouldDisconnectBeforeResponse(const QByteArray &request)
    {
        if (quint8(request.at(1)) != quint8(0x04)
            || requestAddress(request) != kRegOutVoltage) {
            return false;
        }
        if (m_disconnectAfterReset && m_resetConfirmed) {
            m_disconnectAfterReset = false;
            return true;
        }
        if (m_disconnectAfterRetractOn && m_retractOnConfirmed) {
            m_disconnectAfterRetractOn = false;
            return true;
        }
        if (m_disconnectAfterStop && m_stopped) {
            m_disconnectAfterStop = false;
            return true;
        }
        if (m_disconnectAfterStart && m_started) {
            m_disconnectAfterStart = false;
            return true;
        }
        return false;
    }

    void acceptClient()
    {
        m_client = m_server.nextPendingConnection();
        QVERIFY(m_client != nullptr);
        ++m_connectionCount;
        m_client->setParent(this);
        connect(m_client, &QTcpSocket::readyRead, this, &FakeChargePile::consumeRequests);
        QTcpSocket *const acceptedClient = m_client;
        connect(acceptedClient, &QTcpSocket::disconnected, this,
                [this, acceptedClient] {
                    // 异常恢复时新连接可能先建立、旧连接的 disconnected 后到达。
                    // 只有当前连接断开时才清空指针，防止旧连接事件误伤新连接，
                    // 导致 Fake 服务虽收到安全停止帧却不发送回显。
                    if (m_client == acceptedClient)
                        m_client = nullptr;
                });
    }

    void consumeRequests()
    {
        if (m_client == nullptr)
            return;

        m_buffer.append(m_client->readAll());
        while (m_buffer.size() >= 8) {
            const QByteArray request = m_buffer.left(8);
            m_buffer.remove(0, 8);
            m_requests.append(request);
            m_requestTimesMs.append(m_clock.elapsed());

            if (!m_respondToRequests)
                continue;

            if (shouldDisconnectBeforeResponse(request)) {
                m_client->abort();
                continue;
            }

            QByteArray response = responseFor(request);
            const bool isFirstStatusFrameAfterStart =
                quint8(request.at(1)) == quint8(0x04)
                && requestAddress(request) == kRegOutVoltage && m_started;
            if (isFirstStatusFrameAfterStart
                && ((m_suppressAllStatusResponsesAfterStop && m_stopped)
                    || (m_suppressAllStatusResponsesAfterReset
                        && m_resetConfirmed))) {
                continue;
            }
            if (isFirstStatusFrameAfterStart && m_suppressAllStatusResponsesAfterStart) {
                // 所有恢复连接仍保持TCP可达，但首组完整状态永远不回应，用于证明
                // 控制器按有限预算退出，且不会重发已唯一确认的启动或停止命令。
                continue;
            }
            if (isFirstStatusFrameAfterStart && m_timeoutStatusAfterStart) {
                // 保持TCP连接但不回应，精确模拟已确认启动后的只读响应超时。
                m_timeoutStatusAfterStart = false;
                continue;
            }
            if (isFirstStatusFrameAfterStart && m_corruptStatusAfterStart) {
                m_corruptStatusAfterStart = false;
                response[response.size() - 1] =
                    char(quint8(response.at(response.size() - 1)) ^ 0x01);
            } else if (isFirstStatusFrameAfterStart && m_exceptionStatusAfterStart) {
                m_exceptionStatusAfterStart = false;
                response = exceptionResponse(m_responseSlaveId, 0x04, 0x02);
            }
            if (response.isEmpty()) {
                QVERIFY2(m_droppedEcho || m_droppedOffEcho,
                         "测试夹具收到未定义地址的请求");
                continue;
            }
            if (m_requests.size() == 1) {
                // 首帧拆成两段，验证控制器不会把半帧误当完整 RTU 响应。
                m_client->write(response.left(3));
                const QPointer<QTcpSocket> client = m_client;
                QTimer::singleShot(2, this, [client, response] {
                    if (client != nullptr)
                        client->write(response.mid(3));
                });
            } else {
                // 后续帧一次写入；TCP 可将这些连续写操作与通知合并为同一批数据。
                m_client->write(response);
                if (m_requests.size() == m_duplicateResponseAfterRequest) {
                    const QPointer<QTcpSocket> client = m_client;
                    QTimer::singleShot(3, this, [client, response] {
                        if (client != nullptr)
                            client->write(response);
                    });
                }
            }
        }
    }

    QTcpServer m_server;
    QTcpSocket *m_client = nullptr;
    QByteArray m_buffer;
    QList<QByteArray> m_requests;
    QList<qint64> m_requestTimesMs;
    QElapsedTimer m_clock;
    bool m_respondToRequests = true;
    quint16 m_voltageTenths = 584;
    quint16 m_currentTenths = 126;
    quint16 m_inputWord = 0x0004;
    quint16 m_outputWord = 0x0210;
    quint16 m_eventWord = 0x0020;
    quint16 m_faultWord = 0x0080;
    int m_duplicateResponseAfterRequest = -1;
    int m_connectionCount = 0;
    quint8 m_responseSlaveId = 1;
    bool m_chargeScenario = false;
    bool m_started = false;
    bool m_stopped = false;
    bool m_neverProduceChargingOutput = false;
    bool m_droppedEcho = false;
    bool m_mismatchedEcho = false;
    bool m_droppedOffEcho = false;
    bool m_retractOnConfirmed = false;
    bool m_resetConfirmed = false;
    bool m_disconnectAfterStart = false;
    bool m_corruptStatusAfterStart = false;
    bool m_exceptionStatusAfterStart = false;
    bool m_timeoutStatusAfterStart = false;
    bool m_suppressAllStatusResponsesAfterStart = false;
    bool m_suppressAllStatusResponsesAfterStop = false;
    bool m_suppressAllStatusResponsesAfterReset = false;
    bool m_disconnectAfterStop = false;
    bool m_disconnectAfterRetractOn = false;
    bool m_disconnectAfterReset = false;
    bool m_firstPrecheckStatusReadEmitted = false;
    int m_chargeStatusReadCount = 0;
    quint16 m_currentAfterStop = 0;
    quint16 m_faultAfterRetract = 0x0080;
    quint16 m_dropEchoCoil = 0xFFFF;
    quint16 m_mismatchEchoCoil = 0xFFFF;
    quint16 m_dropOffEchoCoil = 0xFFFF;
    std::optional<quint16> m_readbackVoltageOverride;
    QHash<quint16, quint16> m_holdingRegisters;
};

/**
 * @brief 只在指定写帧上注入短写或 -1，其余请求仍走真实 QTcpSocket。
 *
 * QTcpSocket 在本机通常会一次接收完整 8 字节，无法稳定制造这两个返回值。
 * 该最小测试缝只替换生产控制器的单次 writeFrame() 返回，不绕过请求排队、
 * 50ms节流、sendCurrentRequest()、Unknown记录和后续真实TCP恢复矩阵。
 */
class ControlledWriteResultController final : public ChargePileController
{
public:
    void failNextWriteTo(const quint16 address, const qint64 result)
    {
        m_targetAddress = address;
        m_result = result;
        m_injected = false;
    }

    bool injected() const { return m_injected; }

protected:
    qint64 writeFrame(const QByteArray &frame) override
    {
        const auto identity =
            chargePileWriteIdentity(QByteArrayView(frame));
        if (!m_injected && identity.has_value()
            && identity->address == m_targetAddress) {
            m_injected = true;
            return m_result;
        }
        return ChargePileController::writeFrame(frame);
    }

private:
    quint16 m_targetAddress = 0xFFFF;
    qint64 m_result = -1;
    bool m_injected = false;
};

class ChargePileControllerTest : public QObject
{
    Q_OBJECT

private:
    static ChargeSettings loopbackSettings(const FakeChargePile &pile)
    {
        ChargeSettings settings = ChargeSettings::defaults();
        settings.host = QStringLiteral("127.0.0.1");
        settings.port = pile.port();
        settings.responseTimeoutMs = 100;
        settings.connectTimeoutMs = 100;
        settings.pollIntervalMs = 10;
        settings.startTimeoutMs = 1500;
        settings.monitorTimeoutMs = 1500;
        settings.stopTimeoutMs = 1000;
        settings.motionTimeoutMs = 1000;
        return settings;
    }

private slots:
    void rejectsSafeCurrentAbovePythonAuthorityAndNeverRetractsAtFiftyAmps()
    {
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.enableChargeScenario();
        // 500 原始值即 50.0A。即使操作员尝试把“无输出安全电流”误设为
        // 50A，控制器也必须保留现场 Python 已验证的 1.0A 上限并禁止缩回。
        pile.setCurrentAfterStop(500);

        ChargePileController controller;
        ChargeSettings accepted = loopbackSettings(pile);
        accepted.stopTimeoutMs = 500;
        QString error;
        QVERIFY(controller.applySettings(accepted, &error));

        ChargeSettings unsafe = accepted;
        unsafe.safeCurrentA = 50.0;
        QVERIFY2(!controller.applySettings(unsafe, &error),
                 "50A 不能被解释为安全无输出阈值");
        QCOMPARE(controller.appliedSettings().safeCurrentA, 1.0);

        QSignalSpy finishedSpy(
            &controller, &ChargePileController::chargeSessionFinished);
        QVERIFY(controller.startCharge(
            ChargePileController::SessionOrigin::Manual, &error));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 12000);
        QVERIFY(!finishedSpy.last().at(0).toBool());
        QCOMPARE(pile.coilCount(kCoilRetract), 0);
    }

    void rejectsPollIntervalsLongerThanFinitePhaseDeadlines()
    {
        ChargePileController controller;
        QString error;

        for (const QString &phase : {
                 QStringLiteral("启动"),
                 QStringLiteral("监控"),
                 QStringLiteral("停止"),
                 QStringLiteral("推杆")}) {
            ChargeSettings invalid = ChargeSettings::defaults();
            invalid.pollIntervalMs = 5000;
            if (phase == QStringLiteral("启动"))
                invalid.startTimeoutMs = 4999;
            else if (phase == QStringLiteral("监控"))
                invalid.monitorTimeoutMs = 4999;
            else if (phase == QStringLiteral("停止"))
                invalid.stopTimeoutMs = 4999;
            else
                invalid.motionTimeoutMs = 4999;

            QVERIFY2(!controller.applySettings(invalid, &error),
                     qPrintable(QStringLiteral("%1阶段不能接受大于截止期限的轮询间隔")
                                    .arg(phase)));
        }

        ChargeSettings monitorDisabled = ChargeSettings::defaults();
        monitorDisabled.monitorTimeoutMs = 0;
        QVERIFY2(controller.applySettings(monitorDisabled, &error),
                 qPrintable(error));
    }

    void phasePollingDelayUsesRemainingDeadlineDeterministically()
    {
        // 剩余120ms时必须缩短原250ms轮询等待；该断言不受 Windows 定时器
        // 调度抖动、TCP回环和五帧快照耗时影响，并直接覆盖生产调度调用的纯函数。
        QCOMPARE(boundedChargePhasePollDelayMs(250, 650, 530), 120);
        QCOMPARE(boundedChargePhasePollDelayMs(250, 650, 649), 1);
        QCOMPARE(boundedChargePhasePollDelayMs(250, 650, 650), 1);
        QCOMPARE(boundedChargePhasePollDelayMs(250, 650, 700), 1);

        // monitorTimeoutMs=0 表示无限监控，只能沿用完整轮询间隔。
        QCOMPARE(boundedChargePhasePollDelayMs(250, 0, 5000), 250);
    }

    void queryReadsFiveGroupsSeriallyAndNeverWrites()
    {
        // 若控制器改为并发收发、寄存器地址错误或误发写命令，本测试的顺序和功能码断言会失败。
        FakeChargePile pile;
        QVERIFY(pile.listen());

        ChargePileController controller;
        controller.applySettings(loopbackSettings(pile));
        QSignalSpy finishedSpy(&controller, &ChargePileController::queryFinished);
        QSignalSpy snapshotSpy(&controller, &ChargePileController::snapshotChanged);

        controller.queryStatus();
        controller.queryStatus();

        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 2, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(pile.requests().size(), 5, 3000);
        QCOMPARE(snapshotSpy.count(), 1);
        QVERIFY(!controller.isBusy());

        bool sawSuccessfulQuery = false;
        bool sawRejectedConcurrentQuery = false;
        for (const QList<QVariant> &arguments : finishedSpy) {
            const bool ok = arguments.at(0).toBool();
            sawSuccessfulQuery |= ok;
            sawRejectedConcurrentQuery |= !ok && arguments.at(1).toString().contains(QStringLiteral("进行"));
        }
        QVERIFY(sawSuccessfulQuery);
        QVERIFY(sawRejectedConcurrentQuery);

        const QList<QByteArray> requests = pile.requests();
        const QList<quint16> expectedAddresses = {
            kRegOutVoltage, kRegInputSignals, kRegOutputSignals, kRegEvent, kRegError
        };
        QCOMPARE(requests.size(), expectedAddresses.size());
        for (int index = 0; index < requests.size(); ++index) {
            const QByteArray &request = requests.at(index);
            QCOMPARE(quint8(request.at(0)), quint8(1));
            QCOMPARE(quint8(request.at(1)), quint8(0x04));
            const quint16 address = (quint16(quint8(request.at(2))) << 8)
                                  | quint8(request.at(3));
            QCOMPARE(address, expectedAddresses.at(index));
            QVERIFY(quint8(request.at(1)) != quint8(0x05));
            QVERIFY(quint8(request.at(1)) != quint8(0x06));
        }

        const QList<qint64> requestTimes = pile.requestTimesMs();
        QCOMPARE(requestTimes.size(), requests.size());
        for (int index = 1; index < requestTimes.size(); ++index) {
            QVERIFY2(requestTimes.at(index) - requestTimes.at(index - 1) >= 50,
                     "连续RTU请求未遵守现场协议规定的50ms最小发送间隔");
        }

        const ChargePileSnapshot snapshot = controller.snapshot();
        QCOMPARE(snapshot.outputVoltageV, 58.4);
        QCOMPARE(snapshot.outputCurrentA, 12.6);
        QVERIFY(snapshot.extended);
        QVERIFY(!snapshot.retracted);
        QVERIFY(snapshot.working);
        QVERIFY(snapshot.relayOn);
        QVERIFY(eventBit(snapshot.eventWord, 5));
        QVERIFY(faultBit(snapshot.faultWord, 7));
        QVERIFY(snapshot.sampledAt.isValid());
        QVERIFY(controller.shutdownRequired());
    }

    void queryTimesOutWithoutAutomaticRetry()
    {
        // 若控制器吞掉响应超时或自动重发，查询不会以失败结束或夹具会收到额外请求。
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.setRespondToRequests(false);

        ChargePileController controller;
        controller.applySettings(loopbackSettings(pile));
        QSignalSpy finishedSpy(&controller, &ChargePileController::queryFinished);

        controller.queryStatus();

        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1000);
        QCOMPARE(finishedSpy.at(0).at(0).toBool(), false);
        QVERIFY(finishedSpy.at(0).at(1).toString().contains(QStringLiteral("超时")));
        QCOMPARE(pile.requests().size(), 1);
        QVERIFY(!controller.isBusy());
        QVERIFY(controller.shutdownRequired());
    }

    void changingCommunicationTargetReconnectsToNewService()
    {
        // 若 applySettings 复用旧 TCP 连接，第二次查询会继续落到 oldPile 而不是 newPile。
        FakeChargePile oldPile;
        FakeChargePile newPile;
        QVERIFY(oldPile.listen());
        QVERIFY(newPile.listen());

        ChargePileController controller;
        controller.applySettings(loopbackSettings(oldPile));
        QSignalSpy firstFinishedSpy(&controller, &ChargePileController::queryFinished);
        controller.queryStatus();
        QTRY_COMPARE_WITH_TIMEOUT(firstFinishedSpy.count(), 1, 1500);
        QCOMPARE(firstFinishedSpy.at(0).at(0).toBool(), true);
        QCOMPARE(oldPile.requests().size(), 5);

        controller.applySettings(loopbackSettings(newPile));
        QSignalSpy secondFinishedSpy(&controller, &ChargePileController::queryFinished);
        controller.queryStatus();

        QTRY_COMPARE_WITH_TIMEOUT(secondFinishedSpy.count(), 1, 1500);
        QCOMPARE(secondFinishedSpy.at(0).at(0).toBool(), true);
        QCOMPARE(newPile.requests().size(), 5);
        QCOMPARE(oldPile.requests().size(), 5);
    }

    void changingCommunicationTargetInvalidatesPreviousSafeSnapshot()
    {
        // 若目标切换后仍保留 SafeComplete 快照，调用方会把未知新设备误判为可安全结束。
        FakeChargePile oldPile;
        FakeChargePile newPile;
        QVERIFY(oldPile.listen());
        QVERIFY(newPile.listen());
        oldPile.setStatusWords(0, 0, 0x0008, 0x0000, 0, 0x0080);

        ChargePileController controller;
        controller.applySettings(loopbackSettings(oldPile));
        QSignalSpy finishedSpy(&controller, &ChargePileController::queryFinished);
        controller.queryStatus();
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1500);
        QVERIFY(!controller.shutdownRequired());
        QVERIFY(controller.snapshot().sampledAt.isValid());

        controller.applySettings(loopbackSettings(newPile));

        QVERIFY(controller.shutdownRequired());
        QVERIFY(!controller.snapshot().sampledAt.isValid());
    }

    void duplicateOldFrameDuringThrottleNeverBuildsWrongSnapshot()
    {
        // 若待发送请求提前占据在途槽位，第二个输入字响应会被误用于解析第三个输出字。
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.duplicateResponseAfterRequest(2);

        ChargePileController controller;
        controller.applySettings(loopbackSettings(pile));
        QSignalSpy finishedSpy(&controller, &ChargePileController::queryFinished);
        controller.queryStatus();

        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1500);
        if (finishedSpy.at(0).at(0).toBool()) {
            QCOMPARE(controller.snapshot().outputWord, quint16(0x0210));
            QCOMPARE(pile.requests().size(), 5);
        } else {
            QCOMPARE(pile.requests().size(), 2);
            QVERIFY(controller.shutdownRequired());
        }
    }

    void contradictoryExtendedAndRetractedBitsNeverConfirmSafe()
    {
        // 若安全判定只看缩到位，机构两端限位同时为真时会错误进入 SafeComplete。
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.setStatusWords(0, 0, 0x000C, 0x0000, 0, 0x0080);

        ChargePileController controller;
        controller.applySettings(loopbackSettings(pile));
        QSignalSpy finishedSpy(&controller, &ChargePileController::queryFinished);
        controller.queryStatus();

        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1500);
        QCOMPARE(finishedSpy.at(0).at(0).toBool(), true);
        QVERIFY(controller.snapshot().extended);
        QVERIFY(controller.snapshot().retracted);
        QVERIFY(controller.shutdownRequired());
    }

    void changingHostInvalidatesSnapshotAndNeverQueriesOldService()
    {
        // 若 host 变化未被当作通信目标变化，第二轮查询会复用旧连接并继续到达 oldPile。
        FakeChargePile oldPile;
        QVERIFY(oldPile.listen());
        oldPile.setStatusWords(0, 0, 0x0008, 0x0000, 0, 0x0080);

        ChargePileController controller;
        controller.applySettings(loopbackSettings(oldPile));
        QSignalSpy firstFinishedSpy(&controller, &ChargePileController::queryFinished);
        controller.queryStatus();
        QTRY_COMPARE_WITH_TIMEOUT(firstFinishedSpy.count(), 1, 1500);
        QVERIFY(!controller.shutdownRequired());

        ChargeSettings changedHost = loopbackSettings(oldPile);
        changedHost.host = QStringLiteral("127.0.0.2");
        controller.applySettings(changedHost);
        QVERIFY(controller.shutdownRequired());
        QVERIFY(!controller.snapshot().sampledAt.isValid());

        QSignalSpy secondFinishedSpy(&controller, &ChargePileController::queryFinished);
        controller.queryStatus();
        QTRY_COMPARE_WITH_TIMEOUT(secondFinishedSpy.count(), 1, 1500);
        QVERIFY(!secondFinishedSpy.at(0).at(0).toBool());
        QCOMPARE(oldPile.requests().size(), 5);
    }

    void changingSlaveIdInvalidatesSnapshotAndUsesNewStationId()
    {
        // 若 slaveId 变化未使连接上下文失效，第二轮 RTU 请求首字节仍会错误保留为 1。
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.setStatusWords(0, 0, 0x0008, 0x0000, 0, 0x0080);

        ChargePileController controller;
        controller.applySettings(loopbackSettings(pile));
        QSignalSpy firstFinishedSpy(&controller, &ChargePileController::queryFinished);
        controller.queryStatus();
        QTRY_COMPARE_WITH_TIMEOUT(firstFinishedSpy.count(), 1, 1500);
        QVERIFY(!controller.shutdownRequired());

        ChargeSettings changedSlave = loopbackSettings(pile);
        changedSlave.slaveId = 2;
        pile.setResponseSlaveId(2);
        controller.applySettings(changedSlave);
        QVERIFY(controller.shutdownRequired());
        QVERIFY(!controller.snapshot().sampledAt.isValid());

        QSignalSpy secondFinishedSpy(&controller, &ChargePileController::queryFinished);
        controller.queryStatus();
        QTRY_COMPARE_WITH_TIMEOUT(secondFinishedSpy.count(), 1, 1500);
        QCOMPARE(secondFinishedSpy.at(0).at(0).toBool(), true);
        QTRY_COMPARE_WITH_TIMEOUT(pile.requests().size(), 10, 1500);
        QCOMPARE(quint8(pile.requests().at(5).at(0)), quint8(2));
    }

    void nonTargetSettingsUpdateKeepsExistingTcpConnection()
    {
        // 若仅更新电参或超时也断开连接，第二轮查询会让回环桩接受额外 TCP 连接。
        FakeChargePile pile;
        QVERIFY(pile.listen());

        ChargePileController controller;
        controller.applySettings(loopbackSettings(pile));
        QSignalSpy firstFinishedSpy(&controller, &ChargePileController::queryFinished);
        controller.queryStatus();
        QTRY_COMPARE_WITH_TIMEOUT(firstFinishedSpy.count(), 1, 1500);
        QCOMPARE(pile.connectionCount(), 1);

        ChargeSettings hotUpdated = loopbackSettings(pile);
        hotUpdated.voltageV = 60.0;
        hotUpdated.currentA = 45.0;
        hotUpdated.responseTimeoutMs = 200;
        controller.applySettings(hotUpdated);

        QSignalSpy secondFinishedSpy(&controller, &ChargePileController::queryFinished);
        controller.queryStatus();
        QTRY_COMPARE_WITH_TIMEOUT(secondFinishedSpy.count(), 1, 1500);
        QCOMPARE(secondFinishedSpy.at(0).at(0).toBool(), true);
        QCOMPARE(pile.connectionCount(), 1);
        QCOMPARE(pile.requests().size(), 10);
    }

    void completeChargeUsesPythonOrderAndEndsOnlyAfterFinalSafeSnapshot()
    {
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.enableChargeScenario();

        ChargePileController controller;
        controller.applySettings(loopbackSettings(pile));
        QSignalSpy finishedSpy(&controller, &ChargePileController::chargeSessionFinished);

        QString error;
        QVERIFY(controller.startCharge(ChargePileController::SessionOrigin::Manual, &error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 12000);

        QVERIFY(finishedSpy.first().at(0).toBool());
        QCOMPARE(finishedSpy.first().at(1).value<ChargePileController::SessionOrigin>(),
                 ChargePileController::SessionOrigin::Manual);
        QVERIFY(!controller.shutdownRequired());
        QCOMPARE(pile.completeStatusReadsBeforeFirstStart(), 2);
        QVERIFY(pile.receivedWriteValue(kRegSetVoltage, 584));
        QVERIFY(pile.receivedWriteValue(kRegSetCurrent, 500));
        QVERIFY(!pile.receivedWriteTo(kRegSetCutoffCurrent));
        QVERIFY(!pile.receivedWriteTo(kRegSetMaxSeconds));
        QCOMPARE(pile.coilCount(kCoilStart), 1);
        QCOMPARE(pile.coilCount(kCoilStop), 1);
        QCOMPARE(pile.coilCount(kCoilRetract), 1);
        QCOMPARE(pile.coilOffCount(kCoilRetract), 1);
        QCOMPARE(pile.coilCount(kCoilReset), 1);
        QVERIFY(controller.snapshot().retracted);
        QVERIFY(!controller.snapshot().extended);
        QVERIFY(!controller.snapshot().working);
        QVERIFY(!controller.snapshot().relayOn);
        QVERIFY(controller.snapshot().outputCurrentA <= 1.0);

        struct ExpectedRequest {
            quint8 function;
            quint16 address;
            std::optional<quint16> value;
        };
        QList<ExpectedRequest> expected;
        const auto appendStatus = [&expected] {
            expected.append({
                {0x04, kRegOutVoltage, std::nullopt},
                {0x04, kRegInputSignals, std::nullopt},
                {0x04, kRegOutputSignals, std::nullopt},
                {0x04, kRegEvent, std::nullopt},
                {0x04, kRegError, std::nullopt}
            });
        };
        appendStatus();
        expected.append({0x06, kRegSetVoltage, 584});
        expected.append({0x06, kRegSetCurrent, 500});
        expected.append({0x03, kRegSetVoltage, std::nullopt});
        appendStatus();
        expected.append({0x05, kCoilStart, 0xFF00});
        appendStatus(); // 等待启动确认。
        appendStatus(); // 监控中确认已经出现输出。
        appendStatus(); // 监控中确认输出自然结束。
        expected.append({0x05, kCoilStop, 0xFF00});
        appendStatus();
        expected.append({0x05, kCoilRetract, 0xFF00});
        appendStatus();
        expected.append({0x05, kCoilRetract, 0x0000});
        expected.append({0x05, kCoilReset, 0xFF00});
        appendStatus();

        const QList<QByteArray> requests = pile.requests();
        QCOMPARE(requests.size(), expected.size());
        for (int index = 0; index < requests.size(); ++index) {
            QCOMPARE(quint8(requests.at(index).at(1)), expected.at(index).function);
            QCOMPARE(FakeChargePile::requestAddress(requests.at(index)),
                     expected.at(index).address);
            if (expected.at(index).value.has_value()) {
                QCOMPARE(FakeChargePile::requestValue(requests.at(index)),
                         *expected.at(index).value);
            }
        }
    }

    void readbackMismatchAndNonExemptPrecheckNeverSendStart()
    {
        {
            FakeChargePile pile;
            QVERIFY(pile.listen());
            pile.enableChargeScenario();
            pile.setReadbackVoltageRaw(583);

            ChargePileController controller;
            controller.applySettings(loopbackSettings(pile));
            QSignalSpy finishedSpy(&controller, &ChargePileController::chargeSessionFinished);
            QString error;
            QVERIFY(controller.startCharge(ChargePileController::SessionOrigin::Manual, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 6000);
            QVERIFY(!finishedSpy.first().at(0).toBool());
            QVERIFY(!pile.receivedCoil(kCoilStart));
        }

        {
            FakeChargePile pile;
            QVERIFY(pile.listen());
            pile.enableChargeScenario();
            pile.setStatusWords(0, 0, 0x0008, 0, 0, 0x0400); // E11 不在启动豁免集合。

            ChargePileController controller;
            controller.applySettings(loopbackSettings(pile));
            QSignalSpy finishedSpy(&controller, &ChargePileController::chargeSessionFinished);
            QString error;
            QVERIFY(controller.startCharge(ChargePileController::SessionOrigin::Manual, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 4000);
            QVERIFY(!finishedSpy.first().at(0).toBool());
            QVERIFY(!pile.receivedCoil(kCoilStart));
        }
    }

    void initialZeroOutputWaitsForMonitorLimitBeforeSafeShutdown()
    {
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.enableChargeScenario();
        pile.setNeverProduceChargingOutput(true);
        ChargeSettings settings = loopbackSettings(pile);
        settings.monitorTimeoutMs = 700;

        ChargePileController controller;
        controller.applySettings(settings);
        QSignalSpy finishedSpy(&controller, &ChargePileController::chargeSessionFinished);
        QElapsedTimer elapsed;
        elapsed.start();
        QString error;
        QVERIFY(controller.startCharge(ChargePileController::SessionOrigin::Automatic, &error));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 10000);

        QVERIFY(finishedSpy.first().at(0).toBool());
        QVERIFY2(elapsed.elapsed() >= settings.monitorTimeoutMs,
                 "从未观察到充电输出时，不得提前把 0A 解释为自然结束");
        QCOMPARE(pile.coilCount(kCoilStop), 1);
    }

    void retractIgnoresOnlyE8AndResidualCurrentBlocksRetraction()
    {
        {
            FakeChargePile pile;
            FakeChargePile otherPile;
            QVERIFY(pile.listen());
            QVERIFY(otherPile.listen());
            pile.enableChargeScenario();
            pile.setFaultAfterRetract(0x0800); // E12 伸出机械臂超时，缩回阶段不得豁免。

            ChargePileController controller;
            controller.applySettings(loopbackSettings(pile));
            connect(&pile, &FakeChargePile::retractOnReceived,
                    &controller, [&controller, &otherPile] {
                controller.applySettings(loopbackSettings(otherPile));
            });
            QSignalSpy finishedSpy(&controller, &ChargePileController::chargeSessionFinished);
            QString error;
            QVERIFY(controller.startCharge(ChargePileController::SessionOrigin::Manual, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 12000);
            QVERIFY(!finishedSpy.first().at(0).toBool());
            QVERIFY(pile.receivedCoil(kCoilRetract));
            QCOMPARE(pile.coilOffCount(kCoilRetract), 1);
            QVERIFY(!pile.receivedCoil(kCoilReset));
            QCOMPARE(otherPile.requests().size(), 0);
        }

        {
            FakeChargePile pile;
            QVERIFY(pile.listen());
            pile.enableChargeScenario();
            pile.setCurrentAfterStop(20); // 2.0A，高于默认 1.0A 安全阈值。
            ChargeSettings settings = loopbackSettings(pile);
            settings.stopTimeoutMs = 500;

            ChargePileController controller;
            controller.applySettings(settings);
            QSignalSpy finishedSpy(&controller, &ChargePileController::chargeSessionFinished);
            QString error;
            QVERIFY(controller.startCharge(ChargePileController::SessionOrigin::Manual, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 12000);
            QVERIFY(!finishedSpy.first().at(0).toBool());
            QVERIFY(!pile.receivedCoil(kCoilRetract));
        }
    }

    void lostStartEchoIsUnknownAndNeverBlindlyResendsStart()
    {
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.enableChargeScenario();
        pile.dropFirstEchoForCoil(kCoilStart);

        ChargePileController controller;
        controller.applySettings(loopbackSettings(pile));
        QSignalSpy finishedSpy(&controller, &ChargePileController::chargeSessionFinished);
        QSignalSpy stateSpy(&controller, &ChargePileController::stateChanged);
        QString error;
        QVERIFY(controller.startCharge(ChargePileController::SessionOrigin::Manual, &error));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 6000);

        QVERIFY(!finishedSpy.first().at(0).toBool());
        QCOMPARE(pile.coilCount(kCoilStart), 1);
        bool sawUnknown = false;
        for (const QList<QVariant> &arguments : stateSpy) {
            sawUnknown |= arguments.first().value<ChargePileController::State>()
                          == ChargePileController::State::Unknown;
        }
        QVERIFY(sawUnknown);

        QString secondStartError;
        QVERIFY(!controller.startCharge(ChargePileController::SessionOrigin::Manual,
                                        &secondStartError));
        QVERIFY(secondStartError.contains(QStringLiteral("未知")));

        const int requestCountBeforeRecovery = pile.requests().size();
        QSignalSpy querySpy(&controller, &ChargePileController::queryFinished);
        controller.queryStatus();
        QTRY_COMPARE_WITH_TIMEOUT(querySpy.count(), 1, 4000);
        for (int index = requestCountBeforeRecovery; index < pile.requests().size(); ++index)
            QCOMPARE(quint8(pile.requests().at(index).at(1)), quint8(0x04));
        QCOMPARE(pile.coilCount(kCoilStart), 1);

        QString unsafeStartError;
        QVERIFY(!controller.startCharge(ChargePileController::SessionOrigin::Manual,
                                        &unsafeStartError));

        pile.setStatusWords(0, 0, 0x0008, 0, 0, 0x0080);
        QSignalSpy safeQuerySpy(&controller, &ChargePileController::queryFinished);
        controller.queryStatus();
        QTRY_COMPARE_WITH_TIMEOUT(safeQuerySpy.count(), 1, 4000);
        QVERIFY(!controller.shutdownRequired());

        QString recoveredStartError;
        QVERIFY(controller.startCharge(ChargePileController::SessionOrigin::Manual,
                                       &recoveredStartError));
        controller.requestSafeStop(ChargePileController::StopReason::Manual);
    }

    void safeQueryCannotClearUncertainRetractOffContext()
    {
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.enableChargeScenario();
        pile.setFaultAfterRetract(0x0800);
        pile.dropFirstOffEchoForCoil(kCoilRetract);

        ChargePileController controller;
        controller.applySettings(loopbackSettings(pile));
        QSignalSpy chargeFinished(
            &controller, &ChargePileController::chargeSessionFinished);
        QString error;
        QVERIFY(controller.startCharge(
            ChargePileController::SessionOrigin::Manual, &error));
        QTRY_COMPARE_WITH_TIMEOUT(chargeFinished.count(), 1, 12000);
        QVERIFY(!chargeFinished.last().at(0).toBool());
        const auto uncertainBeforeQuery =
            controller.uncertainWriteIdentity();
        QVERIFY(uncertainBeforeQuery.has_value());
        QCOMPARE(uncertainBeforeQuery->address, kCoilRetract);
        QCOMPARE(uncertainBeforeQuery->value, quint16(0x0000));

        // 位置和输出均安全只能证明机构到位，不能证明缩回线圈已经释放。
        pile.setStatusWords(0, 0, 0x0008, 0, 0, 0x0080);
        QSignalSpy queryFinished(
            &controller, &ChargePileController::queryFinished);
        controller.queryStatus();
        QTRY_COMPARE_WITH_TIMEOUT(queryFinished.count(), 1, 5000);
        QVERIFY(queryFinished.last().at(0).toBool());
        QVERIFY(controller.shutdownRequired());
        QCOMPARE(controller.state(), ChargePileController::State::Unknown);

        QString blockedError;
        QVERIFY(!controller.startCharge(
            ChargePileController::SessionOrigin::Manual,
            &blockedError));
        QVERIFY(blockedError.contains(QStringLiteral("安全")));
        const auto uncertainAfterQuery =
            controller.uncertainWriteIdentity();
        QVERIFY(uncertainAfterQuery.has_value());
        QCOMPARE(uncertainAfterQuery->address, kCoilRetract);
        QCOMPARE(uncertainAfterQuery->value, quint16(0x0000));
    }

    void automaticUnknownSessionCanRunConservativeRecoveryWithoutRestart()
    {
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.enableChargeScenario();
        pile.dropFirstEchoForCoil(kCoilStart);

        ChargePileController controller;
        controller.applySettings(loopbackSettings(pile));
        QSignalSpy finishedSpy(&controller,
                               &ChargePileController::chargeSessionFinished);
        QSignalSpy shutdownSpy(
            &controller, &ChargePileController::applicationShutdownFinished);
        QString error;
        QVERIFY(controller.startCharge(
            ChargePileController::SessionOrigin::Automatic, &error));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 8000);
        QVERIFY(!finishedSpy.first().at(0).toBool());
        QCOMPARE(finishedSpy.first().at(1)
                     .value<ChargePileController::SessionOrigin>(),
                 ChargePileController::SessionOrigin::Automatic);
        QCOMPARE(pile.coilCount(kCoilStart), 1);

        QString recoveryError;
        QVERIFY2(controller.requestConservativeRecovery(
                     ChargePileController::SessionOrigin::Automatic,
                     ChargePileController::StopReason::Fault,
                     &recoveryError),
                 qPrintable(recoveryError));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 2, 12000);

        QVERIFY(finishedSpy.last().at(0).toBool());
        QCOMPARE(finishedSpy.last().at(1)
                     .value<ChargePileController::SessionOrigin>(),
                 ChargePileController::SessionOrigin::Automatic);
        QCOMPARE(pile.coilCount(kCoilStart), 1);
        QCOMPARE(pile.coilCount(kCoilStop), 1);
        QCOMPARE(pile.coilCount(kCoilRetract), 1);
        QCOMPARE(pile.coilOffCount(kCoilRetract), 1);
        QCOMPARE(pile.coilCount(kCoilReset), 1);
        QCOMPARE(shutdownSpy.count(), 0);
        QVERIFY(!controller.shutdownRequired());
    }

    void manualUnknownSessionCanRetryRecoveryAfterActiveFlowFinished()
    {
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.enableChargeScenario();
        pile.dropFirstEchoForCoil(kCoilStart);

        ChargePileController controller;
        controller.applySettings(loopbackSettings(pile));
        QSignalSpy finishedSpy(
            &controller, &ChargePileController::chargeSessionFinished);
        QString error;
        QVERIFY(controller.startCharge(
            ChargePileController::SessionOrigin::Manual, &error));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 8000);

        // 写回显未知的人工会话已经结束，不再有 active flow，但安全上下文
        // 必须保留。DeviceManager::stopChargePile() 正是依据这两个条件把面板
        // “停止充电”转入下面同一个 Manual 保守恢复入口。
        QVERIFY(!finishedSpy.first().at(0).toBool());
        QVERIFY(!controller.hasActiveChargeSession());
        QVERIFY(controller.shutdownRequired());
        QCOMPARE(pile.coilCount(kCoilStart), 1);

        QString recoveryError;
        QVERIFY2(controller.requestConservativeRecovery(
                     ChargePileController::SessionOrigin::Manual,
                     ChargePileController::StopReason::Manual,
                     &recoveryError),
                 qPrintable(recoveryError));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 2, 12000);

        QVERIFY2(finishedSpy.last().at(0).toBool(),
                 qPrintable(finishedSpy.last().at(2).toString()));
        QCOMPARE(finishedSpy.last().at(1)
                     .value<ChargePileController::SessionOrigin>(),
                 ChargePileController::SessionOrigin::Manual);
        QCOMPARE(pile.coilCount(kCoilStart), 1);
        QCOMPARE(pile.coilCount(kCoilStop), 1);
        QVERIFY(!controller.shutdownRequired());
    }

    void applicationShutdownConnectionFailureIsUnsafeRequiredAndOneShot()
    {
        // 先向系统申请一个空闲回环端口再释放，确保控制器连接失败来自真实
        // QTcpSocket 路径，而不是测试直接调用私有失败处理函数。
        QTcpServer portReservation;
        QVERIFY(portReservation.listen(QHostAddress::LocalHost, 0));
        const quint16 unusedPort = portReservation.serverPort();
        portReservation.close();

        ChargeSettings settings = ChargeSettings::defaults();
        settings.host = QStringLiteral("127.0.0.1");
        settings.port = unusedPort;
        settings.connectTimeoutMs = 100;
        settings.responseTimeoutMs = 100;

        ChargePileController controller;
        controller.applySettings(settings);
        QSignalSpy shutdownSpy(
            &controller, &ChargePileController::applicationShutdownFinished);

        controller.requestApplicationShutdown();
        // 关闭事件重入发生在第一轮结果尚未到达时，只能升级/等待同一流程。
        controller.requestApplicationShutdown();
        QTRY_COMPARE_WITH_TIMEOUT(shutdownSpy.count(), 1, 3000);
        QVERIFY(!shutdownSpy.first().at(0).toBool());
        QVERIFY(controller.state() != ChargePileController::State::SafeComplete);
        QVERIFY(controller.shutdownRequired());

        // 同一在途请求只发布一次最终结果；MainWindow 在收到失败结果后会进入
        // 双确认路径，不会再次调用 requestApplicationShutdown()。
        QTest::qWait(300);
        QCOMPARE(shutdownSpy.count(), 1);
        QVERIFY(controller.shutdownRequired());
    }

    void conservativeRecoveryNeverRepeatsUncertainSafetyWrites()
    {
        const auto runUnknownStop = [](const quint16 residualCurrent,
                                       const bool expectSafe) {
            FakeChargePile pile;
            QVERIFY(pile.listen());
            pile.enableChargeScenario();
            pile.setCurrentAfterStop(residualCurrent);
            pile.dropFirstEchoForCoil(kCoilStop);
            ChargePileController controller;
            controller.applySettings(loopbackSettings(pile));
            QSignalSpy finished(&controller,
                                &ChargePileController::chargeSessionFinished);
            connect(&controller, &ChargePileController::stateChanged,
                    &controller, [&controller](ChargePileController::State state,
                                               const QString &) {
                if (state == ChargePileController::State::Monitoring)
                    controller.requestSafeStop(ChargePileController::StopReason::Fault);
            });
            QString error;
            QVERIFY(controller.startCharge(
                ChargePileController::SessionOrigin::Automatic, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 8000);
            QVERIFY(!finished.first().at(0).toBool());
            QString recoveryError;
            QVERIFY(controller.requestConservativeRecovery(
                ChargePileController::SessionOrigin::Automatic,
                ChargePileController::StopReason::Fault, &recoveryError));
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 10000);
            QCOMPARE(finished.last().at(0).toBool(), expectSafe);
            QCOMPARE(pile.coilCount(kCoilStop), 1);
        };
        runUnknownStop(0, true);
        runUnknownStop(50, false);

        for (const quint16 uncertainCoil : {kCoilRetract, kCoilReset}) {
            FakeChargePile pile;
            QVERIFY(pile.listen());
            pile.enableChargeScenario();
            pile.dropFirstEchoForCoil(uncertainCoil);
            ChargePileController controller;
            controller.applySettings(loopbackSettings(pile));
            QSignalSpy finished(&controller,
                                &ChargePileController::chargeSessionFinished);
            QString error;
            QVERIFY(controller.startCharge(
                ChargePileController::SessionOrigin::Automatic, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 12000);
            QVERIFY(!finished.first().at(0).toBool());
            QString recoveryError;
            QVERIFY(controller.requestConservativeRecovery(
                ChargePileController::SessionOrigin::Automatic,
                ChargePileController::StopReason::Fault, &recoveryError));
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 12000);
            QVERIFY(finished.last().at(0).toBool());
            QCOMPARE(pile.coilCount(uncertainCoil), 1);
            if (uncertainCoil == kCoilRetract)
                QCOMPARE(pile.coilOffCount(kCoilRetract), 1);
        }
    }

    void unsafeRecoveryContextFreezesTargetAndAllSafetySettingsUntilSafe()
    {
        FakeChargePile pileA;
        FakeChargePile pileB;
        QVERIFY(pileA.listen());
        QVERIFY(pileB.listen());
        pileA.enableChargeScenario();
        pileA.setCurrentAfterStop(50); // 5.0A，高于原始 safeCurrentA=1.0A。
        pileA.dropFirstEchoForCoil(kCoilStop);

        ChargePileController controller;
        const ChargeSettings settingsA = loopbackSettings(pileA);
        controller.applySettings(settingsA);
        connect(&controller, &ChargePileController::stateChanged,
                &controller,
                [&controller](ChargePileController::State state,
                              const QString &) {
            if (state == ChargePileController::State::Monitoring) {
                controller.requestSafeStop(
                    ChargePileController::StopReason::Fault);
            }
        });
        QSignalSpy chargeFinished(
            &controller, &ChargePileController::chargeSessionFinished);
        QString error;
        QVERIFY(controller.startCharge(
            ChargePileController::SessionOrigin::Automatic, &error));
        QTRY_COMPARE_WITH_TIMEOUT(chargeFinished.count(), 1, 12000);
        QVERIFY(!chargeFinished.last().at(0).toBool());
        QCOMPARE(pileA.coilCount(kCoilStop), 1);

        // 先尝试切到 B，再尝试只修改 A 的安全电流和收尾时序。两次都必须
        // 被未安全上下文整体拒绝，不能出现“目标冻结但阈值已变化”的半冻结。
        ChargeSettings settingsB = loopbackSettings(pileB);
        settingsB.safeCurrentA = 0.8;
        settingsB.stopTimeoutMs = 10;
        controller.applySettings(settingsB);
        ChargeSettings changedSafetyOnA = settingsA;
        changedSafetyOnA.safeCurrentA = 0.8;
        changedSafetyOnA.stopTimeoutMs = 10;
        controller.applySettings(changedSafetyOnA);

        const int requestsOnABeforeRecovery = pileA.requests().size();
        QVERIFY(controller.requestConservativeRecovery(
            ChargePileController::SessionOrigin::Automatic,
            ChargePileController::StopReason::Fault, &error));
        QTRY_COMPARE_WITH_TIMEOUT(chargeFinished.count(), 2, 12000);
        QVERIFY(!chargeFinished.last().at(0).toBool());
        QVERIFY(pileA.requests().size() > requestsOnABeforeRecovery);
        QCOMPARE(pileB.requests().size(), 0);
        QCOMPARE(pileA.coilCount(kCoilStop), 1);

        // 原桩输出降到安全值后，同一 A 上下文才能完成；最终 safe 清除上下文
        // 后，设置 B 才被接受，随后的真实状态查询只发往 B。
        pileA.setLiveOutputCurrentRaw(0);
        QVERIFY(controller.requestConservativeRecovery(
            ChargePileController::SessionOrigin::Automatic,
            ChargePileController::StopReason::Fault, &error));
        QTRY_COMPARE_WITH_TIMEOUT(chargeFinished.count(), 3, 12000);
        QVERIFY(chargeFinished.last().at(0).toBool());

        controller.applySettings(settingsB);
        QSignalSpy queryFinished(
            &controller, &ChargePileController::queryFinished);
        controller.queryStatus();
        QTRY_COMPARE_WITH_TIMEOUT(queryFinished.count(), 1, 5000);
        QVERIFY(queryFinished.last().at(0).toBool());
        QCOMPARE(pileB.requests().size(), 5);
    }

    void faultRecoveryContextBlocksNewStartUntilFinalSafeSnapshot()
    {
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.enableChargeScenario();
        pile.setFaultAfterRetract(0x0800); // E12：缩回阶段非豁免机械故障。

        ChargePileController controller;
        controller.applySettings(loopbackSettings(pile));
        QSignalSpy chargeFinished(
            &controller, &ChargePileController::chargeSessionFinished);
        QString error;
        QVERIFY(controller.startCharge(
            ChargePileController::SessionOrigin::Automatic, &error));
        QTRY_COMPARE_WITH_TIMEOUT(chargeFinished.count(), 1, 12000);
        QVERIFY(!chargeFinished.last().at(0).toBool());
        const int startCountBeforeRejectedRetry =
            pile.coilCount(kCoilStart);

        // 该终态是 Fault 而非 Unknown，但缩回/释放后的最终安全快照尚未完成。
        // 直接 Start 必须同步拒绝，且不能清除旧里程碑或发送新 Start。
        QString blockedError;
        QVERIFY(!controller.startCharge(
            ChargePileController::SessionOrigin::Automatic, &blockedError));
        QVERIFY(blockedError.contains(QStringLiteral("安全")));
        QTest::qWait(200);
        QCOMPARE(pile.coilCount(kCoilStart),
                 startCountBeforeRejectedRetry);

        QVERIFY(controller.requestConservativeRecovery(
            ChargePileController::SessionOrigin::Automatic,
            ChargePileController::StopReason::Fault, &error));
        QTRY_COMPARE_WITH_TIMEOUT(chargeFinished.count(), 2, 12000);
        QVERIFY(chargeFinished.last().at(0).toBool());

        pile.setLiveFaultWord(0x0080);
        QString acceptedError;
        QVERIFY(controller.startCharge(
            ChargePileController::SessionOrigin::Automatic,
            &acceptedError));
        QTRY_COMPARE_WITH_TIMEOUT(
            pile.coilCount(kCoilStart),
            startCountBeforeRejectedRetry + 1, 8000);
    }

    void recoveryMilestonesSurviveAcrossMultipleRecoveryAttempts()
    {
        {
            FakeChargePile pile;
            QVERIFY(pile.listen());
            pile.enableChargeScenario();
            pile.dropFirstEchoForCoil(kCoilStart);
            ChargePileController controller;
            ChargeSettings settings = loopbackSettings(pile);
            controller.applySettings(settings);
            QSignalSpy finished(&controller,
                                &ChargePileController::chargeSessionFinished);
            QString error;
            QVERIFY(controller.startCharge(
                ChargePileController::SessionOrigin::Automatic, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 6000);

            pile.suppressAllStatusResponsesAfterStop(true);
            QVERIFY(controller.requestConservativeRecovery(
                ChargePileController::SessionOrigin::Automatic,
                ChargePileController::StopReason::Fault, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 8000);
            QVERIFY(!finished.last().at(0).toBool());
            QCOMPARE(pile.coilCount(kCoilStop), 1);

            pile.suppressAllStatusResponsesAfterStop(false);
            QVERIFY(controller.requestConservativeRecovery(
                ChargePileController::SessionOrigin::Automatic,
                ChargePileController::StopReason::Fault, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, 10000);
            QVERIFY(finished.last().at(0).toBool());
            QCOMPARE(pile.coilCount(kCoilStart), 1);
            QCOMPARE(pile.coilCount(kCoilStop), 1);
        }

        {
            FakeChargePile pile;
            QVERIFY(pile.listen());
            pile.enableChargeScenario();
            pile.dropFirstEchoForCoil(kCoilRetract);
            ChargePileController controller;
            ChargeSettings settings = loopbackSettings(pile);
            controller.applySettings(settings);
            QSignalSpy finished(&controller,
                                &ChargePileController::chargeSessionFinished);
            QString error;
            QVERIFY(controller.startCharge(
                ChargePileController::SessionOrigin::Automatic, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);

            pile.suppressAllStatusResponsesAfterReset(true);
            QVERIFY(controller.requestConservativeRecovery(
                ChargePileController::SessionOrigin::Automatic,
                ChargePileController::StopReason::Fault, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 10000);
            QVERIFY(!finished.last().at(0).toBool());
            const int retractOnCount = pile.coilCount(kCoilRetract);
            const int retractOffCount = pile.coilOffCount(kCoilRetract);
            const int resetCount = pile.coilCount(kCoilReset);

            pile.suppressAllStatusResponsesAfterReset(false);
            QVERIFY(controller.requestConservativeRecovery(
                ChargePileController::SessionOrigin::Automatic,
                ChargePileController::StopReason::Fault, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, 10000);
            QVERIFY(finished.last().at(0).toBool());
            QCOMPARE(pile.coilCount(kCoilRetract), retractOnCount);
            QCOMPARE(pile.coilOffCount(kCoilRetract), retractOffCount);
            QCOMPARE(pile.coilCount(kCoilReset), resetCount);
        }
    }

    void applicationShutdownUsesPersistentRecoveryContextWithoutRepeatingUnknownWrite()
    {
        const auto runUnknownCoil = [](const quint16 uncertainCoil) {
            FakeChargePile pile;
            QVERIFY(pile.listen());
            pile.enableChargeScenario();
            pile.dropFirstEchoForCoil(uncertainCoil);
            ChargePileController controller;
            controller.applySettings(loopbackSettings(pile));
            QSignalSpy chargeFinished(
                &controller, &ChargePileController::chargeSessionFinished);
            QSignalSpy shutdownFinished(
                &controller, &ChargePileController::applicationShutdownFinished);
            QString error;
            QVERIFY(controller.startCharge(
                ChargePileController::SessionOrigin::Automatic, &error));
            if (uncertainCoil == kCoilStop) {
                connect(&controller, &ChargePileController::stateChanged,
                        &controller,
                        [&controller](ChargePileController::State state,
                                      const QString &) {
                    if (state == ChargePileController::State::Monitoring)
                        controller.requestSafeStop(
                            ChargePileController::StopReason::Fault);
                });
            }
            QTRY_COMPARE_WITH_TIMEOUT(chargeFinished.count(), 1, 12000);
            const int before = uncertainCoil == kCoilRetract
                                   ? pile.coilOffCount(uncertainCoil)
                                   : pile.coilCount(uncertainCoil);
            controller.requestApplicationShutdown();
            QTRY_COMPARE_WITH_TIMEOUT(shutdownFinished.count(), 1, 12000);
            const int after = uncertainCoil == kCoilRetract
                                  ? pile.coilOffCount(uncertainCoil)
                                  : pile.coilCount(uncertainCoil);
            QCOMPARE(after, before);
            QCOMPARE(shutdownFinished.count(), 1);
        };

        runUnknownCoil(kCoilStop);

        // OFF 不确定使用已有 E12 路径产生；位置到位不能证明线圈已释放，关闭也不得重发。
        {
            FakeChargePile pile;
            QVERIFY(pile.listen());
            pile.enableChargeScenario();
            pile.setFaultAfterRetract(0x0800);
            pile.dropFirstOffEchoForCoil(kCoilRetract);
            ChargePileController controller;
            controller.applySettings(loopbackSettings(pile));
            QSignalSpy chargeFinished(
                &controller, &ChargePileController::chargeSessionFinished);
            QSignalSpy shutdownFinished(
                &controller, &ChargePileController::applicationShutdownFinished);
            QString error;
            QVERIFY(controller.startCharge(
                ChargePileController::SessionOrigin::Automatic, &error));
            QTRY_COMPARE_WITH_TIMEOUT(chargeFinished.count(), 1, 12000);
            const int offCount = pile.coilOffCount(kCoilRetract);
            controller.requestApplicationShutdown();
            QTRY_COMPARE_WITH_TIMEOUT(shutdownFinished.count(), 1, 12000);
            QCOMPARE(pile.coilOffCount(kCoilRetract), offCount);
            QCOMPARE(shutdownFinished.count(), 1);
        }

        runUnknownCoil(kCoilReset);

        // 保守恢复的五组预检已经在途时，程序关闭只能升级原因和完成通知。
        // 若旁路到普通 requestSafeStop()，结果未知的 Stop 会被立即重复发送。
        {
            FakeChargePile pile;
            QVERIFY(pile.listen());
            pile.enableChargeScenario();
            pile.dropFirstEchoForCoil(kCoilStop);
            ChargePileController controller;
            controller.applySettings(loopbackSettings(pile));
            connect(&controller, &ChargePileController::stateChanged,
                    &controller,
                    [&controller](ChargePileController::State state,
                                  const QString &) {
                if (state == ChargePileController::State::Monitoring) {
                    controller.requestSafeStop(
                        ChargePileController::StopReason::Fault);
                }
            });
            QSignalSpy chargeFinished(
                &controller, &ChargePileController::chargeSessionFinished);
            QSignalSpy shutdownFinished(
                &controller, &ChargePileController::applicationShutdownFinished);
            QString error;
            QVERIFY(controller.startCharge(
                ChargePileController::SessionOrigin::Automatic, &error));
            QTRY_COMPARE_WITH_TIMEOUT(chargeFinished.count(), 1, 12000);
            QCOMPARE(pile.coilCount(kCoilStop), 1);

            QVERIFY(controller.requestConservativeRecovery(
                ChargePileController::SessionOrigin::Automatic,
                ChargePileController::StopReason::Fault, &error));
            controller.requestApplicationShutdown();
            QTRY_COMPARE_WITH_TIMEOUT(shutdownFinished.count(), 1, 12000);
            QCOMPARE(pile.coilCount(kCoilStop), 1);
            QCOMPARE(shutdownFinished.count(), 1);
        }
    }

    void applicationShutdownOwnershipSurvivesSynchronousRecoveryReentry()
    {
        const auto runReentrantRecovery = [](const bool secondRecoverySafe) {
            FakeChargePile pile;
            QVERIFY(pile.listen());
            pile.enableChargeScenario();
            pile.suppressAllStatusResponsesAfterReset(true);

            ChargePileController controller;
            controller.applySettings(loopbackSettings(pile));
            QSignalSpy chargeFinished(
                &controller, &ChargePileController::chargeSessionFinished);
            QSignalSpy shutdownFinished(
                &controller, &ChargePileController::applicationShutdownFinished);

            bool shutdownRequested = false;
            connect(&controller, &ChargePileController::stateChanged,
                    &controller,
                    [&controller, &shutdownRequested](
                        ChargePileController::State state, const QString &) {
                if (!shutdownRequested
                    && state == ChargePileController::State::Monitoring) {
                    shutdownRequested = true;
                    controller.requestApplicationShutdown();
                }
            });

            bool reentrantRecoveryRequested = false;
            bool reentrantRecoveryAccepted = false;
            QString reentrantError;
            connect(&controller,
                    &ChargePileController::chargeSessionFinished,
                    &controller,
                    [&controller, &pile, secondRecoverySafe,
                     &reentrantRecoveryRequested,
                     &reentrantRecoveryAccepted,
                     &reentrantError](
                        const bool safe,
                        ChargePileController::SessionOrigin origin,
                        const QString &) {
                if (safe || reentrantRecoveryRequested)
                    return;
                reentrantRecoveryRequested = true;
                if (secondRecoverySafe)
                    pile.suppressAllStatusResponsesAfterReset(false);
                reentrantRecoveryAccepted =
                    controller.requestConservativeRecovery(
                        origin, ChargePileController::StopReason::Fault,
                        &reentrantError);
            });

            QString error;
            QVERIFY(controller.startCharge(
                ChargePileController::SessionOrigin::Automatic, &error));
            QTRY_VERIFY_WITH_TIMEOUT(reentrantRecoveryRequested, 15000);
            QVERIFY2(reentrantRecoveryAccepted,
                     qPrintable(reentrantError));
            QTRY_COMPARE_WITH_TIMEOUT(shutdownFinished.count(), 1, 18000);
            QCOMPARE(shutdownFinished.first().at(0).toBool(),
                     secondRecoverySafe);
            QCOMPARE(shutdownFinished.count(), 1);
            QTRY_COMPARE_WITH_TIMEOUT(chargeFinished.count(), 2, 18000);
            QCOMPARE(chargeFinished.last().at(0).toBool(),
                     secondRecoverySafe);
        };

        runReentrantRecovery(true);
        runReentrantRecovery(false);
    }

    void shortAndFailedWritesUseProductionSendPathAndPreserveIdentity()
    {
        {
            FakeChargePile pile;
            QVERIFY(pile.listen());
            pile.enableChargeScenario();
            ControlledWriteResultController controller;
            controller.applySettings(loopbackSettings(pile));
            controller.failNextWriteTo(kCoilStart, 4); // 8字节 Start 的受控短写。
            QSignalSpy finished(
                &controller, &ChargePileController::chargeSessionFinished);

            QString error;
            QVERIFY(controller.startCharge(
                ChargePileController::SessionOrigin::Automatic, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
            QVERIFY(controller.injected());
            QVERIFY(!finished.last().at(0).toBool());
            const auto uncertain = controller.uncertainWriteIdentity();
            QVERIFY(uncertain.has_value());
            QCOMPARE(uncertain->function, quint8(0x05));
            QCOMPARE(uncertain->address, kCoilStart);
            QCOMPARE(uncertain->value, quint16(0xFF00));
            QCOMPARE(pile.coilCount(kCoilStart), 0);

            QVERIFY(controller.requestConservativeRecovery(
                ChargePileController::SessionOrigin::Automatic,
                ChargePileController::StopReason::Fault, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 12000);
            QVERIFY(finished.last().at(0).toBool());
            QCOMPARE(pile.coilCount(kCoilStart), 0);
            QCOMPARE(pile.coilCount(kCoilStop), 1);
        }

        {
            FakeChargePile pile;
            QVERIFY(pile.listen());
            pile.enableChargeScenario();
            ControlledWriteResultController controller;
            controller.applySettings(loopbackSettings(pile));
            controller.failNextWriteTo(kCoilStop, -1);
            connect(&controller, &ChargePileController::stateChanged,
                    &controller,
                    [&controller](ChargePileController::State state,
                                  const QString &) {
                if (state == ChargePileController::State::Monitoring) {
                    controller.requestSafeStop(
                        ChargePileController::StopReason::Fault);
                }
            });
            QSignalSpy finished(
                &controller, &ChargePileController::chargeSessionFinished);

            QString error;
            QVERIFY(controller.startCharge(
                ChargePileController::SessionOrigin::Automatic, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 12000);
            QVERIFY(controller.injected());
            QVERIFY(!finished.last().at(0).toBool());
            const auto uncertain = controller.uncertainWriteIdentity();
            QVERIFY(uncertain.has_value());
            QCOMPARE(uncertain->function, quint8(0x05));
            QCOMPARE(uncertain->address, kCoilStop);
            QCOMPARE(uncertain->value, quint16(0xFF00));
            QCOMPARE(pile.coilCount(kCoilStop), 0);

            QVERIFY(controller.requestConservativeRecovery(
                ChargePileController::SessionOrigin::Automatic,
                ChargePileController::StopReason::Fault, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 12000);
            QCOMPARE(pile.coilCount(kCoilStop), 0);
        }
    }

    void writeIdentityIsAvailableBeforeSocketWriteResult()
    {
        const QByteArray frame =
            ChargePileProtocol::buildWriteCoilRequest(1, kCoilStop, true);
        const std::optional<ChargePileWriteIdentity> identity =
            chargePileWriteIdentity(QByteArrayView(frame));
        QVERIFY(identity.has_value());
        QCOMPARE(identity->function, quint8(0x05));
        QCOMPARE(identity->address, kCoilStop);
        QCOMPARE(identity->value, quint16(0xFF00));
    }

    void mismatchedStartEchoAlsoEntersUnknownGate()
    {
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.enableChargeScenario();
        pile.mismatchFirstEchoForCoil(kCoilStart);

        ChargePileController controller;
        controller.applySettings(loopbackSettings(pile));
        QSignalSpy finishedSpy(&controller, &ChargePileController::chargeSessionFinished);
        QString error;
        QVERIFY(controller.startCharge(ChargePileController::SessionOrigin::Manual, &error));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 6000);
        QVERIFY(!finishedSpy.first().at(0).toBool());
        QCOMPARE(pile.coilCount(kCoilStart), 1);

        QString blockedError;
        QVERIFY(!controller.startCharge(ChargePileController::SessionOrigin::Manual,
                                        &blockedError));
        QVERIFY(blockedError.contains(QStringLiteral("未知")));
    }

    void shutdownUpgradeUsesSameInFlightSafeStopOnlyOnce()
    {
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.enableChargeScenario();
        pile.setNeverProduceChargingOutput(true);

        ChargePileController controller;
        controller.applySettings(loopbackSettings(pile));
        QSignalSpy finishedSpy(&controller, &ChargePileController::chargeSessionFinished);
        QSignalSpy shutdownSpy(&controller,
                               &ChargePileController::applicationShutdownFinished);
        QString error;
        QVERIFY(controller.startCharge(ChargePileController::SessionOrigin::Manual, &error));
        QTRY_VERIFY_WITH_TIMEOUT(pile.receivedCoil(kCoilStart), 5000);

        controller.requestSafeStop(ChargePileController::StopReason::Manual);
        controller.requestApplicationShutdown();

        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(shutdownSpy.count(), 1, 10000);
        QVERIFY(shutdownSpy.first().at(0).toBool());
        QCOMPARE(pile.coilCount(kCoilStart), 1);
        QCOMPARE(pile.coilCount(kCoilStop), 1);
        QCOMPARE(pile.coilCount(kCoilRetract), 1);
        QCOMPARE(pile.coilCount(kCoilReset), 1);
    }

    void applicationShutdownWithoutActiveSessionConnectsThenSafelyCloses()
    {
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.enableChargeScenario();

        ChargePileController controller;
        controller.applySettings(loopbackSettings(pile));
        QSignalSpy shutdownSpy(&controller,
                               &ChargePileController::applicationShutdownFinished);

        controller.requestApplicationShutdown();

        QTRY_COMPARE_WITH_TIMEOUT(shutdownSpy.count(), 1, 10000);
        QVERIFY(shutdownSpy.first().at(0).toBool());
        QCOMPARE(pile.connectionCount(), 1);
        QCOMPARE(pile.coilCount(kCoilStop), 1);
        QCOMPARE(pile.coilCount(kCoilRetract), 1);
        QCOMPARE(pile.coilCount(kCoilReset), 1);
        QVERIFY(!controller.shutdownRequired());
    }

    void readFailuresRecoverFromConfirmedMilestonesWithoutRepeatingWrites()
    {
        {
            FakeChargePile pile;
            QVERIFY(pile.listen());
            pile.enableChargeScenario();
            pile.disconnectNextStatusAfterStart();
            ChargePileController controller;
            controller.applySettings(loopbackSettings(pile));
            QSignalSpy finishedSpy(&controller, &ChargePileController::chargeSessionFinished);
            QString error;
            QVERIFY(controller.startCharge(ChargePileController::SessionOrigin::Manual, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 12000);
            QVERIFY2(finishedSpy.first().at(0).toBool(),
                     qPrintable(finishedSpy.first().at(2).toString()));
            QCOMPARE(pile.coilCount(kCoilStart), 1);
            QCOMPARE(pile.coilCount(kCoilStop), 1);
            QVERIFY(pile.connectionCount() >= 2);
        }

        {
            FakeChargePile pile;
            QVERIFY(pile.listen());
            pile.enableChargeScenario();
            pile.disconnectNextStatusAfterStop();
            ChargePileController controller;
            controller.applySettings(loopbackSettings(pile));
            QSignalSpy finishedSpy(&controller, &ChargePileController::chargeSessionFinished);
            QString error;
            QVERIFY(controller.startCharge(ChargePileController::SessionOrigin::Manual, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 12000);
            QVERIFY(finishedSpy.first().at(0).toBool());
            QCOMPARE(pile.coilCount(kCoilStop), 1);
            QCOMPARE(pile.coilCount(kCoilReset), 1);
        }

        {
            FakeChargePile pile;
            QVERIFY(pile.listen());
            pile.enableChargeScenario();
            pile.disconnectNextStatusAfterReset();
            ChargePileController controller;
            controller.applySettings(loopbackSettings(pile));
            QSignalSpy finishedSpy(&controller, &ChargePileController::chargeSessionFinished);
            QString error;
            QVERIFY(controller.startCharge(ChargePileController::SessionOrigin::Manual, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 12000);
            QVERIFY(finishedSpy.first().at(0).toBool());
            QCOMPARE(pile.coilCount(kCoilReset), 1);
        }
    }

    void crcAndModbusReadFailuresAfterStartAlsoRecoverToSingleSafeStop()
    {
        const auto runScenario = [](const bool crcFailure) {
            FakeChargePile pile;
            QVERIFY(pile.listen());
            pile.enableChargeScenario();
            if (crcFailure)
                pile.corruptNextStatusCrcAfterStart();
            else
                pile.exceptionNextStatusAfterStart();

            ChargePileController controller;
            controller.applySettings(loopbackSettings(pile));
            QSignalSpy finishedSpy(&controller, &ChargePileController::chargeSessionFinished);
            QString error;
            QVERIFY(controller.startCharge(ChargePileController::SessionOrigin::Manual, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 12000);
            QVERIFY2(finishedSpy.first().at(0).toBool(),
                     qPrintable(finishedSpy.first().at(2).toString()));
            QCOMPARE(pile.coilCount(kCoilStart), 1);
            QCOMPARE(pile.coilCount(kCoilStop), 1);
            QVERIFY(pile.connectionCount() >= 2);
        };

        runScenario(true);
        runScenario(false);
    }

    void readTimeoutAfterStartAlsoRecoversToSingleSafeStop()
    {
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.enableChargeScenario();
        pile.timeoutNextStatusAfterStart();

        ChargePileController controller;
        controller.applySettings(loopbackSettings(pile));
        QSignalSpy finishedSpy(&controller, &ChargePileController::chargeSessionFinished);
        QString error;
        QVERIFY(controller.startCharge(ChargePileController::SessionOrigin::Manual, &error));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 12000);
        QVERIFY2(finishedSpy.first().at(0).toBool(),
                 qPrintable(finishedSpy.first().at(2).toString()));
        QCOMPARE(pile.coilCount(kCoilStart), 1);
        QCOMPARE(pile.coilCount(kCoilStop), 1);
        QVERIFY(pile.connectionCount() >= 2);
    }

    void retractReadFailureReconnectsOnlyToReleaseOffAndNeverResets()
    {
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.enableChargeScenario();
        pile.disconnectNextStatusAfterRetractOn();

        ChargePileController controller;
        controller.applySettings(loopbackSettings(pile));
        QSignalSpy finishedSpy(&controller, &ChargePileController::chargeSessionFinished);
        QString error;
        QVERIFY(controller.startCharge(ChargePileController::SessionOrigin::Manual, &error));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 12000);

        QVERIFY(!finishedSpy.first().at(0).toBool());
        QCOMPARE(pile.coilCount(kCoilRetract), 1);
        QCOMPARE(pile.coilOffCount(kCoilRetract), 1);
        QCOMPARE(pile.coilCount(kCoilReset), 0);
        QVERIFY(pile.connectionCount() >= 2);
    }

    void uncertainRetractOffEchoEntersUnknownAndRequiresManualConfirmation()
    {
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.enableChargeScenario();
        pile.setFaultAfterRetract(0x0800);
        pile.dropFirstOffEchoForCoil(kCoilRetract);

        ChargePileController controller;
        ChargeSettings settings = loopbackSettings(pile);
        settings.responseTimeoutMs = 50;
        controller.applySettings(settings);
        QSignalSpy finishedSpy(&controller, &ChargePileController::chargeSessionFinished);
        QSignalSpy stateSpy(&controller, &ChargePileController::stateChanged);
        QString error;
        QVERIFY(controller.startCharge(ChargePileController::SessionOrigin::Manual, &error));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 12000);

        QVERIFY(!finishedSpy.first().at(0).toBool());
        QCOMPARE(pile.coilCount(kCoilRetract), 1);
        QCOMPARE(pile.coilOffCount(kCoilRetract), 1);
        QCOMPARE(pile.coilCount(kCoilReset), 0);
        bool sawUnknown = false;
        for (const QList<QVariant> &arguments : stateSpy) {
            sawUnknown |= arguments.first().value<ChargePileController::State>()
                          == ChargePileController::State::Unknown;
        }
        QVERIFY(sawUnknown);
        QVERIFY(finishedSpy.first().at(2).toString().contains(QStringLiteral("人工")));

        QString recoveryError;
        QVERIFY(controller.requestConservativeRecovery(
            ChargePileController::SessionOrigin::Manual,
            ChargePileController::StopReason::Fault, &recoveryError));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 2, 8000);
        QVERIFY(!finishedSpy.last().at(0).toBool());
        QCOMPARE(pile.coilOffCount(kCoilRetract), 1);
    }

    void unknownWriteThenConservativeShutdownClearsGateAndAllowsNextStart()
    {
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.enableChargeScenario();
        pile.dropFirstEchoForCoil(kCoilStart);

        ChargePileController controller;
        ChargeSettings settings = loopbackSettings(pile);
        controller.applySettings(settings);
        QSignalSpy chargeFinishedSpy(
            &controller, &ChargePileController::chargeSessionFinished);
        QSignalSpy shutdownFinishedSpy(
            &controller, &ChargePileController::applicationShutdownFinished);
        QSignalSpy stateSpy(&controller, &ChargePileController::stateChanged);
        QString error;
        QVERIFY(controller.startCharge(
            ChargePileController::SessionOrigin::Manual, &error));
        QTRY_COMPARE_WITH_TIMEOUT(chargeFinishedSpy.count(), 1, 8000);
        QVERIFY(!chargeFinishedSpy.first().at(0).toBool());
        QCOMPARE(stateSpy.last().first().value<ChargePileController::State>(),
                 ChargePileController::State::Unknown);

        controller.requestApplicationShutdown();
        QTRY_COMPARE_WITH_TIMEOUT(shutdownFinishedSpy.count(), 1, 10000);
        QVERIFY2(shutdownFinishedSpy.first().at(0).toBool(),
                 qPrintable(shutdownFinishedSpy.first().at(1).toString()));
        QCOMPARE(stateSpy.last().first().value<ChargePileController::State>(),
                 ChargePileController::State::SafeComplete);
        QVERIFY(!controller.shutdownRequired());
        QTest::qWait(200);
        QCOMPARE(shutdownFinishedSpy.count(), 1);

        QString nextStartError;
        QVERIFY2(controller.startCharge(
                     ChargePileController::SessionOrigin::Manual, &nextStartError),
                 qPrintable(nextStartError));
        controller.requestSafeStop(ChargePileController::StopReason::Manual);
    }

    void persistentReadFailureAfterConfirmedStartExhaustsTwoRecoveries()
    {
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.enableChargeScenario();
        pile.suppressAllStatusResponsesAfterStart();

        ChargePileController controller;
        ChargeSettings settings = loopbackSettings(pile);
        // 本用例验证的是“Start 已确认后的状态读失败”。50ms 在 Windows
        // 高负载/重复运行时可能先误伤正常的参数写回显，使测试落入写结果未知；
        // 200ms 仍能快速制造读超时，同时给本机 TCP/Qt 事件循环留下确定余量。
        settings.responseTimeoutMs = 200;
        controller.applySettings(settings);
        QSignalSpy finishedSpy(
            &controller, &ChargePileController::chargeSessionFinished);
        QSignalSpy stateSpy(&controller, &ChargePileController::stateChanged);
        QString error;
        QVERIFY(controller.startCharge(
            ChargePileController::SessionOrigin::Manual, &error));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 8000);

        QVERIFY(!finishedSpy.first().at(0).toBool());
        QVERIFY(finishedSpy.first().at(2).toString().contains(
            QStringLiteral("有限重连失败")));
        QCOMPARE(stateSpy.last().first().value<ChargePileController::State>(),
                 ChargePileController::State::Unknown);
        QCOMPARE(pile.coilCount(kCoilStart), 1);
        QCOMPARE(pile.coilCount(kCoilStop), 1);
        QCOMPARE(pile.connectionCount(), 3);
        const int stableRequestCount = pile.requests().size();
        QTest::qWait(300);
        QCOMPARE(pile.requests().size(), stableRequestCount);
        QCOMPARE(pile.connectionCount(), 3);
        QCOMPARE(finishedSpy.count(), 1);
        QVERIFY(!controller.isBusy());
    }

    void activeChargeRejectsNonTargetSettingsAndKeepsOriginalValuesAndTiming()
    {
        FakeChargePile pile;
        QVERIFY(pile.listen());
        pile.enableChargeScenario();

        ChargePileController controller;
        ChargeSettings original = loopbackSettings(pile);
        original.pollIntervalMs = 250;
        controller.applySettings(original);

        ChargeSettings candidate = original;
        candidate.voltageV = 60.0;
        candidate.currentA = 40.0;
        candidate.responseTimeoutMs = 1;
        candidate.pollIntervalMs = 1;
        bool candidateAccepted = true;
        QString candidateError;
        connect(&pile, &FakeChargePile::firstPrecheckStatusReadReceived,
                &controller, [&controller, candidate,
                              &candidateAccepted, &candidateError] {
                    candidateAccepted =
                        controller.applySettings(candidate, &candidateError);
                });

        QSignalSpy finishedSpy(
            &controller, &ChargePileController::chargeSessionFinished);
        QString error;
        QVERIFY(controller.startCharge(
            ChargePileController::SessionOrigin::Manual, &error));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 15000);
        QVERIFY2(finishedSpy.first().at(0).toBool(),
                 qPrintable(finishedSpy.first().at(2).toString()));
        QVERIFY(!candidateAccepted);
        QVERIFY(candidateError.contains(QStringLiteral("安全恢复上下文")));

        QVERIFY(pile.receivedWriteValue(kRegSetVoltage, 584));
        QVERIFY(pile.receivedWriteValue(kRegSetCurrent, 500));
        QVERIFY(!pile.receivedWriteValue(kRegSetVoltage, 600));
        QVERIFY(!pile.receivedWriteValue(kRegSetCurrent, 400));

        const QList<QByteArray> requests = pile.requests();
        const QList<qint64> times = pile.requestTimesMs();
        int startIndex = -1;
        for (int index = 0; index < requests.size(); ++index) {
            if (quint8(requests.at(index).at(1)) == quint8(0x05)
                && FakeChargePile::requestAddress(requests.at(index)) == kCoilStart) {
                startIndex = index;
                break;
            }
        }
        QVERIFY(startIndex >= 0);
        int firstStatusVoltageIndex = -1;
        int firstStatusFaultIndex = -1;
        int nextStatusVoltageIndex = -1;
        for (int index = startIndex + 1; index < requests.size(); ++index) {
            const QByteArray &request = requests.at(index);
            const bool isInputRead =
                quint8(request.at(1)) == quint8(0x04);
            const quint16 address = FakeChargePile::requestAddress(request);
            const quint16 count = FakeChargePile::requestValue(request);
            if (firstStatusVoltageIndex < 0 && isInputRead
                && address == kRegOutVoltage && count == 2) {
                // Start 后第一轮完整状态组的首帧。
                firstStatusVoltageIndex = index;
            } else if (firstStatusVoltageIndex >= 0
                       && firstStatusFaultIndex < 0 && isInputRead
                       && address == kRegError && count == 1) {
                // 同一轮完整状态组的末帧。轮询间隔从该帧完整响应后开始，
                // 因此不能用两轮 voltage 首帧间距，避免被组内4×70ms节流掩盖。
                firstStatusFaultIndex = index;
            } else if (firstStatusFaultIndex >= 0 && isInputRead
                       && address == kRegOutVoltage && count == 2) {
                // 下一轮完整状态组的首帧。
                nextStatusVoltageIndex = index;
                break;
            }
        }
        QVERIFY(firstStatusVoltageIndex >= 0);
        QVERIFY(firstStatusFaultIndex > firstStatusVoltageIndex);
        QVERIFY(nextStatusVoltageIndex > firstStatusFaultIndex);
        QVERIFY2(times.at(nextStatusVoltageIndex)
                         - times.at(firstStatusFaultIndex) >= 200,
                 "活动会话错误采用了候选轮询间隔");
    }

    void activeChargeRejectsTargetSwitchAndKeepsAllSafetyWritesOnOriginalPile()
    {
        FakeChargePile originalPile;
        FakeChargePile otherPile;
        QVERIFY(originalPile.listen());
        QVERIFY(otherPile.listen());
        originalPile.enableChargeScenario();
        originalPile.setNeverProduceChargingOutput(true);

        ChargePileController controller;
        controller.applySettings(loopbackSettings(originalPile));
        QSignalSpy finishedSpy(&controller, &ChargePileController::chargeSessionFinished);
        QString error;
        QVERIFY(controller.startCharge(ChargePileController::SessionOrigin::Manual, &error));
        QTRY_VERIFY_WITH_TIMEOUT(originalPile.receivedCoil(kCoilStart), 5000);

        controller.applySettings(loopbackSettings(otherPile));
        controller.requestSafeStop(ChargePileController::StopReason::Manual);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 10000);

        QVERIFY(finishedSpy.first().at(0).toBool());
        QCOMPARE(otherPile.requests().size(), 0);
        QCOMPARE(originalPile.coilCount(kCoilStop), 1);
        QCOMPARE(originalPile.coilCount(kCoilRetract), 1);
        QCOMPARE(originalPile.coilOffCount(kCoilRetract), 1);
        QCOMPARE(originalPile.coilCount(kCoilReset), 1);
    }
};

QTEST_GUILESS_MAIN(ChargePileControllerTest)
#include "test_charge_pile_controller.moc"

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

    /** 启动后始终不出现工作、继电器或电流，验证不会把初始 0A 当作自然结束。 */
    void setNeverProduceChargingOutput(const bool enabled) { m_neverProduceChargingOutput = enabled; }

    /** 丢弃指定线圈第一次写回显，模拟命令已经到桩但结果对上位机不确定。 */
    void dropFirstEchoForCoil(const quint16 address) { m_dropEchoCoil = address; }

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

    /** 关闭响应可让真实控制器走到响应超时和断开连接分支。 */
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

private:
    static quint16 requestAddress(const QByteArray &request)
    {
        return (quint16(quint8(request.at(2))) << 8) | quint8(request.at(3));
    }

    static quint16 requestValue(const QByteArray &request)
    {
        return (quint16(quint8(request.at(4))) << 8) | quint8(request.at(5));
    }

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
            m_inputWord = 0x0008;
            m_faultWord = m_faultAfterRetract;
        } else if (address == kCoilReset) {
            m_inputWord = 0x0008;
            m_outputWord = 0;
            m_currentTenths = 0;
        }
    }

    QByteArray responseFor(const QByteArray &request)
    {
        const quint8 function = quint8(request.at(1));
        const quint16 address = requestAddress(request);
        updateScenarioBeforeResponse(request);

        if ((function == 0x05 || function == 0x06)) {
            if (function == 0x05 && address == m_dropEchoCoil && !m_droppedEcho) {
                m_droppedEcho = true;
                return {};
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

    void acceptClient()
    {
        m_client = m_server.nextPendingConnection();
        QVERIFY(m_client != nullptr);
        ++m_connectionCount;
        m_client->setParent(this);
        connect(m_client, &QTcpSocket::readyRead, this, &FakeChargePile::consumeRequests);
        connect(m_client, &QTcpSocket::disconnected, this, [this] { m_client = nullptr; });
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

            const QByteArray response = responseFor(request);
            if (response.isEmpty()) {
                QVERIFY2(m_droppedEcho, "测试夹具收到未定义地址的请求");
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
    int m_chargeStatusReadCount = 0;
    quint16 m_currentAfterStop = 0;
    quint16 m_faultAfterRetract = 0x0080;
    quint16 m_dropEchoCoil = 0xFFFF;
    std::optional<quint16> m_readbackVoltageOverride;
    QHash<quint16, quint16> m_holdingRegisters;
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
        QCOMPARE(pile.coilCount(kCoilReset), 1);
        QVERIFY(controller.snapshot().retracted);
        QVERIFY(!controller.snapshot().extended);
        QVERIFY(!controller.snapshot().working);
        QVERIFY(!controller.snapshot().relayOn);
        QVERIFY(controller.snapshot().outputCurrentA <= 1.0);
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
            QVERIFY(pile.listen());
            pile.enableChargeScenario();
            pile.setFaultAfterRetract(0x0800); // E12 伸出机械臂超时，缩回阶段不得豁免。

            ChargePileController controller;
            controller.applySettings(loopbackSettings(pile));
            QSignalSpy finishedSpy(&controller, &ChargePileController::chargeSessionFinished);
            QString error;
            QVERIFY(controller.startCharge(ChargePileController::SessionOrigin::Manual, &error));
            QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 12000);
            QVERIFY(!finishedSpy.first().at(0).toBool());
            QVERIFY(pile.receivedCoil(kCoilRetract));
            QVERIFY(!pile.receivedCoil(kCoilReset));
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

        const int requestCountBeforeRecovery = pile.requests().size();
        QSignalSpy querySpy(&controller, &ChargePileController::queryFinished);
        controller.queryStatus();
        QTRY_COMPARE_WITH_TIMEOUT(querySpy.count(), 1, 4000);
        for (int index = requestCountBeforeRecovery; index < pile.requests().size(); ++index)
            QCOMPARE(quint8(pile.requests().at(index).at(1)), quint8(0x04));
        QCOMPARE(pile.coilCount(kCoilStart), 1);
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
};

QTEST_GUILESS_MAIN(ChargePileControllerTest)
#include "test_charge_pile_controller.moc"

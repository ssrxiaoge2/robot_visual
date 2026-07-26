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

    /** 关闭响应可让真实控制器走到响应超时和断开连接分支。 */
    void setRespondToRequests(const bool enabled) { m_respondToRequests = enabled; }

private:
    static QByteArray readResponse(const QByteArray &payload)
    {
        QByteArray response;
        response.append(char(1));
        response.append(char(0x04));
        response.append(char(payload.size()));
        response.append(payload);
        const quint16 crc = crc16(QByteArrayView(response));
        response.append(char(crc & 0xFF));
        response.append(char((crc >> 8) & 0xFF));
        return response;
    }

    QByteArray responseFor(const QByteArray &request) const
    {
        const quint16 address = (quint16(quint8(request.at(2))) << 8)
                              | quint8(request.at(3));
        switch (address) {
        case kRegOutVoltage:
            // 584 和 126 分别表示现场协议中按 0.1 缩放的 58.4V、12.6A。
            return readResponse(QByteArray::fromHex("0248007e"));
        case kRegInputSignals:
            return readResponse(QByteArray::fromHex("0004"));
        case kRegOutputSignals:
            return readResponse(QByteArray::fromHex("0210"));
        case kRegEvent:
            return readResponse(QByteArray::fromHex("0020"));
        case kRegError:
            return readResponse(QByteArray::fromHex("0080"));
        default:
            return {};
        }
    }

    void acceptClient()
    {
        m_client = m_server.nextPendingConnection();
        QVERIFY(m_client != nullptr);
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
            QVERIFY2(!response.isEmpty(), "测试夹具收到未定义地址的请求");
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
};

QTEST_GUILESS_MAIN(ChargePileControllerTest)
#include "test_charge_pile_controller.moc"

#include <QtTest/QtTest>

#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>

#include "visionclient.h"

class DelayedInferenceServer : public QObject
{
    Q_OBJECT

public:
    explicit DelayedInferenceServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                socket->setParent(this);
                m_clients.append(socket);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    m_requests[socket] += socket->readAll();
                    if (m_requests.value(socket).contains("\r\n\r\n"))
                        emit requestReady(m_clients.indexOf(socket));
                });
            }
        });
    }

    bool listen()
    {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    quint16 port() const
    {
        return m_server.serverPort();
    }

    int clientCount() const
    {
        return m_clients.size();
    }

    void respond(int index, qint64 frameId, double timestamp,
                 double targetX = 100.0)
    {
        QVERIFY(index >= 0 && index < m_clients.size());
        QTcpSocket *socket = m_clients.at(index);
        if (!socket || socket->state() == QAbstractSocket::UnconnectedState)
            return;

        const QByteArray body = QStringLiteral(
            "{\"frame_id\":%1,\"timestamp\":%2,\"objects\":["
            "{\"offset_mm\":{\"x\":%3,\"y\":0},"
            "\"depth_compensated\":500,\"angle\":0,\"confidence\":0.99}]}")
                                    .arg(frameId)
                                    .arg(timestamp, 0, 'f', 3)
                                    .arg(targetX, 0, 'f', 1)
                                    .toUtf8();
        const QByteArray response =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Connection: close\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        socket->write(response);
        socket->flush();
        socket->disconnectFromHost();
    }

signals:
    void requestReady(int index);

private:
    QTcpServer m_server;
    QList<QPointer<QTcpSocket>> m_clients;
    QHash<QTcpSocket *, QByteArray> m_requests;
};

class VisionClientGenerationTest : public QObject
{
    Q_OBJECT

private slots:
    void obsoleteReplyCannotEnterNextPickupGeneration()
    {
        DelayedInferenceServer server;
        QVERIFY(server.listen());

        VisionHttpClient client;
        client.setServerUrl(QStringLiteral("127.0.0.1"), server.port());
        QSignalSpy rawSpy(
            &client,
            qOverload<double, double, double, double>(
                &VisionHttpClient::rawCoordinatesReady));
        QSignalSpy noObjectSpy(&client, &VisionHttpClient::noObjectDetected);
        QSignalSpy errorSpy(&client, &VisionHttpClient::errorOccurred);

        VisionHttpClient::TargetSelectionContext broad;
        broad.stationRoiHalfX = 500.0;
        broad.stationRoiHalfY = 500.0;
        client.setTargetSelectionContext(broad);
        client.fetchInference();
        QTRY_COMPARE(server.clientCount(), 1);

        client.invalidateInferenceRequests();
        server.respond(0, 101, 1.001);
        QTest::qWait(100);
        QCOMPARE(rawSpy.count(), 0);
        QCOMPARE(noObjectSpy.count(), 0);
        QCOMPARE(errorSpy.count(), 0);
        QCOMPARE(client.lastInferenceFrameId(), qint64(-1));

        // 请求发起后再改上下文；本次响应必须继续使用发起时快照，而不是新任务上下文。
        client.setTargetSelectionContext(broad);
        client.fetchInference();
        QTRY_COMPARE(server.clientCount(), 2);
        VisionHttpClient::TargetSelectionContext narrow = broad;
        narrow.stationRoiHalfX = 1.0;
        client.setTargetSelectionContext(narrow);
        server.respond(1, 102, 1.002, 100.0);

        QTRY_COMPARE(rawSpy.count(), 1);
        QCOMPARE(noObjectSpy.count(), 0);
        QCOMPARE(errorSpy.count(), 0);
        QCOMPARE(client.lastInferenceFrameId(), qint64(102));
        QCOMPARE(client.lastInferenceTimestampMs(), qint64(1002));
    }
};

QTEST_GUILESS_MAIN(VisionClientGenerationTest)

#include "test_visionclient_generation.moc"

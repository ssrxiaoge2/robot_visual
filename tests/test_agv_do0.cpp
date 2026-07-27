#include "agvcontroller.h"

#include <QHostAddress>
#include <QModbusTcpServer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTest>

namespace {

/**
 * @brief 只监听本机回环地址的仙工AGV Modbus模拟端。
 *
 * 测试只模拟DO0需要的三个地址：线圈59对应文档[0x]00060置高命令，
 * 线圈19对应文档[0x]00020置低命令，离散输入59对应[1x]00060实际状态。
 */
class FakeAgvDo0Server final
{
public:
    bool listen()
    {
        QTcpServer portProbe;
        if (!portProbe.listen(QHostAddress::LocalHost, 0))
            return false;
        m_port = portProbe.serverPort();
        portProbe.close();

        QModbusDataUnitMap map;
        map.insert(QModbusDataUnit::Coils,
                   QModbusDataUnit(QModbusDataUnit::Coils, 0, 100));
        map.insert(QModbusDataUnit::DiscreteInputs,
                   QModbusDataUnit(QModbusDataUnit::DiscreteInputs, 0, 100));
        map.insert(QModbusDataUnit::InputRegisters,
                   QModbusDataUnit(QModbusDataUnit::InputRegisters, 0, 150));
        if (!m_server.setMap(map))
            return false;

        m_server.setServerAddress(1);
        m_server.setConnectionParameter(
            QModbusDevice::NetworkAddressParameter,
            QStringLiteral("127.0.0.1"));
        m_server.setConnectionParameter(
            QModbusDevice::NetworkPortParameter,
            m_port);

        QObject::connect(
            &m_server, &QModbusServer::dataWritten,
            &m_server,
            [this](const QModbusDataUnit::RegisterType table,
                   const int address, const int size) {
                Q_UNUSED(size)
                if (table != QModbusDataUnit::Coils)
                    return;
                if (address == 59) {
                    ++m_highCommandCount;
                    if (m_mirrorCommands)
                        m_server.setData(
                            QModbusDataUnit::DiscreteInputs, 59, true);
                } else if (address == 19) {
                    ++m_lowCommandCount;
                    if (m_mirrorCommands)
                        m_server.setData(
                            QModbusDataUnit::DiscreteInputs, 59, false);
                }
            });
        return m_server.connectDevice();
    }

    quint16 port() const { return m_port; }
    QModbusDevice::State state() const { return m_server.state(); }
    int highCommandCount() const { return m_highCommandCount; }
    int lowCommandCount() const { return m_lowCommandCount; }

    void setMirrorCommands(const bool enabled) { m_mirrorCommands = enabled; }
    void setDo0(const bool high)
    {
        QVERIFY(m_server.setData(
            QModbusDataUnit::DiscreteInputs, 59, high));
    }

private:
    QModbusTcpServer m_server;
    quint16 m_port = 0;
    int m_highCommandCount = 0;
    int m_lowCommandCount = 0;
    bool m_mirrorCommands = true;
};

void connectController(AgvController &controller, FakeAgvDo0Server &server)
{
    QVERIFY(server.listen());
    QTRY_COMPARE_WITH_TIMEOUT(
        server.state(), QModbusDevice::ConnectedState, 3000);

    QSignalSpy connectedSpy(&controller, &AgvController::connected);
    controller.connectToHost(QStringLiteral("127.0.0.1"), server.port());
    QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 3000);
}

} // namespace

class AgvDo0Test final : public QObject
{
    Q_OBJECT

private slots:
    void ensureHighAvoidsDuplicateWriteAndThenCloses()
    {
        FakeAgvDo0Server server;
        AgvController controller;
        connectController(controller, server);

        QSignalSpy ensureSpy(
            &controller, &AgvController::do0EnsureFinished);
        QString error;
        QVERIFY2(controller.ensureDo0(true, &error), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(ensureSpy.count(), 1, 5000);
        const QList<QVariant> openResult = ensureSpy.takeFirst();
        QVERIFY(openResult.at(0).toBool());
        QVERIFY(openResult.at(1).toBool());
        QVERIFY(openResult.at(2).toBool());
        QCOMPARE(server.highCommandCount(), 1);

        QVERIFY2(controller.ensureDo0(true, &error), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(ensureSpy.count(), 1, 5000);
        const QList<QVariant> alreadyHighResult = ensureSpy.takeFirst();
        QVERIFY(alreadyHighResult.at(1).toBool());
        QVERIFY(alreadyHighResult.at(2).toBool());
        QCOMPARE(server.highCommandCount(), 1);

        QVERIFY2(controller.ensureDo0(false, &error), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(ensureSpy.count(), 1, 5000);
        const QList<QVariant> closeResult = ensureSpy.takeFirst();
        QVERIFY(!closeResult.at(0).toBool());
        QVERIFY(closeResult.at(1).toBool());
        QVERIFY(!closeResult.at(2).toBool());
        QCOMPARE(server.lowCommandCount(), 1);
    }

    void unchangedStateFailsAfterOneWriteAndThreeConfirmations()
    {
        FakeAgvDo0Server server;
        server.setMirrorCommands(false);
        AgvController controller;
        connectController(controller, server);

        QSignalSpy ensureSpy(
            &controller, &AgvController::do0EnsureFinished);
        QString error;
        QVERIFY2(controller.ensureDo0(true, &error), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(ensureSpy.count(), 1, 6000);
        const QList<QVariant> result = ensureSpy.takeFirst();
        QVERIFY(result.at(0).toBool());
        QVERIFY(!result.at(1).toBool());
        QVERIFY(!result.at(2).toBool());
        QCOMPARE(server.highCommandCount(), 1);
    }

    void queryIsReadOnly()
    {
        FakeAgvDo0Server server;
        AgvController controller;
        connectController(controller, server);
        server.setDo0(true);

        QSignalSpy stateSpy(&controller, &AgvController::do0StateRead);
        QString error;
        QVERIFY2(controller.queryDo0(&error), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(stateSpy.count(), 1, 3000);
        const QList<QVariant> result = stateSpy.takeFirst();
        QVERIFY(result.at(0).toBool());
        QVERIFY(result.at(1).toBool());
        QCOMPARE(server.highCommandCount(), 0);
        QCOMPARE(server.lowCommandCount(), 0);
    }
};

QTEST_MAIN(AgvDo0Test)

#include "test_agv_do0.moc"

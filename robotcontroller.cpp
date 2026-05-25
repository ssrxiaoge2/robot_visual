#include "robotcontroller.h"

#include <QModbusReply>
#include <QVariant>
#include <QDebug>

RobotController::RobotController(QObject *parent)
    : QObject(parent)
{
    m_client = new QModbusTcpClient(this);
    connect(m_client, &QModbusClient::stateChanged, this, &RobotController::onStateChanged);

    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(5000);
    connect(m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (m_client->state() != QModbusDevice::ConnectedState)
            m_client->connectDevice();
    });
}

RobotController::~RobotController()
{
    disconnectFromHost();
}

void RobotController::connectToHost(const QString &ip, int port)
{
    m_ip   = ip;
    m_port = port;
    m_client->setConnectionParameter(QModbusDevice::NetworkAddressParameter, QVariant(ip));
    m_client->setConnectionParameter(QModbusDevice::NetworkPortParameter,    QVariant(port));
    m_client->setTimeout(2000);
    m_client->setNumberOfRetries(2);
    m_client->connectDevice();
}

void RobotController::disconnectFromHost()
{
    m_reconnectTimer->stop();
    m_client->disconnectDevice();
}

bool RobotController::isConnected() const
{
    return m_client->state() == QModbusDevice::ConnectedState;
}

void RobotController::onStateChanged(QModbusDevice::State state)
{
    switch (state) {
    case QModbusDevice::ConnectedState:
        emit connected();
        break;
    case QModbusDevice::UnconnectedState:
        emit disconnected();
        break;
    case QModbusDevice::ConnectingState:
        break;
    }
}

void RobotController::readHoldingRegisters(int startAddr, int count)
{
    if (!isConnected()) return;

    auto *reply = m_client->sendReadRequest(
        QModbusDataUnit(QModbusDataUnit::HoldingRegisters, startAddr, count), 1);
    if (!reply) return;

    connect(reply, &QModbusReply::finished, this, [this, reply, startAddr]() {
        reply->deleteLater();
        if (reply->error() == QModbusDevice::NoError) {
            QList<quint16> vals;
            const auto unit = reply->result();
            for (uint i = 0; i < unit.valueCount(); ++i)
                vals.append(unit.value(i));
            emit registersRead(startAddr, vals);
        } else {
            emit errorOccurred(QString("寄存器读取失败: %1").arg(reply->errorString()));
        }
    });
}

void RobotController::writeRegister(int addr, quint16 value)
{
    if (!isConnected()) return;

    QModbusDataUnit unit(QModbusDataUnit::HoldingRegisters, addr, 1);
    unit.setValue(0, value);

    auto *reply = m_client->sendWriteRequest(unit, 1);
    if (!reply) return;

    connect(reply, &QModbusReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() == QModbusDevice::NoError)
            emit writeCompleted();
        else
            emit errorOccurred(QString("寄存器写入失败: %1").arg(reply->errorString()));
    });
}

void RobotController::writeCoil(int addr, bool on)
{
    if (!isConnected()) return;

    QModbusDataUnit unit(QModbusDataUnit::Coils, addr, 1);
    unit.setValue(0, on ? 1 : 0);

    auto *reply = m_client->sendWriteRequest(unit, 1);
    if (!reply) return;

    connect(reply, &QModbusReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() == QModbusDevice::NoError)
            emit writeCompleted();
        else
            emit errorOccurred(QString("Coil 写入失败: %1").arg(reply->errorString()));
    });
}

void RobotController::readCoils(int startAddr, int count)
{
    if (!isConnected()) return;

    auto *reply = m_client->sendReadRequest(
        QModbusDataUnit(QModbusDataUnit::Coils, startAddr, count), 1);
    if (!reply) return;

    connect(reply, &QModbusReply::finished, this, [this, reply, startAddr]() {
        reply->deleteLater();
        if (reply->error() == QModbusDevice::NoError) {
            QList<bool> vals;
            const auto unit = reply->result();
            for (uint i = 0; i < unit.valueCount(); ++i)
                vals.append(unit.value(i) != 0);
            emit coilsRead(startAddr, vals);
        } else {
            emit errorOccurred(QString("Coil 读取失败: %1").arg(reply->errorString()));
        }
    });
}

void RobotController::writeRegisters(int startAddr, const QList<quint16> &values)
{
    if (!isConnected() || values.isEmpty()) return;

    QModbusDataUnit unit(QModbusDataUnit::HoldingRegisters, startAddr,
                         static_cast<quint16>(values.size()));
    for (int i = 0; i < values.size(); ++i)
        unit.setValue(i, values[i]);

    auto *reply = m_client->sendWriteRequest(unit, 1);
    if (!reply) return;

    connect(reply, &QModbusReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() == QModbusDevice::NoError)
            emit writeCompleted();
        else
            emit errorOccurred(QString("批量写入失败: %1").arg(reply->errorString()));
    });
}

void RobotController::readInputRegisters(int startAddr, int count)
{
    if (!isConnected()) return;

    auto *reply = m_client->sendReadRequest(
        QModbusDataUnit(QModbusDataUnit::InputRegisters, startAddr, count), 1);
    if (!reply) return;

    connect(reply, &QModbusReply::finished, this, [this, reply, startAddr]() {
        reply->deleteLater();
        if (reply->error() == QModbusDevice::NoError) {
            QList<quint16> vals;
            const auto unit = reply->result();
            for (uint i = 0; i < unit.valueCount(); ++i)
                vals.append(unit.value(i));
            emit inputRegistersRead(startAddr, vals);
        } else {
            emit errorOccurred(QString("Input 寄存器读取失败: %1").arg(reply->errorString()));
        }
    });
}

void RobotController::onReadReady()
{
}

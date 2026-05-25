#include "agvcontroller.h"

#include <QModbusReply>
#include <QVariant>

AgvController::AgvController(QObject *parent)
    : QObject(parent)
{
    m_client = new QModbusTcpClient(this);
    connect(m_client, &QModbusClient::stateChanged,
            this, &AgvController::onStateChanged);

    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(5000);
    connect(m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (!m_ip.isEmpty() &&
            m_client->state() == QModbusDevice::UnconnectedState)
            m_client->connectDevice();
    });
}

AgvController::~AgvController()
{
    disconnectFromHost();
}

// ── 连接管理 ────────────────────────────────────────────────

void AgvController::connectToHost(const QString &ip, int port)
{
    m_ip   = ip;
    m_port = port;
    m_client->setConnectionParameter(QModbusDevice::NetworkAddressParameter, QVariant(ip));
    m_client->setConnectionParameter(QModbusDevice::NetworkPortParameter,    QVariant(port));
    m_client->setTimeout(2000);
    m_client->setNumberOfRetries(2);
    m_client->connectDevice();
    m_reconnectTimer->start();
}

void AgvController::disconnectFromHost()
{
    m_ip.clear();
    m_reconnectTimer->stop();
    m_client->disconnectDevice();
}

bool AgvController::isConnected() const
{
    return m_client->state() == QModbusDevice::ConnectedState;
}

// ── AGV 控制接口 ────────────────────────────────────────────

void AgvController::sendToStation(int stationNo)
{
    if (!isConnected()) return;

    QModbusDataUnit unit(QModbusDataUnit::HoldingRegisters, REG_TARGET_STATION, 1);
    unit.setValue(0, static_cast<quint16>(stationNo));

    auto *reply = m_client->sendWriteRequest(unit, 1);
    if (!reply) return;

    connect(reply, &QModbusReply::finished, this, [this, reply, stationNo]() {
        reply->deleteLater();
        if (reply->error() != QModbusDevice::NoError) {
            emit errorOccurred(QString("AGV 工位写入失败 (站=%1): %2")
                               .arg(stationNo).arg(reply->errorString()));
        }
    });
}

void AgvController::readStatusRegister()
{
    if (!isConnected()) return;

    auto *reply = m_client->sendReadRequest(
        QModbusDataUnit(QModbusDataUnit::InputRegisters, REG_STATUS, 1), 1);
    if (!reply) return;

    connect(reply, &QModbusReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() == QModbusDevice::NoError) {
            const quint16 raw = reply->result().value(0);
            auto st = static_cast<AgvStatus>(raw);
            emit statusRead(st);
            if (st == AgvStatus::Arrived)
                emit arrived();
        } else {
            emit errorOccurred(QString("AGV 状态读取失败: %1").arg(reply->errorString()));
        }
    });
}

// ── 内部槽 ──────────────────────────────────────────────────

void AgvController::onStateChanged(QModbusDevice::State state)
{
    switch (state) {
    case QModbusDevice::ConnectedState:
        m_reconnectTimer->stop();
        emit connected();
        break;
    case QModbusDevice::UnconnectedState:
        emit disconnected();
        if (!m_ip.isEmpty())            // 曾主动连接过 → 开启重连
            m_reconnectTimer->start();
        break;
    default:
        break;
    }
}

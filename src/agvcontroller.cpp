/**
 * @file agvcontroller.cpp
 * @brief AgvController 仙工 AGV Modbus TCP 客户端实现
 *
 * 与 RobotController 结构对称，但寄存器类型不同：
 *   写目标工位：FC16 写 Holding 1000
 *   读 AGV 状态：FC04 读 Input 1001
 *
 * 自动重连逻辑与 RobotController 相同：
 *   连接成功 → 停止重连定时器
 *   连接断开 → 若 IP 非空则 5s 后重试
 */

#include "agvcontroller.h"

#include <QModbusReply>
#include <QVariant>

// ── 构造 / 析构 ───────────────────────────────────────────────

AgvController::AgvController(QObject *parent)
    : QObject(parent)
{
    m_client = new QModbusTcpClient(this);
    connect(m_client, &QModbusClient::stateChanged,
            this, &AgvController::onStateChanged);

    // 5s 重连定时器：仅在曾经主动连接（m_ip 非空）且当前未连接时触发重连
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

// ── 连接管理 ─────────────────────────────────────────────────

void AgvController::connectToHost(const QString &ip, int port)
{
    m_ip   = ip;
    m_port = port;
    m_client->setConnectionParameter(QModbusDevice::NetworkAddressParameter, QVariant(ip));
    m_client->setConnectionParameter(QModbusDevice::NetworkPortParameter,    QVariant(port));
    m_client->setTimeout(2000);      // 请求超时 2s
    m_client->setNumberOfRetries(2); // 超时重试 2 次
    m_client->connectDevice();
    m_reconnectTimer->start(); // 连接发起后启动重连定时器（连接成功后会自动停止）
}

void AgvController::disconnectFromHost()
{
    m_ip.clear();              // 清空 IP，防止重连定时器在主动断开后继续重连
    m_reconnectTimer->stop();
    m_client->disconnectDevice();
}

bool AgvController::isConnected() const
{
    return m_client->state() == QModbusDevice::ConnectedState;
}

// ── AGV 控制接口 ─────────────────────────────────────────────

/**
 * @brief 向 AGV 写入目标工位编号，触发 AGV 自动行走
 * @param stationNo 目标工位编号（1-12），写入 AGV Holding 1000
 *
 * 写入成功后 AGV 会自动开始行走，上层通过 readStatusRegister()
 * 轮询 Input 1001 直到状态变为 Arrived(2) 再进入下一步骤。
 */
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
        // 写入成功时不发信号，上层通过轮询 readStatusRegister() 跟踪行走状态
    });
}

/**
 * @brief 请求读取 AGV 当前状态（FC04 读 Input 1001）
 *
 * 结果通过 statusRead(AgvStatus) 信号异步返回。
 * 若状态为 Arrived，同时额外 emit arrived() 快捷信号。
 * WorkflowEngine 在 Step2 中以 300ms 间隔调用此方法轮询。
 */
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
                emit arrived(); // 到位快捷信号（可选连接，WorkflowEngine 用 statusRead）
        } else {
            emit errorOccurred(QString("AGV 状态读取失败: %1").arg(reply->errorString()));
        }
    });
}

// ── 内部槽 ───────────────────────────────────────────────────

void AgvController::onStateChanged(QModbusDevice::State state)
{
    switch (state) {
    case QModbusDevice::ConnectedState:
        m_reconnectTimer->stop(); // 连接成功，停止重连定时器
        emit connected();
        break;
    case QModbusDevice::UnconnectedState:
        emit disconnected();
        if (!m_ip.isEmpty())     // 若是主动连接断开，则尝试重连
            m_reconnectTimer->start();
        break;
    default:
        break;
    }
}

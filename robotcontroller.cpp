/**
 * @file robotcontroller.cpp
 * @brief RobotController 华沿机械臂 Modbus TCP 客户端实现
 *
 * Modbus 功能码对应关系：
 *   FC03 readHoldingRegisters  — 读 Holding Register（可读写寄存器，如 900~906）
 *   FC04 readInputRegisters    — 读 Input Register（只读寄存器，如状态字 1132）
 *   FC06 writeRegister         — 写单个 Holding Register
 *   FC16 writeRegisters        — 写多个连续 Holding Register（批量写坐标）
 *   FC01 readCoils / FC05 writeCoil — 线圈操作（预留，当前未使用）
 *
 * 所有 Modbus 操作均为异步：sendXxxRequest() 返回 QModbusReply*，
 * 通过 reply->finished 信号回调，结果通过 registersRead / writeCompleted 等信号返回。
 *
 * 自动重连：设备断线后，m_reconnectTimer 每 5s 尝试重连一次。
 */

#include "robotcontroller.h"

#include <QModbusReply>
#include <QVariant>
#include <QDebug>

// ── 构造 / 析构 ───────────────────────────────────────────────

RobotController::RobotController(QObject *parent)
    : QObject(parent)
{
    // QModbusTcpClient 是 Qt SerialBus 模块提供的 Modbus TCP 主站实现
    m_client = new QModbusTcpClient(this);
    connect(m_client, &QModbusClient::stateChanged,
            this, &RobotController::onStateChanged);

    // 断线重连定时器：每 5s 检查一次，若仍未连接则重试
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(5000);
    connect(m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (m_client->state() != QModbusDevice::ConnectedState)
            m_client->connectDevice();
    });
}

RobotController::~RobotController()
{
    disconnectFromHost(); // 确保析构时正常断开连接
}

// ── 连接管理 ─────────────────────────────────────────────────

void RobotController::connectToHost(const QString &ip, int port)
{
    m_ip   = ip;
    m_port = port;
    // 设置连接参数（必须在 connectDevice() 之前调用）
    m_client->setConnectionParameter(QModbusDevice::NetworkAddressParameter, QVariant(ip));
    m_client->setConnectionParameter(QModbusDevice::NetworkPortParameter,    QVariant(port));
    m_client->setTimeout(2000);     // 单次请求超时 2s
    m_client->setNumberOfRetries(2); // 超时后最多重试 2 次
    m_client->connectDevice();
}

void RobotController::disconnectFromHost()
{
    m_reconnectTimer->stop();   // 停止重连定时器，防止断开后继续重连
    m_client->disconnectDevice();
}

bool RobotController::isConnected() const
{
    return m_client->state() == QModbusDevice::ConnectedState;
}

// ── 状态变化处理 ─────────────────────────────────────────────

void RobotController::onStateChanged(QModbusDevice::State state)
{
    switch (state) {
    case QModbusDevice::ConnectedState:
        // 连接成功：停止重连定时器，通知上层
        m_reconnectTimer->stop();
        emit connected();
        break;
    case QModbusDevice::UnconnectedState:
        // 连接断开：通知上层，若 IP 非空则启动重连
        emit disconnected();
        if (!m_ip.isEmpty())
            m_reconnectTimer->start();
        break;
    case QModbusDevice::ConnectingState:
        // 连接中：不需要额外处理，等待状态变化
        break;
    default:
        break;
    }
}

// ── Modbus 读操作 ─────────────────────────────────────────────

/**
 * @brief 读取 Holding Register（FC03）
 *
 * 结果通过 registersRead(startAddr, values) 信号异步返回。
 * 典型用途：读取命令字（900）、坐标寄存器（901-906）。
 */
void RobotController::readHoldingRegisters(int startAddr, int count)
{
    if (!isConnected()) return;

    auto *reply = m_client->sendReadRequest(
        QModbusDataUnit(QModbusDataUnit::HoldingRegisters, startAddr, count), 1);
    if (!reply) return;

    connect(reply, &QModbusReply::finished, this, [this, reply, startAddr]() {
        reply->deleteLater(); // 回调完成后释放 reply 对象
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

/**
 * @brief 读取 Input Register（FC04）
 *
 * Input Register 是只读寄存器，机器人用其上报状态字（Input 1132）。
 * 结果通过 inputRegistersRead(startAddr, values) 信号异步返回。
 * 与 registersRead 信号分开，避免地址相同时的歧义。
 */
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

// ── Modbus 写操作 ─────────────────────────────────────────────

/**
 * @brief 写单个 Holding Register（FC06）
 *
 * 用于写命令字（Holding 900）和翻转/复位指令。
 * 写完后通过 writeCompleted() 信号通知。
 */
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

/**
 * @brief 批量写连续 Holding Register（FC16）
 *
 * 用于一次性写入 6 个坐标寄存器（Holding 901-906）。
 * ⚠ 写坐标后必须等待 writeCompleted() 回调，再写命令字 900，
 *   防止机器人读到尚未更新完成的坐标。
 *
 * @param startAddr 起始地址（901）
 * @param values    寄存器值列表，长度必须与寄存器数量一致（6个）
 */
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

/**
 * @brief 写单个线圈（FC05）
 * @param addr  线圈地址
 * @param on    true=置位，false=复位
 */
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

/**
 * @brief 读取线圈（FC01）
 *
 * 结果通过 coilsRead(startAddr, values) 信号返回。
 */
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

// ── 预留槽（QModbusClient::readyRead，当前不使用）────────────
void RobotController::onReadReady() {}

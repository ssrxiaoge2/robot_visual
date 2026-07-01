/**
 * @file agvcontroller.cpp
 * @brief AgvController 仙工 AGV Modbus TCP 客户端实现
 *
 * 寄存器交互模式为"自清零触发式"：
 *   下发站点：FC06 写 [4x]00001，AGV 收到后自动清 0 并开始路径导航
 *   状态轮询：FC04 读 [3x]00007-00009（导航站点 / 定位状态 / 导航状态）
 *   取消导航：FC05 写 [0x]00006=1
 *
 * 文档地址位以 00001 为起始，请求时经 pdu() 统一减 1。
 *
 * 自动重连：
 *   连接成功 → 停止重连定时器
 *   连接断开 → 若 IP 非空则 5s 后重试
 */

#include "agvcontroller.h"

#include <QMetaType>
#include <QModbusReply>
#include <QVariant>

// ── 构造 / 析构 ───────────────────────────────────────────────

AgvController::AgvController(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<AgvMonitorData>();

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

    m_monitorTimer = new QTimer(this);
    m_monitorTimer->setInterval(1000);
    connect(m_monitorTimer, &QTimer::timeout, this, &AgvController::pollMonitor);
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
 * @brief 写目标站点 id 到 [4x]00001，触发 AGV 路径导航
 * @param stationNo 目标站点 id（须 >0，且地图中存在对应数字编号的站点）
 *
 * AGV 收到后会将该寄存器自动清 0 并开始导航，上层通过
 * readStatusRegister() 轮询导航状态直到 Arrived(4) 再进入下一步骤。
 */
void AgvController::sendToStation(int stationNo)
{
    if (!isConnected()) {
        emit errorOccurred(QStringLiteral("AGV 未连接，无法发送导航到站点 %1").arg(stationNo));
        return;
    }

    m_targetStation = stationNo;

    QModbusDataUnit unit(QModbusDataUnit::HoldingRegisters, pdu(REG_TARGET_STATION), 1);
    unit.setValue(0, static_cast<quint16>(stationNo));

    auto *reply = m_client->sendWriteRequest(unit, 1);
    if (!reply) return;

    connect(reply, &QModbusReply::finished, this, [this, reply, stationNo]() {
        reply->deleteLater();
        if (reply->error() != QModbusDevice::NoError) {
            emit errorOccurred(QString("AGV 站点写入失败 (站=%1): %2")
                               .arg(stationNo).arg(reply->errorString()));
        }
        // 写入成功时不发信号，上层通过轮询 readStatusRegister() 跟踪导航状态
    });
}

/**
 * @brief 请求读取 AGV 状态（FC04 读 [3x]00007-00009）
 *
 * 一次读取 3 个寄存器：当前导航站点 / 定位状态 / 导航状态。
 * 结果通过 statusRead(NavStatus, navStation) 信号异步返回。
 * 若导航状态为 Arrived 且导航站点与本次下发的目标一致，额外 emit arrived()。
 */
void AgvController::readStatusRegister()
{
    if (!isConnected()) return;

    auto *reply = m_client->sendReadRequest(
        QModbusDataUnit(QModbusDataUnit::InputRegisters, pdu(REG_NAV_STATION), 3), 1);
    if (!reply) return;

    connect(reply, &QModbusReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() == QModbusDevice::NoError) {
            const int  navStation = static_cast<qint16>(reply->result().value(0));
            const auto status     = static_cast<NavStatus>(reply->result().value(2));
            emit statusRead(status, navStation);
            if (status == NavStatus::Arrived && navStation == m_targetStation)
                emit arrived();
        } else {
            emit errorOccurred(QString("AGV 状态读取失败: %1").arg(reply->errorString()));
        }
    });
}

/**
 * @brief 取消当前导航（FC05 写 [0x]00006=1，AGV 收到后自动清 0）
 */
void AgvController::cancelNavigation()
{
    if (!isConnected()) return;

    QModbusDataUnit unit(QModbusDataUnit::Coils, pdu(COIL_CANCEL_NAV), 1);
    unit.setValue(0, 1);

    auto *reply = m_client->sendWriteRequest(unit, 1);
    if (!reply) return;

    connect(reply, &QModbusReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QModbusDevice::NoError)
            emit errorOccurred(QString("AGV 取消导航失败: %1").arg(reply->errorString()));
    });
}

void AgvController::pauseNavigation()
{
    if (!isConnected()) return;
    QModbusDataUnit unit(QModbusDataUnit::Coils, pdu(COIL_PAUSE_NAV), 1);
    unit.setValue(0, 1);
    auto *reply = m_client->sendWriteRequest(unit, 1);
    if (!reply) return;
    connect(reply, &QModbusReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QModbusDevice::NoError)
            emit errorOccurred(QString("AGV 暂停导航失败: %1").arg(reply->errorString()));
    });
}

void AgvController::resumeNavigation()
{
    if (!isConnected()) return;
    QModbusDataUnit unit(QModbusDataUnit::Coils, pdu(COIL_RESUME_NAV), 1);
    unit.setValue(0, 1);
    auto *reply = m_client->sendWriteRequest(unit, 1);
    if (!reply) return;
    connect(reply, &QModbusReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QModbusDevice::NoError)
            emit errorOccurred(QString("AGV 继续导航失败: %1").arg(reply->errorString()));
    });
}

void AgvController::startMonitor(int intervalMs)
{
    m_monitorTimer->setInterval(intervalMs);
    m_monitorTimer->start();
}

void AgvController::stopMonitor()
{
    m_monitorTimer->stop();
    m_monitorBusy = false; // 防止断线重连后因标志残留导致轮询永久卡死
}

/**
 * @brief 监控轮询：两次 FC04 读取后发布快照
 *
 * 第一读 [3x]00007-00013（7 个寄存器，覆盖导航站点/定位/导航状态/电量），
 * 第二读 [3x]00034-00043（10 个寄存器，首尾分别是当前站点和控制权）。
 * 任一读失败则跳过本轮（下轮重试），不刷错误日志避免轮询噪音。
 */
void AgvController::pollMonitor()
{
    if (!isConnected() || m_monitorBusy) return; // 防止上一轮读取未完成时叠发请求
    m_monitorBusy = true;

    auto *r1 = m_client->sendReadRequest(
        QModbusDataUnit(QModbusDataUnit::InputRegisters, pdu(REG_NAV_STATION), 7), 1);
    if (!r1) {
        m_monitorBusy = false;
        return;
    }
    connect(r1, &QModbusReply::finished, this, [this, r1]() {
        r1->deleteLater();
        if (r1->error() != QModbusDevice::NoError) {
            m_monitorBusy = false;
            return;
        }
        const QModbusDataUnit u = r1->result();
        m_monitorData.navStation = static_cast<qint16>(u.value(0));
        m_monitorData.locStatus  = u.value(1);
        m_monitorData.navStatus  = u.value(2);
        m_monitorData.battery    = u.value(6);

        auto *r2 = m_client->sendReadRequest(
            QModbusDataUnit(QModbusDataUnit::InputRegisters, pdu(REG_CUR_STATION), 10), 1);
        if (!r2) {
            m_monitorBusy = false;
            return;
        }
        connect(r2, &QModbusReply::finished, this, [this, r2]() {
            r2->deleteLater();
            if (r2->error() == QModbusDevice::NoError) {
                m_monitorData.curStation = static_cast<qint16>(r2->result().value(0));
                m_monitorData.ctrlSeized = (r2->result().value(9) == 1);
                emit monitorUpdated(m_monitorData);
            }
            m_monitorBusy = false;
        });
    });
}

/**
 * @brief 读取控制权状态（FC04 读 [3x]00043）
 *
 * 连接成功后自动调用一次，供联调时确认 Modbus 指令是否会被
 * 外部调度系统抢占而失效。
 */
void AgvController::readControlOwnership()
{
    if (!isConnected()) return;

    auto *reply = m_client->sendReadRequest(
        QModbusDataUnit(QModbusDataUnit::InputRegisters, pdu(REG_CTRL_SEIZED), 1), 1);
    if (!reply) return;

    connect(reply, &QModbusReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() == QModbusDevice::NoError)
            emit controlOwnershipRead(reply->result().value(0) == 1);
        else
            emit errorOccurred(QString("AGV 控制权读取失败: %1").arg(reply->errorString()));
    });
}

// ── 内部槽 ───────────────────────────────────────────────────

void AgvController::onStateChanged(QModbusDevice::State state)
{
    switch (state) {
    case QModbusDevice::ConnectedState:
        m_reconnectTimer->stop(); // 连接成功，停止重连定时器
        emit connected();
        readControlOwnership();   // 联调自检：确认控制权未被外部抢占
        startMonitor();
        break;
    case QModbusDevice::UnconnectedState:
        stopMonitor();
        emit disconnected();
        if (!m_ip.isEmpty())     // 若是主动连接断开，则尝试重连
            m_reconnectTimer->start();
        break;
    default:
        break;
    }
}

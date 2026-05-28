/**
 * @file devicemanager.cpp
 * @brief DeviceManager 设备管理器实现
 *
 * 职责划分：
 *   ① 生命周期管理：创建并持有 RobotController / AgvController / VisionHttpClient，
 *      在 applyConfig() 时重新连接 Modbus 并更新视觉服务 URL
 *   ② 连通性测试：tcpPing() 同步探测 Modbus 设备；
 *                  VisionHttpClient::checkStatus() 异步检测视觉服务
 *   ③ 补光灯控制：Linux 环境调用外部 fill_light 程序控制 GPIO；
 *                  Windows 环境模拟切换（不访问硬件）
 *   ④ 信号透传：将子控制器的连接状态/错误信号
 *               转发为 DeviceManager 自己的信号，供 MainWindow 订阅
 *
 * 注意：tcpPing() 是同步阻塞调用，会在当前线程等待最多 timeoutMs（默认2s）。
 * testCamera() 已改为异步方式（通过 VisionHttpClient::checkStatus()），
 * 结果通过 cameraStatusChanged 信号异步通知 UI。
 */

#include "devicemanager.h"
#include "robotcontroller.h"
#include "agvcontroller.h"
#include "visionclient.h"

#include <QTcpSocket>
#ifdef Q_OS_LINUX
#  include <QProcess>  // fill_light GPIO 控制（仅 Linux 编译）
#endif

// ── 构造 ─────────────────────────────────────────────────────

DeviceManager::DeviceManager(QObject *parent)
    : QObject(parent)
{
    // ── 机械臂控制器（Modbus TCP 主站，华沿机器人）────────────
    m_robotCtrl = new RobotController(this);
    // 透传连接状态信号：MainWindow 订阅 DeviceManager 的信号，不直接接触 RobotController
    connect(m_robotCtrl, &RobotController::connected,
            this, &DeviceManager::robotModbusConnected);
    connect(m_robotCtrl, &RobotController::disconnected,
            this, &DeviceManager::robotModbusDisconnected);
    connect(m_robotCtrl, &RobotController::errorOccurred,
            this, &DeviceManager::robotModbusError);
    connect(m_robotCtrl, &RobotController::registersRead,
            this, &DeviceManager::debugRegistersRead);

    // ── AGV 控制器（Modbus TCP 主站，仙工 AGV）───────────────
    m_agvCtrl = new AgvController(this);
    connect(m_agvCtrl, &AgvController::connected,
            this, &DeviceManager::agvModbusConnected);
    connect(m_agvCtrl, &AgvController::disconnected,
            this, &DeviceManager::agvModbusDisconnected);
    connect(m_agvCtrl, &AgvController::errorOccurred,
            this, &DeviceManager::agvModbusError);

    // ── 视觉服务客户端（HTTP，Flask 8080）────────────────────
    m_visionClient = new VisionHttpClient(this);
    // 将视觉服务连通性结果透传为 cameraStatusChanged 信号
    // testCamera() 调用 checkStatus()，结果由此链路异步回到 UI
    connect(m_visionClient, &VisionHttpClient::statusChanged,
            this, [this](bool ok, const QString &msg) {
        emit logMessage(QString("[视觉] %1").arg(msg));
        emit cameraStatusChanged(ok, msg);
    });
}

// ── 配置应用 ─────────────────────────────────────────────────

/**
 * @brief 应用当前配置并重新连接所有设备
 *
 * 先断开现有 Modbus 连接再重建，同时更新视觉服务地址。
 * 调用后各设备会异步尝试连接，连接结果通过信号通知。
 */
void DeviceManager::applyConfig()
{
    if (m_cfg.robotIP.isEmpty() || m_cfg.agvIP.isEmpty()) {
        emit logMessage(QStringLiteral("[配置] 机器人/AGV IP 不能为空"));
        return;
    }

    emit logMessage(QString("配置已更新 → 机器人 %1:%2  AGV %3:%4  视觉 %5:%6")
                        .arg(m_cfg.robotIP).arg(m_cfg.robotPort)
                        .arg(m_cfg.agvIP).arg(m_cfg.agvPort)
                        .arg(m_cfg.cameraIP).arg(m_cfg.cameraPort));
    emit configApplied(m_cfg.robotIP, m_cfg.robotPort, m_cfg.agvIP);

    // 重连机械臂 Modbus（先断开，再用新参数连接）
    m_robotCtrl->disconnectFromHost();
    m_robotCtrl->connectToHost(m_cfg.robotIP, m_cfg.robotPort);

    // 重连 AGV Modbus
    m_agvCtrl->disconnectFromHost();
    m_agvCtrl->connectToHost(m_cfg.agvIP, m_cfg.agvPort);

    // 更新视觉服务 URL（不需要重连，HTTP 是无状态协议）
    if (!m_cfg.cameraIP.isEmpty())
        m_visionClient->setServerUrl(m_cfg.cameraIP, m_cfg.cameraPort);
}

// ── TCP 连通性测试 ────────────────────────────────────────────

/**
 * @brief 测试机械臂 TCP 连通性（同步，阻塞约 2s）
 *
 * 仅用于手动点击"测试"按钮的场景，不用于自动运行中。
 * 结果通过 robotStatusChanged(bool, QString) 信号通知 UI。
 */
void DeviceManager::testRobot()
{
    if (m_cfg.robotIP.isEmpty()) {
        emit logMessage(QStringLiteral("[测试] 机器人 IP 为空"));
        return;
    }
    emit logMessage(QString("[测试] 正在连接机器人 %1:%2 ...")
                        .arg(m_cfg.robotIP).arg(m_cfg.robotPort));
    emit robotStatusChanged(false, QStringLiteral("测试中..."));

    if (tcpPing(m_cfg.robotIP, m_cfg.robotPort)) {
        emit logMessage(QString("[测试] 机器人 %1:%2 连接成功 ✓")
                            .arg(m_cfg.robotIP).arg(m_cfg.robotPort));
        emit robotStatusChanged(true, QString("可达 %1").arg(m_cfg.robotIP));
    } else {
        emit logMessage(QString("[测试] 机器人 %1:%2 连接失败 ✗")
                            .arg(m_cfg.robotIP).arg(m_cfg.robotPort));
        emit robotStatusChanged(false, QStringLiteral("不可达"));
    }
}

void DeviceManager::testAgv()
{
    if (m_cfg.agvIP.isEmpty()) {
        emit logMessage(QStringLiteral("[测试] AGV IP 为空"));
        return;
    }
    emit logMessage(QString("[测试] 正在连接 AGV %1:%2 ...")
                        .arg(m_cfg.agvIP).arg(m_cfg.agvPort));
    emit agvStatusChanged(false, QStringLiteral("测试中..."));

    if (tcpPing(m_cfg.agvIP, m_cfg.agvPort)) {
        emit logMessage(QString("[测试] AGV %1:%2 连接成功 ✓")
                            .arg(m_cfg.agvIP).arg(m_cfg.agvPort));
        emit agvStatusChanged(true, QString("可达 %1").arg(m_cfg.agvIP));
    } else {
        emit logMessage(QString("[测试] AGV %1:%2 连接失败 ✗")
                            .arg(m_cfg.agvIP).arg(m_cfg.agvPort));
        emit agvStatusChanged(false, QStringLiteral("不可达"));
    }
}

/**
 * @brief 检测视觉服务（Flask HTTP）连通性（异步）
 *
 * 替换原来的同步 tcpPing（奥比相机 8090 端口）。
 * 改为调用 VisionHttpClient::checkStatus()（GET /status），
 * 结果由构造函数中已连接的 statusChanged 信号异步传回 UI。
 */
void DeviceManager::testCamera()
{
    if (m_cfg.cameraIP.isEmpty()) {
        emit logMessage(QStringLiteral("[视觉] 相机 IP 为空"));
        return;
    }
    emit logMessage(QString("[视觉] 正在检测服务 %1:%2 ...")
                        .arg(m_cfg.cameraIP).arg(m_cfg.cameraPort));
    emit cameraStatusChanged(false, QStringLiteral("检测中..."));

    // 先更新 URL，再发起检测
    m_visionClient->setServerUrl(m_cfg.cameraIP, m_cfg.cameraPort);
    m_visionClient->checkStatus(); // 异步，结果通过 statusChanged → cameraStatusChanged
}

void DeviceManager::testScanner()
{
    if (m_cfg.scannerIP.isEmpty()) {
        emit logMessage(QStringLiteral("[扫码器] IP 为空"));
        return;
    }
    emit logMessage(QString("[扫码器] 正在连接 %1:8080 ...").arg(m_cfg.scannerIP));
    emit scannerStatusChanged(false, QStringLiteral("连接中..."));

    if (tcpPing(m_cfg.scannerIP, 8080)) {
        emit logMessage(QString("[扫码器] %1:8080 端口可达 ✓").arg(m_cfg.scannerIP));
        emit scannerStatusChanged(true, QString("可达 %1").arg(m_cfg.scannerIP));
    } else {
        emit logMessage(QString("[扫码器] %1:8080 连接失败 ✗").arg(m_cfg.scannerIP));
        emit scannerStatusChanged(false, QStringLiteral("不可达"));
    }
}

// ── 补光灯控制 ────────────────────────────────────────────────

/**
 * @brief 切换补光灯开/关状态
 *
 * Linux 部署环境：调用 PROJECT_SOURCE_DIR/tools/fill_light on|off 控制 GPIO 9554
 * Windows 开发环境：仅模拟切换，发出日志提示，不访问硬件
 *
 * 结果通过 lightChanged(bool on, bool success) 信号通知 UI。
 */
void DeviceManager::toggleLight()
{
    m_lightOn = !m_lightOn; // 先翻转目标状态，再执行

#ifdef Q_OS_LINUX
    // Linux 专属：调用外部 GPIO 控制程序
    const QString lightBin = QStringLiteral(PROJECT_SOURCE_DIR "/tools/fill_light");
    const QString arg      = m_lightOn ? "on" : "off";

    QProcess proc;
    proc.start(lightBin, QStringList() << arg);
    const bool success = proc.waitForFinished(3000) && (proc.exitCode() == 0);

    if (!success) {
        emit logMessage(QString("[补光灯] %1失败 (exit=%2)")
                            .arg(m_lightOn ? "开启" : "关闭")
                            .arg(proc.exitCode()));
    } else {
        emit logMessage(QString("补光灯 → %1").arg(m_lightOn ? "开启" : "关闭"));
    }
    emit lightChanged(m_lightOn, success);
#else
    // 非 Linux 平台（Windows 开发环境）：模拟切换
    emit logMessage(QString("[补光灯] 模拟%1（当前平台不支持 GPIO 控制）")
                        .arg(m_lightOn ? "开启" : "关闭"));
    emit lightChanged(m_lightOn, true); // success=true 表示"切换"成功（虽然只是模拟）
#endif
}

// ── 机械臂调试接口 ────────────────────────────────────────────

void DeviceManager::debugReadRobotRegisters(int addr, int count)
{
    m_robotCtrl->readHoldingRegisters(addr, count);
}

void DeviceManager::debugWriteRobotRegister(int addr, quint16 value)
{
    emit logMessage(QString("[机械臂] 写入寄存器 %1 = %2").arg(addr).arg(value));
    m_robotCtrl->writeRegister(addr, value);
}

// ── 手眼矩阵动态加载 ──────────────────────────────────────────

void DeviceManager::applyHandEyeMatrix(const float m[16])
{
    m_visionClient->setHandEyeMatrix(m);
    emit logMessage(QStringLiteral("[手眼] 手眼矩阵已更新，新矩阵将立即生效"));
    emit handEyeMatrixApplied();
}

// ── 内部工具 ─────────────────────────────────────────────────

/**
 * @brief 同步 TCP 连通性探测
 * @param ip         目标 IP 地址
 * @param port       目标端口
 * @param timeoutMs  最长等待时间（默认 2000ms）
 * @return true=TCP 握手成功（端口可达），false=连接失败或超时
 *
 * ⚠ 此函数会阻塞调用线程最多 timeoutMs 毫秒。
 * 仅用于用户手动点击"测试"按钮的场景（robot/AGV）。
 * 视觉服务改用异步 HTTP 检测，不再使用此函数。
 */
bool DeviceManager::tcpPing(const QString &ip, int port, int timeoutMs)
{
    QTcpSocket sock;
    sock.connectToHost(ip, static_cast<quint16>(port));
    const bool ok = sock.waitForConnected(timeoutMs); // 阻塞等待
    if (ok) sock.disconnectFromHost();                // 握手成功后立即断开（只测试可达性）
    return ok;
}

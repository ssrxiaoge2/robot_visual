#include "devicemanager.h"
#include "robotcontroller.h"
#include "agvcontroller.h"
#include "visionclient.h"
#include "huayanScheduler.h"

#include <QTcpSocket>
#ifdef Q_OS_LINUX
#  include <QProcess>  // fill_light GPIO 控制（仅 Linux 编译）
#endif

DeviceManager::DeviceManager(QObject *parent)
    : QObject(parent)
{
    m_robotCtrl = new RobotController(this);
    connect(m_robotCtrl, &RobotController::connected,
            this, &DeviceManager::robotModbusConnected);
    connect(m_robotCtrl, &RobotController::disconnected,
            this, &DeviceManager::robotModbusDisconnected);
    connect(m_robotCtrl, &RobotController::errorOccurred,
            this, &DeviceManager::robotModbusError);
    connect(m_robotCtrl, &RobotController::registersRead,
            this, &DeviceManager::debugRegistersRead);

    m_agvCtrl = new AgvController(this);
    connect(m_agvCtrl, &AgvController::connected,
            this, &DeviceManager::agvModbusConnected);
    connect(m_agvCtrl, &AgvController::disconnected,
            this, &DeviceManager::agvModbusDisconnected);
    connect(m_agvCtrl, &AgvController::errorOccurred,
            this, &DeviceManager::agvModbusError);

    m_visionClient = new VisionHttpClient(this);
    connect(m_visionClient, &VisionHttpClient::statusChanged,
            this, [this](bool ok, const QString &msg) {
        emit logMessage(QString("[视觉] %1").arg(msg));
        emit cameraStatusChanged(ok, msg);
    });

    m_huayanScheduler = new HuayanScheduler(this);

    connect(m_huayanScheduler, &HuayanScheduler::surveyReady,
            m_visionClient,    &VisionHttpClient::fetchInference);
    connect(m_visionClient,    &VisionHttpClient::rawCoordinatesReady,
            m_huayanScheduler, &HuayanScheduler::setGrabOffset);

    connect(m_huayanScheduler, &HuayanScheduler::logMessage,
            this, &DeviceManager::logMessage);
    connect(m_huayanScheduler, &HuayanScheduler::stageError,
            this, [this](const QString &msg) {
        emit logMessage(QStringLiteral("[华沿] 错误：%1").arg(msg));
    });
}

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

    m_huayanScheduler->setConnectionParams(m_cfg.huayanIP, m_cfg.huayanPort);
}

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

void DeviceManager::debugReadRobotRegisters(int addr, int count)
{
    m_robotCtrl->readHoldingRegisters(addr, count);
}

void DeviceManager::debugWriteRobotRegister(int addr, quint16 value)
{
    emit logMessage(QString("[机械臂] 写入寄存器 %1 = %2").arg(addr).arg(value));
    m_robotCtrl->writeRegister(addr, value);
}

void DeviceManager::applyHandEyeMatrix(const float m[16])
{
    m_visionClient->setHandEyeMatrix(m);
    emit logMessage(QStringLiteral("[手眼] 手眼矩阵已更新，新矩阵将立即生效"));
    emit handEyeMatrixApplied();
}

// 同步阻塞，仅用于手动测试按钮，不用于自动流程
bool DeviceManager::tcpPing(const QString &ip, int port, int timeoutMs)
{
    QTcpSocket sock;
    sock.connectToHost(ip, static_cast<quint16>(port));
    const bool ok = sock.waitForConnected(timeoutMs); // 阻塞等待
    if (ok) sock.disconnectFromHost();                // 握手成功后立即断开（只测试可达性）
    return ok;
}

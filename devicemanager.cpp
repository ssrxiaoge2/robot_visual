#include "devicemanager.h"
#include "robotcontroller.h"
#include "agvcontroller.h"

#include <QTcpSocket>
#ifdef Q_OS_LINUX
#  include <QProcess>
#endif

DeviceManager::DeviceManager(QObject *parent)
    : QObject(parent)
{
    // ── 机械臂控制器 ─────────────────────────────────────────
    m_robotCtrl = new RobotController(this);
    connect(m_robotCtrl, &RobotController::connected,
            this, &DeviceManager::robotModbusConnected);
    connect(m_robotCtrl, &RobotController::disconnected,
            this, &DeviceManager::robotModbusDisconnected);
    connect(m_robotCtrl, &RobotController::errorOccurred,
            this, &DeviceManager::robotModbusError);

    // ── AGV 控制器 ────────────────────────────────────────────
    m_agvCtrl = new AgvController(this);
    connect(m_agvCtrl, &AgvController::connected,
            this, &DeviceManager::agvModbusConnected);
    connect(m_agvCtrl, &AgvController::disconnected,
            this, &DeviceManager::agvModbusDisconnected);
    connect(m_agvCtrl, &AgvController::errorOccurred,
            this, &DeviceManager::agvModbusError);
}

// ── 配置应用 ────────────────────────────────────────────────

void DeviceManager::applyConfig()
{
    if (m_cfg.robotIP.isEmpty() || m_cfg.agvIP.isEmpty()) {
        emit logMessage(QStringLiteral("[配置] IP 不能为空"));
        return;
    }

    emit logMessage(QString("配置已更新 → 机器人 %1:%2  AGV %3:%4")
                        .arg(m_cfg.robotIP).arg(m_cfg.robotPort)
                        .arg(m_cfg.agvIP).arg(m_cfg.agvPort));
    emit configApplied(m_cfg.robotIP, m_cfg.robotPort, m_cfg.agvIP);

    // 重连机械臂
    m_robotCtrl->disconnectFromHost();
    m_robotCtrl->connectToHost(m_cfg.robotIP, m_cfg.robotPort);

    // 重连 AGV
    m_agvCtrl->disconnectFromHost();
    m_agvCtrl->connectToHost(m_cfg.agvIP, m_cfg.agvPort);
}

// ── 连通性测试 ──────────────────────────────────────────────

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
        emit logMessage(QStringLiteral("[相机] IP 为空"));
        return;
    }
    emit logMessage(QString("[相机] 正在连接网络 %1:8090 ...").arg(m_cfg.cameraIP));
    emit cameraStatusChanged(false, QStringLiteral("连接中..."));

    if (tcpPing(m_cfg.cameraIP, 8090)) {
        emit logMessage(QString("[相机] %1:8090 端口可达 ✓").arg(m_cfg.cameraIP));
        emit cameraStatusChanged(true, QString("网络可达 %1").arg(m_cfg.cameraIP));
    } else {
        emit logMessage(QString("[相机] %1:8090 连接失败 ✗").arg(m_cfg.cameraIP));
        emit cameraStatusChanged(false, QStringLiteral("网络不可达"));
    }
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

// ── 补光灯控制 ──────────────────────────────────────────────

void DeviceManager::toggleLight()
{
    m_lightOn = !m_lightOn;

#ifdef Q_OS_LINUX
    // Linux 部署环境：调用 GPIO 控制程序
    const QString lightBin = QStringLiteral(PROJECT_SOURCE_DIR "/fill_light");
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
    // 非 Linux 平台（Windows 开发环境）：模拟切换，不调用硬件
    emit logMessage(QString("[补光灯] 模拟%1（当前平台不支持 GPIO 控制）")
                        .arg(m_lightOn ? "开启" : "关闭"));
    emit lightChanged(m_lightOn, true);
#endif
}

// ── 内部工具 ────────────────────────────────────────────────

bool DeviceManager::tcpPing(const QString &ip, int port, int timeoutMs)
{
    QTcpSocket sock;
    sock.connectToHost(ip, static_cast<quint16>(port));
    const bool ok = sock.waitForConnected(timeoutMs);
    if (ok) sock.disconnectFromHost();
    return ok;
}

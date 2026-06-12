#include "devicemanager.h"
#include "agvcontroller.h"
#include "visionclient.h"
#include "huayanScheduler.h"
#include "lineorchestrator.h"

#include <QSettings>
#include <QTcpSocket>
#ifdef Q_OS_LINUX
#  include <QProcess>  // fill_light GPIO 控制（仅 Linux 编译）
#endif

// QSettings 组织/应用名与映射键（值格式 "工位:站点,工位:站点"，如 "2:3,5:7"）
static const char *kSettingsOrg  = "wh-robot";
static const char *kSettingsApp  = "robot-visual";
static const char *kStationMapKey = "agv/stationMap";

DeviceManager::DeviceManager(QObject *parent)
    : QObject(parent)
{
    m_agvCtrl = new AgvController(this);
    connect(m_agvCtrl, &AgvController::connected,
            this, &DeviceManager::agvModbusConnected);
    connect(m_agvCtrl, &AgvController::disconnected,
            this, &DeviceManager::agvModbusDisconnected);
    connect(m_agvCtrl, &AgvController::errorOccurred,
            this, &DeviceManager::agvModbusError);
    // 连接成功后控制器自动读 [3x]00043，确认控制权未被外部调度系统抢占
    connect(m_agvCtrl, &AgvController::controlOwnershipRead, this, [this](bool seized) {
        emit logMessage(seized
            ? QStringLiteral("[AGV] 控制权已被外部抢占（[3x]00043=1），Modbus 指令可能无效")
            : QStringLiteral("[AGV] 控制权未被外部抢占（[3x]00043=0）✓"));
    });

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
    connect(m_visionClient, &VisionHttpClient::noObjectDetected,
            this, [this] {
        emit logMessage(QStringLiteral("[视觉] 未检测到目标，停止调度"));
        m_huayanScheduler->stop();
    });
    connect(m_visionClient, &VisionHttpClient::errorOccurred,
            this, [this](const QString &msg) {
        emit logMessage(QStringLiteral("[视觉] 推理错误：%1，停止调度").arg(msg));
        m_huayanScheduler->stop();
    });

    connect(m_huayanScheduler, &HuayanScheduler::logMessage,
            this, &DeviceManager::logMessage);
    connect(m_huayanScheduler, &HuayanScheduler::stageError,
            this, [this](const QString &msg) {
        emit logMessage(QStringLiteral("[华沿] 错误：%1").arg(msg));
    });

    m_lineOrch = new LineOrchestrator(m_agvCtrl, m_huayanScheduler, this);
    // 编排器请求派单 → 经映射表解析后下发（复用 dispatchAgv）
    connect(m_lineOrch, &LineOrchestrator::agvDispatchRequested,
            this, &DeviceManager::dispatchAgv);
    connect(m_lineOrch, &LineOrchestrator::lineLog,
            this, &DeviceManager::logMessage);
    connect(m_lineOrch, &LineOrchestrator::lineError, this, [this](const QString &msg) {
        emit logMessage(QStringLiteral("[整线错误] %1").arg(msg));
    });

    loadStationMap();
}

void DeviceManager::applyConfig()
{
    if (m_cfg.robotIP.isEmpty() || m_cfg.agvIP.isEmpty()) {
        emit logMessage(QStringLiteral("[配置] 机器人/AGV IP 不能为空"));
        return;
    }

    emit logMessage(QString("配置已更新 → 机械臂 %1  AGV %2:%3  视觉 %4:%5")
                        .arg(m_cfg.robotIP)
                        .arg(m_cfg.agvIP).arg(m_cfg.agvPort)
                        .arg(m_cfg.cameraIP).arg(m_cfg.cameraPort));
    emit configApplied(m_cfg.robotIP, m_cfg.agvIP);

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
        emit logMessage(QStringLiteral("[测试] 机械臂 IP 为空"));
        return;
    }
    emit logMessage(QString("[测试] 正在连接机械臂 SDK %1:%2 ...")
                        .arg(m_cfg.robotIP).arg(m_cfg.huayanPort));
    emit robotStatusChanged(false, QStringLiteral("测试中..."));

    if (tcpPing(m_cfg.robotIP, m_cfg.huayanPort)) {
        emit logMessage(QString("[测试] 机械臂 SDK %1:%2 连接成功 ✓")
                            .arg(m_cfg.robotIP).arg(m_cfg.huayanPort));
        emit robotStatusChanged(true, QString("可达 %1").arg(m_cfg.robotIP));
    } else {
        emit logMessage(QString("[测试] 机械臂 SDK %1:%2 连接失败 ✗")
                            .arg(m_cfg.robotIP).arg(m_cfg.huayanPort));
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

void DeviceManager::loadStationMap()
{
    m_stationMap.clear();
    QSettings settings(kSettingsOrg, kSettingsApp);
    const QString raw = settings.value(kStationMapKey).toString();
    for (const QString &entry : raw.split(',', Qt::SkipEmptyParts)) {
        const QStringList kv = entry.split(':');
        if (kv.size() != 2) continue;
        bool okW = false, okS = false;
        const int w = kv[0].toInt(&okW);
        const int s = kv[1].toInt(&okS);
        if (okW && okS && w > 0 && s > 0 && w <= 65535 && s <= 65535)
            m_stationMap.insert(w, s);
    }
}

void DeviceManager::saveStationMap() const
{
    QStringList entries;
    for (auto it = m_stationMap.constBegin(); it != m_stationMap.constEnd(); ++it)
        entries << QString("%1:%2").arg(it.key()).arg(it.value());
    QSettings settings(kSettingsOrg, kSettingsApp);
    settings.setValue(kStationMapKey, entries.join(','));
}

void DeviceManager::setStationMap(const QHash<int, int> &map)
{
    m_stationMap = map;
    saveStationMap();
}

int DeviceManager::resolveStation(int workstation) const
{
    return m_stationMap.value(workstation, workstation);
}

void DeviceManager::dispatchAgv(int workstation)
{
    const int station = resolveStation(workstation);
    emit logMessage(QStringLiteral("[AGV] 派单：工位 %1 → 站点 %2").arg(workstation).arg(station));
    m_agvCtrl->sendToStation(station);
}

void DeviceManager::cancelAgvNav()
{
    m_agvCtrl->cancelNavigation();
    emit logMessage(QStringLiteral("[AGV] 已发送取消导航"));
}

void DeviceManager::pauseAgvNav()
{
    m_agvCtrl->pauseNavigation();
    emit logMessage(QStringLiteral("[AGV] 已发送暂停导航"));
}

void DeviceManager::resumeAgvNav()
{
    m_agvCtrl->resumeNavigation();
    emit logMessage(QStringLiteral("[AGV] 已发送继续导航"));
}
